//***************************************************************************************
// CascadedShadowMap.cpp
//***************************************************************************************

#include "CascadedShadowMap.h"

CascadedShadowMap::CascadedShadowMap(ID3D12Device* device, UINT width, UINT height, UINT cascadeCount)
{
	md3dDevice = device;
	mWidth = width;
	mHeight = height;
	mCascadeCount = cascadeCount;

	mViewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
	mScissorRect = { 0, 0, (int)width, (int)height };

	BuildResource();
}

UINT CascadedShadowMap::Width()const
{
	return mWidth;
}

UINT CascadedShadowMap::Height()const
{
	return mHeight;
}

UINT CascadedShadowMap::CascadeCount()const
{
	return mCascadeCount;
}

ID3D12Resource* CascadedShadowMap::Resource()
{
	return mShadowMap.Get();
}

CD3DX12_GPU_DESCRIPTOR_HANDLE CascadedShadowMap::Srv()const
{
	return mhGpuSrv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE CascadedShadowMap::Dsv(UINT cascadeIndex)const
{
	return CD3DX12_CPU_DESCRIPTOR_HANDLE(mhCpuDsv, cascadeIndex, mDsvDescriptorSize);
}

D3D12_VIEWPORT CascadedShadowMap::Viewport()const
{
	return mViewport;
}

D3D12_RECT CascadedShadowMap::ScissorRect()const
{
	return mScissorRect;
}

void CascadedShadowMap::BuildDescriptors(
	CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv,
	CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv,
	CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuDsv,
	UINT dsvDescriptorSize)
{
	mhCpuSrv = hCpuSrv;
	mhGpuSrv = hGpuSrv;
	mhCpuDsv = hCpuDsv;
	mDsvDescriptorSize = dsvDescriptorSize;

	BuildDescriptors();
}

void CascadedShadowMap::OnResize(UINT newWidth, UINT newHeight)
{
	if ((mWidth != newWidth) || (mHeight != newHeight))
	{
		mWidth = newWidth;
		mHeight = newHeight;

		mViewport = { 0.0f, 0.0f, (float)newWidth, (float)newHeight, 0.0f, 1.0f };
		mScissorRect = { 0, 0, (int)newWidth, (int)newHeight };

		BuildResource();
		BuildDescriptors();
	}
}

void CascadedShadowMap::BuildDescriptors()
{
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Texture2DArray.MostDetailedMip = 0;
	srvDesc.Texture2DArray.MipLevels = 1;
	srvDesc.Texture2DArray.FirstArraySlice = 0;
	srvDesc.Texture2DArray.ArraySize = mCascadeCount;
	srvDesc.Texture2DArray.PlaneSlice = 0;
	srvDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
	md3dDevice->CreateShaderResourceView(mShadowMap.Get(), &srvDesc, mhCpuSrv);

	for (UINT i = 0; i < mCascadeCount; ++i)
	{
		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
		dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
		dsvDesc.Texture2DArray.MipSlice = 0;
		dsvDesc.Texture2DArray.FirstArraySlice = i;
		dsvDesc.Texture2DArray.ArraySize = 1;

		md3dDevice->CreateDepthStencilView(
			mShadowMap.Get(),
			&dsvDesc,
			CD3DX12_CPU_DESCRIPTOR_HANDLE(mhCpuDsv, i, mDsvDescriptorSize));
	}
}

void CascadedShadowMap::BuildResource()
{
	D3D12_RESOURCE_DESC texDesc = {};
	texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texDesc.Alignment = 0;
	texDesc.Width = mWidth;
	texDesc.Height = mHeight;
	texDesc.DepthOrArraySize = (UINT16)mCascadeCount;
	texDesc.MipLevels = 1;
	texDesc.Format = mFormat;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE optClear = {};
	optClear.Format = DXGI_FORMAT_D32_FLOAT;
	optClear.DepthStencil.Depth = 1.0f;
	optClear.DepthStencil.Stencil = 0;

	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&texDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		&optClear,
		IID_PPV_ARGS(&mShadowMap)));
}
