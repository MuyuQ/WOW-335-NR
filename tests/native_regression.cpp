#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "../native/live_params.hpp"
#include "../native/screenshot_readback.hpp"
#include "../native/pending_save.hpp"
static bool fail_read=false,short_write=false,fail_replace=false;
static BOOL ReadForTest(HANDLE h,void *p,DWORD n,DWORD *done,OVERLAPPED *o) {
    if (fail_read) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return ReadFile(h,p,n,done,o);
}
static BOOL WriteForTest(HANDLE h,const void *p,DWORD n,DWORD *done,OVERLAPPED *o) {
    return WriteFile(h,p,short_write && n ? n-1 : n,done,o);
}
static BOOL ReplaceForTest(LPCWSTR p,LPCWSTR replacement,LPCWSTR backup,DWORD flags,void *a,void *b) {
    if (fail_replace) { SetLastError(ERROR_SHARING_VIOLATION); return FALSE; }
    return ReplaceFileW(p,replacement,backup,flags,a,b);
}
#define ReadFile ReadForTest
#define WriteFile WriteForTest
#define ReplaceFileW ReplaceForTest
#include "../native/atomic_text.hpp"
#undef ReadFile
#undef WriteFile
#undef ReplaceFileW
using Microsoft::WRL::ComPtr;
static void Check(bool ok,const char *why) { if (!ok) throw std::runtime_error(why); }
static void Hr(HRESULT hr) { Check(SUCCEEDED(hr),"D3D12 operation failed"); }
static std::string Read(const std::filesystem::path &p) {
    std::ifstream f(p,std::ios::binary);
    return {std::istreambuf_iterator<char>(f),{}};
}
static void Parser() {
    const std::string prefix=R"({"model":2,"intensity":)", suffix=R"(,"localTone":1,"localStructure":1,"skinStructure":-1,"neuralRendering":true,"frameGeneration":true,"fgMultiplier":4})";
    nr_params::Values v;
    Check(nr_params::Parse(prefix+"0.75"+suffix,v) && v.multiplier==4 && v.fg,"valid params rejected");
    for (const char *bad : {"nan","NaN","Infinity","1e999","01","+1","0.5junk","1.",".5"}) {
        v.intensity=0.25f;
        Check(!nr_params::Parse(prefix+bad+suffix,v) && v.intensity==0.25f,"invalid params mutated output");
    }
    const auto good=prefix+"0.75"+suffix;
    Check(!nr_params::Parse(good+"x",v),"trailing garbage accepted");
    Check(!nr_params::Parse(good.substr(0,good.size()-1),v),"truncated object accepted");
    auto dup=good; dup.insert(1,"\"model\":1,");
    Check(!nr_params::Parse(dup,v),"duplicate key accepted");
    auto bad_model=good;bad_model.replace(bad_model.find("2"),1,"1e20");
    Check(!nr_params::Parse(bad_model,v),"unsafe integer conversion allowed");
    nr_files::PendingSave pending;
    pending.dirty=true;
    Check(!pending.Try([] {return false;}) && pending.dirty && pending.failed && pending.IntervalMs()==1000,
          "failed save lost pending request");
    Check(pending.Try([] {return true;}) && !pending.dirty && !pending.failed,"pending request did not recover");
    std::cout << "PASS strict parameters, multiplier roundtrip and unchanged state on rejection\n";
}
static void AtomicText(const std::filesystem::path &dir) {
    const auto path=dir/L"synthetic-Config.wtf";
    const std::string original="SET accountName \"fixture\"\r\nSET gxResolution \"1920x1080\"\r\n";
    {std::ofstream f(path,std::ios::binary);f<<original;}
    auto edit=[](std::string &s) { s+="SET MaxFPS \"30\"\r\n";return true; };
    fail_read=true;
    Check(!nr_files::EditText(path.wstring(),edit) && Read(path)==original,"read failure lost data");
    fail_read=false;short_write=true;
    Check(!nr_files::EditText(path.wstring(),edit) && Read(path)==original,"short write lost data");
    short_write=false;fail_replace=true;
    Check(!nr_files::EditText(path.wstring(),edit) && Read(path)==original,"replace failure lost data");
    fail_replace=false;
    Check(nr_files::EditText(path.wstring(),edit),"successful atomic edit failed");
    Check(Read(path)==original+"SET MaxFPS \"30\"\r\n","successful edit damaged settings");
    bool backup=false;
    for (const auto &p:std::filesystem::directory_iterator(dir))
        if(p.path().extension()==L".bak" && Read(p.path())==original) backup=true;
    Check(backup,"original backup missing");
    std::cout << "PASS read/short-write/replace failures preserve original; success preserves other settings and backup\n";
}
static void Readback() {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    else std::cout << "NOTE D3D12 debug layer unavailable\n";
    ComPtr<ID3D12Device> d;Hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)));
    D3D12_COMMAND_QUEUE_DESC qd={};ComPtr<ID3D12CommandQueue> q;Hr(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));
    ComPtr<ID3D12Fence> fence;Hr(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    UINT64 tick=0;
    for(auto fmt:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R10G10B10A2_UNORM}) {
        D3D12_RESOURCE_DESC desc={};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=63;desc.Height=5;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.Format=fmt;desc.SampleDesc.Count=1;
        ComPtr<ID3D12Resource> rb;D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp={};UINT64 bytes=0;
        Hr(nr_screenshot::CreateReadback(d.Get(),desc,rb,fp,bytes));
        Check(rb->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER,"readback is not buffer");
        Check(fp.Footprint.RowPitch>=63*4 && fp.Footprint.RowPitch%256==0,"invalid row pitch");
        D3D12_HEAP_PROPERTIES heap={};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> texture;Hr(d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&texture)));
        heap.Type=D3D12_HEAP_TYPE_UPLOAD;auto buffer_desc=rb->GetDesc();ComPtr<ID3D12Resource> upload;
        Hr(d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer_desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)));
        unsigned char *mapped=nullptr;Hr(upload->Map(0,nullptr,reinterpret_cast<void**>(&mapped)));
        for(UINT y=0;y<5;++y) for(UINT x=0;x<63*4;++x) mapped[y*fp.Footprint.RowPitch+x]=static_cast<unsigned char>((x+y*17)%251);
        upload->Unmap(0,nullptr);
        ComPtr<ID3D12CommandAllocator> allocator;Hr(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list;Hr(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
        D3D12_TEXTURE_COPY_LOCATION from={},to={};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=fp;
        to.pResource=texture.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        D3D12_RESOURCE_BARRIER barrier={};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource=texture.Get();barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1,&barrier);
        nr_screenshot::CopyToReadback(list.Get(),texture.Get(),rb.Get(),fp);Hr(list->Close());
        ID3D12CommandList *lists[]={list.Get()};q->ExecuteCommandLists(1,lists);Hr(q->Signal(fence.Get(),++tick));
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(event!=nullptr,"event creation failed");
        Hr(fence->SetEventOnCompletion(tick,event));Check(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"GPU readback timed out");CloseHandle(event);
        Hr(rb->Map(0,nullptr,reinterpret_cast<void**>(&mapped)));
        for(UINT y=0;y<5;++y) for(UINT x=0;x<63*4;++x) Check(mapped[y*fp.Footprint.RowPitch+x]==static_cast<unsigned char>((x+y*17)%251),"pixel mismatch");
        rb->Unmap(0,nullptr);
    }
    std::cout<<"PASS GPU texture readback: RGBA8/BGRA8/10-bit, odd width, all pixel bytes\n";
}
int wmain(int argc,wchar_t **argv) {
    try {Check(argc==2,"test output required");Parser();AtomicText(argv[1]);Readback();return 0;}
    catch(const std::exception &e) {std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
