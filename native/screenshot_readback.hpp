#pragma once
#include <d3d12.h>
#include <wrl/client.h>

namespace nr_screenshot {
inline HRESULT CreateReadback(ID3D12Device *device, const D3D12_RESOURCE_DESC &texture,
    Microsoft::WRL::ComPtr<ID3D12Resource> &buffer,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint, UINT64 &bytes) {
    device->GetCopyableFootprints(&texture,0,1,0,&footprint,nullptr,nullptr,&bytes);
    D3D12_HEAP_PROPERTIES heap={};
    heap.Type=D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc={};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width=bytes; desc.Height=1; desc.DepthOrArraySize=1; desc.MipLevels=1;
    desc.SampleDesc.Count=1; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&buffer));
}
inline void CopyToReadback(ID3D12GraphicsCommandList *list, ID3D12Resource *texture,
    ID3D12Resource *buffer, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint) {
    D3D12_TEXTURE_COPY_LOCATION from={};
    from.pResource=texture; from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION to={};
    to.pResource=buffer; to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint=footprint;
    list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
}
}
