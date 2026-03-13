#include "GBuffer.h"

using Microsoft::WRL::ComPtr;

GBuffer::GBuffer(
    ID3D12Device* device,
    UINT width,
    UINT height,
    DXGI_FORMAT rt0Format,
    DXGI_FORMAT rt1Format,
    DXGI_FORMAT rt2Format,
    DXGI_FORMAT depthFormat)
{
    Initialize(device, width, height, rt0Format, rt1Format, rt2Format, depthFormat);
}

void GBuffer::Initialize(
    ID3D12Device* device,
    UINT width,
    UINT height,
    DXGI_FORMAT rt0Format,
    DXGI_FORMAT rt1Format,
    DXGI_FORMAT rt2Format,
    DXGI_FORMAT depthFormat)
{
    md3dDevice = device;
    mWidth = width;
    mHeight = height;

    mRtFormats[0] = rt0Format;
    mRtFormats[1] = rt1Format;
    mRtFormats[2] = rt2Format;
    mDepthFormat = depthFormat;

    BuildResources();
}

void GBuffer::OnResize(UINT newWidth, UINT newHeight)
{
    if (mWidth == newWidth && mHeight == newHeight)
        return;

    mWidth = newWidth;
    mHeight = newHeight;

    for (auto& rt : mRenderTargets)
        rt.Reset();

    mDepthStencilBuffer.Reset();

    BuildResources();
}

void GBuffer::BuildResources()
{
    for (UINT i = 0; i < kBufferCount; ++i)
    {
        auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            mRtFormats[i],
            mWidth,
            mHeight,
            1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

        D3D12_CLEAR_VALUE optClear = {};
        optClear.Format = mRtFormats[i];
        optClear.Color[0] = 0.0f;
        optClear.Color[1] = 0.0f;
        optClear.Color[2] = 0.0f;
        optClear.Color[3] = 0.0f;

        ThrowIfFailed(md3dDevice->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &optClear,
            IID_PPV_ARGS(&mRenderTargets[i])));
    }

    DXGI_FORMAT depthResourceFormat = DXGI_FORMAT_R24G8_TYPELESS;

    auto depthDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        depthResourceFormat,
        mWidth,
        mHeight,
        1, 1, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

    D3D12_CLEAR_VALUE depthClear = {};
    depthClear.Format = mDepthFormat;
    depthClear.DepthStencil.Depth = 1.0f;
    depthClear.DepthStencil.Stencil = 0;

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &depthDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &depthClear,
        IID_PPV_ARGS(&mDepthStencilBuffer)));
}

void GBuffer::BuildDescriptors(
    CD3DX12_CPU_DESCRIPTOR_HANDLE& rtvHandle,
    CD3DX12_CPU_DESCRIPTOR_HANDLE& srvCpuHandle,
    CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle,
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle,
    UINT rtvDescriptorSize,
    UINT srvDescriptorSize)
{
    mRtvCpuStart = rtvHandle;
    mSrvCpuStart = srvCpuHandle;
    mSrvGpuStart = srvGpuHandle;
    mDsvCpu = dsvHandle;
    mRtvDescriptorSize = rtvDescriptorSize;
    mSrvDescriptorSize = srvDescriptorSize;

    // RTV for all GBuffer render targets
    for (UINT i = 0; i < kBufferCount; ++i)
    {
        md3dDevice->CreateRenderTargetView(mRenderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, rtvDescriptorSize);
    }

    // SRV #0 -> RT0
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = mRtFormats[0];
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        md3dDevice->CreateShaderResourceView(mRenderTargets[0].Get(), &srvDesc, srvCpuHandle);
        srvCpuHandle.Offset(1, srvDescriptorSize);
    }

    // SRV #1 -> RT1
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = mRtFormats[1];
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        md3dDevice->CreateShaderResourceView(mRenderTargets[1].Get(), &srvDesc, srvCpuHandle);
        srvCpuHandle.Offset(1, srvDescriptorSize);
    }

    // SRV #2 -> Depth
    mDepthSrvGpu = srvGpuHandle;
    mDepthSrvGpu.Offset(2, srvDescriptorSize);

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Texture2D.MostDetailedMip = 0;
        depthSrvDesc.Texture2D.MipLevels = 1;
        depthSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        md3dDevice->CreateShaderResourceView(mDepthStencilBuffer.Get(), &depthSrvDesc, srvCpuHandle);
        srvCpuHandle.Offset(1, srvDescriptorSize);
    }

    // SRV #3 -> RT2 (reserved for future use)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = mRtFormats[2];
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        md3dDevice->CreateShaderResourceView(mRenderTargets[2].Get(), &srvDesc, srvCpuHandle);
        srvCpuHandle.Offset(1, srvDescriptorSize);
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Format = mDepthFormat;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;

    md3dDevice->CreateDepthStencilView(mDepthStencilBuffer.Get(), &dsvDesc, dsvHandle);
}

void GBuffer::Clear(ID3D12GraphicsCommandList* cmdList, const float clearColor[4])
{
    for (UINT i = 0; i < kBufferCount; ++i)
        cmdList->ClearRenderTargetView(GetRtv(i), clearColor, 0, nullptr);

    cmdList->ClearDepthStencilView(
        GetDsv(),
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f,
        0,
        0,
        nullptr);
}

void GBuffer::SetAsRenderTarget(ID3D12GraphicsCommandList* cmdList)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[kBufferCount];
    for (UINT i = 0; i < kBufferCount; ++i)
        rtvs[i] = GetRtv(i);

    cmdList->OMSetRenderTargets(kBufferCount, rtvs, true, &mDsvCpu);
}

void GBuffer::TransitionToWrite(ID3D12GraphicsCommandList* cmdList)
{
    for (UINT i = 0; i < kBufferCount; ++i)
    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            mRenderTargets[i].Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);

        cmdList->ResourceBarrier(1, &barrier);
    }

    auto depthBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        mDepthStencilBuffer.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);

    cmdList->ResourceBarrier(1, &depthBarrier);
}

void GBuffer::TransitionToRead(ID3D12GraphicsCommandList* cmdList)
{
    for (UINT i = 0; i < kBufferCount; ++i)
    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            mRenderTargets[i].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        cmdList->ResourceBarrier(1, &barrier);
    }

    auto depthBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        mDepthStencilBuffer.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    cmdList->ResourceBarrier(1, &depthBarrier);
}