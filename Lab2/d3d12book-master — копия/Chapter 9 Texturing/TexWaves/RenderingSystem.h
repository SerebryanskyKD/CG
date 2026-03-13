#pragma once

#include "GBuffer.h"
#include "../../Common/d3dUtil.h"
#include "../../Common/d3dx12.h"
#include <wrl.h>
#include <unordered_map>
#include <string>

class RenderingSystem
{
public:
    RenderingSystem() = default;
    RenderingSystem(const RenderingSystem&) = delete;
    RenderingSystem& operator=(const RenderingSystem&) = delete;

    void Initialize(
        ID3D12Device* device,
        UINT clientWidth,
        UINT clientHeight,
        DXGI_FORMAT backBufferFormat,
        DXGI_FORMAT depthStencilFormat,
        UINT rtvDescriptorSize,
        UINT dsvDescriptorSize,
        UINT cbvSrvUavDescriptorSize);

    void BuildOffscreenDescriptors(
        ID3D12DescriptorHeap* rtvHeap,
        UINT rtvHeapOffset,
        ID3D12DescriptorHeap* srvHeap,
        UINT srvHeapOffset,
        ID3D12DescriptorHeap* dsvHeap,
        UINT dsvHeapOffset);

    void OnResize(UINT clientWidth, UINT clientHeight);

    GBuffer& GetGBuffer() { return mGBuffer; }
    const GBuffer& GetGBuffer() const { return mGBuffer; }

private:
    ID3D12Device* md3dDevice = nullptr;

    UINT mClientWidth = 1;
    UINT mClientHeight = 1;

    DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT mDepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    UINT mRtvDescriptorSize = 0;
    UINT mDsvDescriptorSize = 0;
    UINT mCbvSrvUavDescriptorSize = 0;

    GBuffer mGBuffer;
};