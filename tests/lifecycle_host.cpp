#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include <stdexcept>
#include <vector>
#pragma comment(lib, "user32.lib")

// Minimal API double for the SDK used by the actual addon. This is not a GPU
// runtime simulation; only registration/config/log calls are stubbed.
extern "C" __declspec(dllexport) bool ReShadeRegisterAddon(void *, unsigned) { return true; }
extern "C" __declspec(dllexport) void ReShadeUnregisterAddon(void *) {}
extern "C" __declspec(dllexport) const void *ReShadeGetImGuiFunctionTable(unsigned) {
    static unsigned char table[65536] = {};
    return table; // No UI function is called in these tests.
}
extern "C" __declspec(dllexport) bool ReShadeGetConfigValue(void *, void *, const char *, const char *, char *, size_t *) { return false; }
extern "C" __declspec(dllexport) void ReShadeSetConfigValue(void *, void *, const char *, const char *, const char *) {}
extern "C" __declspec(dllexport) void ReShadeLogMessage(void *, int, const char *) {}
extern "C" __declspec(dllexport) void ReShadeRegisterEvent(unsigned, void *) {}
extern "C" __declspec(dllexport) void ReShadeUnregisterEvent(unsigned, void *) {}
extern "C" __declspec(dllexport) void ReShadeRegisterOverlay(const char *, void *) {}
extern "C" __declspec(dllexport) void ReShadeUnregisterOverlay(const char *, void *) {}
static void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
struct TestWork { HANDLE entered, release; };
struct Api {
    HMODULE module;
    bool (*init)(HMODULE, HMODULE);
    void (*uninit)(HMODULE, HMODULE);
    DWORD (WINAPI *shutdown)(DWORD);
    DWORD (*owned_close)();
    BOOL (*queue)(TestWork *), (*worker)(TestWork *), (*gpu)(), (*signal_gpu)(), (*hook)();
    BOOL (*legacy)();
    void (*feature_failure)(BOOL);
    unsigned (*state)();
    int (*call_hook)();
    BOOL (*window_hook)(), (*window_class)();
    int (*trampoline)();
    void (*external_callback)();
    explicit Api(const wchar_t *path) {
        module = LoadLibraryW(path); Check(module != nullptr, "fixture load failed");
#define RESOLVE(field, name) field = reinterpret_cast<decltype(field)>(GetProcAddress(module, name)); Check(field != nullptr, "missing export " name)
        RESOLVE(legacy, "NRTestLegacy"); RESOLVE(feature_failure, "NRTestFeatureFailure");
        RESOLVE(owned_close, "NRTestOwnedClose");
        RESOLVE(init, "AddonInit"); RESOLVE(uninit, "AddonUninit"); RESOLVE(shutdown, "NRShutdown");
        RESOLVE(queue, "NRTestQueue"); RESOLVE(worker, "NRTestWorker"); RESOLVE(gpu, "NRTestGpu");
        RESOLVE(signal_gpu, "NRTestSignalGpu"); RESOLVE(state, "NRTestState"); RESOLVE(hook, "NRTestHook");
        RESOLVE(call_hook, "NRTestCallHook");
        RESOLVE(window_hook, "NRTestWindowHook"); RESOLVE(trampoline, "NRTestTrampoline");
        RESOLVE(window_class, "NRTestWindowClass");
        RESOLVE(external_callback, "NRTestExternalCallback");
#undef RESOLVE
        Check(init(module, GetModuleHandleW(nullptr)), "fixture initialization failed");
    }
    void Unload() {
        Check(state() == 16u, "resources or work remain before unload");
        uninit(module, GetModuleHandleW(nullptr));
        Check(FreeLibrary(module) != FALSE, "caller release failed"); module = nullptr;
        const auto deadline = GetTickCount64() + 3000;
        while (GetModuleHandleW(L"nr-lifecycle-fixture.dll") && GetTickCount64() < deadline) Sleep(1);
        Check(GetModuleHandleW(L"nr-lifecycle-fixture.dll") == nullptr, "module still loaded after complete shutdown");
    }
};
static void Blocked(const wchar_t *path, bool worker) {
    Api api(path);
    TestWork work{CreateEventW(nullptr, TRUE, FALSE, nullptr), CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Check(work.entered && work.release && (worker ? api.worker(&work) : api.queue(&work)), "work setup failed");
    Check(WaitForSingleObject(work.entered, 3000) == WAIT_OBJECT_0, "work did not start");
    const ULONGLONG begin = GetTickCount64();
    Check(api.shutdown(30) == ERROR_TIMEOUT && GetTickCount64()-begin < 1000, "blocked shutdown was not bounded");
    Check((api.state() & (worker ? 12u : 1u)) == (worker ? 12u : 1u), "timeout destroyed live handles/work");
    Check(!api.queue(&work), "closed admission accepted more work");
    Check(FreeLibrary(api.module) != FALSE, "initial caller release failed");
    Check(GetModuleHandleExW(0, L"nr-lifecycle-fixture.dll", &api.module) != FALSE, "timeout failed to retain executable module");
    SetEvent(work.release);
    Check(api.shutdown(3000) == ERROR_SUCCESS && api.state() == 16u, "retry did not complete shutdown");
    api.Unload(); CloseHandle(work.release); CloseHandle(work.entered);
    puts(worker ? "PASS blocked render worker: timeout retains handles/module; retry joins and unloads" :
                  "PASS queued work: timeout, admission closed, module retained, retry and unload");
}
static void Gpu(const wchar_t *path) {
    Api api(path); Check(api.gpu(), "GPU setup failed");
    Check(api.shutdown(30) == ERROR_TIMEOUT && (api.state() & 2u), "GPU timeout released a live resource");
    Check(api.signal_gpu(), "GPU gate signal failed");
    Check(api.shutdown(3000) == ERROR_SUCCESS && api.state() == 16u, "GPU retry did not drain and release");
    api.Unload(); puts("PASS two real D3D12 queues: incomplete fences retain resources; completed fences allow release/unload");
}
struct ConcurrentCall { DWORD (WINAPI *shutdown)(DWORD); DWORD result; };
static DWORD WINAPI CallShutdown(void *p) { auto *call = static_cast<ConcurrentCall *>(p); call->result = call->shutdown(3000); return 0; }
static void Concurrent(const wchar_t *path) {
    Api api(path); TestWork work{CreateEventW(nullptr, TRUE, FALSE, nullptr),CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Check(api.queue(&work) && WaitForSingleObject(work.entered, 3000) == WAIT_OBJECT_0, "concurrent work setup failed");
    ConcurrentCall call{api.shutdown, ERROR_GEN_FAILURE}; HANDLE thread = CreateThread(nullptr,0,CallShutdown,&call,0,nullptr);
    Check(thread != nullptr, "concurrent shutdown thread failed");
    // The first closer holds its lock while draining this known blocked work.
    DWORD result = ERROR_GEN_FAILURE;
    for (int i=0; i<100; ++i) { result=api.shutdown(0); if(result==ERROR_BUSY) break; Sleep(1); }
    Check(result==ERROR_BUSY, "concurrent shutdown was not serialized");
    SetEvent(work.release); Check(WaitForSingleObject(thread,3000)==WAIT_OBJECT_0 && call.result==ERROR_SUCCESS,"concurrent shutdown failed");
    CloseHandle(thread); api.Unload(); CloseHandle(work.release);CloseHandle(work.entered);
    puts("PASS concurrent close requests are serialized");
}
static void Child(const wchar_t *mode, const wchar_t *path) {
    wchar_t self[32768]{}; GetModuleFileNameW(nullptr,self,32768);
    std::wstring command=L"\""+std::wstring(self)+L"\" "+mode+L" \""+path+L"\"";
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    Check(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE,"child start failed");
    CloseHandle(process.hThread);
    const DWORD waited=WaitForSingleObject(process.hProcess,10000);
    if(waited!=WAIT_OBJECT_0) { TerminateProcess(process.hProcess,99);CloseHandle(process.hProcess);throw std::runtime_error("isolated shutdown child timed out"); }
    DWORD code=99;GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hProcess);Check(code==0,"isolated child failed");
}
int wmain(int argc,wchar_t **argv) {
    try {
        Check(argc>=2,"fixture path required");
        if(argc==3 && wcscmp(argv[1],L"--exit-pending")==0) {
            Api api(argv[2]);
            Check(api.gpu(), "pending-exit GPU setup failed");
            // Deliberately exit without uninit while a background worker waits.
            static TestWork work{CreateEventW(nullptr,TRUE,FALSE,nullptr),CreateEventW(nullptr,TRUE,FALSE,nullptr)};
            Check(api.worker(&work) && WaitForSingleObject(work.entered,3000)==WAIT_OBJECT_0,"child worker setup failed");
            return 0;
        }
        if(argc==3 && wcscmp(argv[1],L"--hook")==0) {
            Api api(argv[2]);Check(api.hook() && api.call_hook()==99,"native hook setup failed");
            Check(api.shutdown(3000)==ERROR_SUCCESS && api.call_hook()==42,"native hook was not disabled");
            api.uninit(api.module,GetModuleHandleW(nullptr));FreeLibrary(api.module);
            Check(GetModuleHandleW(L"nr-lifecycle-fixture.dll")!=nullptr,"in-flight native callback code was not retained");return 0;
        }
        if(argc==3 && wcscmp(argv[1],L"--external-callback")==0) {
            Api api(argv[2]);api.external_callback();
            Check(api.shutdown(3000)==ERROR_SUCCESS,"external callback shutdown failed");
            api.uninit(api.module,GetModuleHandleW(nullptr));FreeLibrary(api.module);
            Check(GetModuleHandleW(L"nr-lifecycle-fixture.dll")!=nullptr,"escaped runtime callback code was not retained");return 0;
        }
        if(argc==3 && wcscmp(argv[1],L"--window-hook")==0) {
            Api api(argv[2]);Check(api.window_hook() && api.call_hook()==99,"window hook setup failed");
            Check(api.shutdown(3000)==ERROR_SUCCESS && api.call_hook()==42 && api.trampoline()==42,
                "window hook shutdown removed its original trampoline");
            api.uninit(api.module,GetModuleHandleW(nullptr));FreeLibrary(api.module);
            Check(GetModuleHandleW(L"nr-lifecycle-fixture.dll")!=nullptr,"window callback code was not retained");return 0;
        }
        for(int i=0;i<5;++i) { Api api(argv[1]); Check(api.shutdown(3000)==ERROR_SUCCESS,"normal close failed");Check(api.shutdown(0)==ERROR_SUCCESS,"repeated close failed");api.Unload(); }
        puts("PASS five real DLL load/init/shutdown/uninit/unload cycles and repeated close");
        { Api api(argv[1]); Check(api.owned_close()==ERROR_SUCCESS && api.state()==16u,"device callback rundown counted itself");api.Unload(); }
        puts("PASS owning device callback can close without waiting on its own lease");
        { Api api(argv[1]);Check(api.window_class(),"window class setup failed");
          Check(api.shutdown(3000)==ERROR_SUCCESS,"window class close failed");
          WNDCLASSEXW wc{sizeof(wc)};
          Check(!GetClassInfoExW(api.module,L"StandaloneDLSSNRNativeOutput",&wc),"window callback class remains registered");api.Unload(); }
        puts("PASS output window class removed before module unload");
        Blocked(argv[1],false);Blocked(argv[1],true);Gpu(argv[1]);Concurrent(argv[1]);
        { Api api(argv[1]);Check(api.legacy(),"D3D11 WARP setup failed");Check(api.shutdown(3000)==ERROR_SUCCESS && api.state()==16u,"D3D11 query did not drain");api.Unload(); }
        puts("PASS D3D11 event query drains legacy GPU work before resource release");
        { Api api(argv[1]);Check(api.gpu() && api.signal_gpu(),"release failure GPU setup failed");api.feature_failure(FALSE);
          Check(api.shutdown(3000)==ERROR_INVALID_STATE && (api.state()&2u),"feature release failure discarded resources");
          api.feature_failure(TRUE);Check(api.shutdown(3000)==ERROR_SUCCESS,"feature release retry failed");api.Unload(); }
        puts("PASS injected feature-release failure preserves resources and allows retry");
        Child(L"--exit-pending",argv[1]);puts("PASS process exit with blocked worker and GPU queues does not wait in DllMain");
        Child(L"--hook",argv[1]);puts("PASS native hook disabled; callback module retained through caller FreeLibrary");
        Child(L"--window-hook",argv[1]);puts("PASS window API hook disabled without deleting its original trampoline");
        Child(L"--external-callback",argv[1]);puts("PASS escaped runtime callback keeps code mapped until process exit");
        return 0;
    } catch(const std::exception &e) { fprintf(stderr,"FAIL %s (Win32=%lu)\n",e.what(),GetLastError());return 1; }
}
