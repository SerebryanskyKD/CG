#pragma once

#include "../../Common/d3dx12.h"
#include <wrl.h>
#include <string>

HRESULT CreateTGATextureFromFile12(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const std::wstring& filename,
    Microsoft::WRL::ComPtr<ID3D12Resource>& texture,
    Microsoft::WRL::ComPtr<ID3D12Resource>& textureUploadHeap);