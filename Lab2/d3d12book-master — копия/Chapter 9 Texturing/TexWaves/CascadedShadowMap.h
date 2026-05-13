//***************************************************************************************
// CascadedShadowMap.h
//***************************************************************************************

#pragma once

#include "../../Common/d3dUtil.h"

class CascadedShadowMap
{
public:
	CascadedShadowMap(ID3D12Device* device, UINT width, UINT height, UINT cascadeCount);

	CascadedShadowMap(const CascadedShadowMap& rhs) = delete;
	CascadedShadowMap& operator=(const CascadedShadowMap& rhs) = delete;
	~CascadedShadowMap() = default;

	UINT Width()const;
	UINT Height()const;
	UINT CascadeCount()const;
	ID3D12Resource* Resource();
	CD3DX12_GPU_DESCRIPTOR_HANDLE Srv()const;
	CD3DX12_CPU_DESCRIPTOR_HANDLE Dsv(UINT cascadeIndex)const;

	D3D12_VIEWPORT Viewport()const;
	D3D12_RECT ScissorRect()const;

	void BuildDescriptors(
		CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv,
		CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv,
		CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuDsv,
		UINT dsvDescriptorSize);

	void OnResize(UINT newWidth, UINT newHeight);

private:
	void BuildDescriptors();
	void BuildResource();

private:
	ID3D12Device* md3dDevice = nullptr;

	D3D12_VIEWPORT mViewport;
	D3D12_RECT mScissorRect;

	UINT mWidth = 0;
	UINT mHeight = 0;
	UINT mCascadeCount = 0;
	DXGI_FORMAT mFormat = DXGI_FORMAT_R32_TYPELESS;

	CD3DX12_CPU_DESCRIPTOR_HANDLE mhCpuSrv;
	CD3DX12_GPU_DESCRIPTOR_HANDLE mhGpuSrv;
	CD3DX12_CPU_DESCRIPTOR_HANDLE mhCpuDsv;
	UINT mDsvDescriptorSize = 0;

	Microsoft::WRL::ComPtr<ID3D12Resource> mShadowMap = nullptr;
};
