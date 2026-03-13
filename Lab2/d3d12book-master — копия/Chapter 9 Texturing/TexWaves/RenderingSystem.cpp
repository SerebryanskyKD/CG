#include "RenderingSystem.h"

void RenderingSystem::Initialize(
    ID3D12Device* device,
    UINT clientWidth,
    UINT clientHeight,
    DXGI_FORMAT backBufferFormat,
    DXGI_FORMAT depthStencilFormat,
    UINT rtvDescriptorSize,
    UINT dsvDescriptorSize,
    UINT cbvSrvUavDescriptorSize)
{
    md3dDevice = device;
    mClientWidth = clientWidth;
    mClientHeight = clientHeight;
    mBackBufferFormat = backBufferFormat;
    mDepthStencilFormat = depthStencilFormat;
    mRtvDescriptorSize = rtvDescriptorSize;
    mDsvDescriptorSize = dsvDescriptorSize;
    mCbvSrvUavDescriptorSize = cbvSrvUavDescriptorSize;

    mGBuffer.Initialize(
        md3dDevice,
        mClientWidth,
        mClientHeight,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16_FLOAT,
        mDepthStencilFormat);
}

void RenderingSystem::BuildOffscreenDescriptors(
    ID3D12DescriptorHeap* rtvHeap,
    UINT rtvHeapOffset,
    ID3D12DescriptorHeap* srvHeap,
    UINT srvHeapOffset,
    ID3D12DescriptorHeap* dsvHeap,
    UINT dsvHeapOffset)
{
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
        rtvHeap->GetCPUDescriptorHandleForHeapStart(),
        rtvHeapOffset,
        mRtvDescriptorSize);

    CD3DX12_CPU_DESCRIPTOR_HANDLE srvCpuHandle(
        srvHeap->GetCPUDescriptorHandleForHeapStart(),
        srvHeapOffset,
        mCbvSrvUavDescriptorSize);

    CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle(
        srvHeap->GetGPUDescriptorHandleForHeapStart(),
        srvHeapOffset,
        mCbvSrvUavDescriptorSize);

    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(
        dsvHeap->GetCPUDescriptorHandleForHeapStart(),
        dsvHeapOffset,
        mDsvDescriptorSize);

    mGBuffer.BuildDescriptors(
        rtvHandle,
        srvCpuHandle,
        srvGpuHandle,
        dsvHandle,
        mRtvDescriptorSize,
        mCbvSrvUavDescriptorSize);
}

void RenderingSystem::OnResize(UINT clientWidth, UINT clientHeight)
{
    mClientWidth = clientWidth;
    mClientHeight = clientHeight;
    mGBuffer.OnResize(clientWidth, clientHeight);
}