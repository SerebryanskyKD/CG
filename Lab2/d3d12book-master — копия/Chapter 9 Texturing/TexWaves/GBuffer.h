#pragma once

#include "../../Common/d3dUtil.h"
#include "../../Common/d3dx12.h"
#include <wrl.h>
#include <array>

class GBuffer
{
public:
    static const UINT kBufferCount = 3;

public:
    GBuffer() = default;
    GBuffer(ID3D12Device* device, UINT width, UINT height,
        DXGI_FORMAT rt0Format,
        DXGI_FORMAT rt1Format,
        DXGI_FORMAT rt2Format,
        DXGI_FORMAT depthFormat);

    GBuffer(const GBuffer&) = delete;
    GBuffer& operator=(const GBuffer&) = delete;

    void Initialize(
        ID3D12Device* device,
        UINT width,
        UINT height,
        DXGI_FORMAT rt0Format,
        DXGI_FORMAT rt1Format,
        DXGI_FORMAT rt2Format,
        DXGI_FORMAT depthFormat);

    void OnResize(UINT newWidth, UINT newHeight);

    void BuildResources();
    void BuildDescriptors(
        CD3DX12_CPU_DESCRIPTOR_HANDLE& rtvHandle,
        CD3DX12_CPU_DESCRIPTOR_HANDLE& srvCpuHandle,
        CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle,
        CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle,
        UINT rtvDescriptorSize,
        UINT srvDescriptorSize);

    void Clear(ID3D12GraphicsCommandList* cmdList, const float clearColor[4]);
    void SetAsRenderTarget(ID3D12GraphicsCommandList* cmdList);

    void TransitionToWrite(ID3D12GraphicsCommandList* cmdList);
    void TransitionToRead(ID3D12GraphicsCommandList* cmdList);

    ID3D12Resource* GetBuffer(UINT index) const { return mRenderTargets[index].Get(); }
    ID3D12Resource* GetDepthBuffer() const { return mDepthStencilBuffer.Get(); }

    CD3DX12_GPU_DESCRIPTOR_HANDLE GetDepthSrvGpu() const
    {
        return mDepthSrvGpu;
    }

    CD3DX12_GPU_DESCRIPTOR_HANDLE GetSrvGpu(UINT index) const
    {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(mSrvGpuStart, index, mSrvDescriptorSize);
    }

    CD3DX12_CPU_DESCRIPTOR_HANDLE GetRtv(UINT index) const
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(mRtvCpuStart, index, mRtvDescriptorSize);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE GetDsv() const { return mDsvCpu; }

    UINT Width() const { return mWidth; }
    UINT Height() const { return mHeight; }

    DXGI_FORMAT RtFormat(UINT i) const { return mRtFormats[i]; }
    DXGI_FORMAT DepthFormat() const { return mDepthFormat; }

private:
    ID3D12Device* md3dDevice = nullptr;

    UINT mWidth = 1;
    UINT mHeight = 1;

    UINT mRtvDescriptorSize = 0;
    UINT mSrvDescriptorSize = 0;

    std::array<DXGI_FORMAT, kBufferCount> mRtFormats =
    {
        DXGI_FORMAT_R16G16B16A16_FLOAT,   // albedo/spec
        DXGI_FORMAT_R16G16B16A16_FLOAT,   // normal/roughness
        DXGI_FORMAT_R16G16_FLOAT,         // emissive/metal or reserved
    };

    DXGI_FORMAT mDepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kBufferCount> mRenderTargets;
    Microsoft::WRL::ComPtr<ID3D12Resource> mDepthStencilBuffer;

    CD3DX12_CPU_DESCRIPTOR_HANDLE mRtvCpuStart{};
    CD3DX12_CPU_DESCRIPTOR_HANDLE mSrvCpuStart{};
    CD3DX12_GPU_DESCRIPTOR_HANDLE mSrvGpuStart{};
    CD3DX12_GPU_DESCRIPTOR_HANDLE mDepthSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE mDsvCpu{};
};
