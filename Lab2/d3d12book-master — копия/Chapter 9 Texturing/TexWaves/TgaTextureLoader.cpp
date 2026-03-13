#include "TgaTextureLoader.h"
#include "../../Common/d3dUtil.h"
#include "../../Common/d3dx12.h"

#include <vector>
#include <fstream>
#include <stdexcept>
#include <cstdint>

using Microsoft::WRL::ComPtr;

#pragma pack(push, 1)
struct TGAHeader
{
    uint8_t  idLength;
    uint8_t  colorMapType;
    uint8_t  imageType;
    uint16_t colorMapFirstEntry;
    uint16_t colorMapLength;
    uint8_t  colorMapEntrySize;
    uint16_t xOrigin;
    uint16_t yOrigin;
    uint16_t width;
    uint16_t height;
    uint8_t  pixelDepth;
    uint8_t  imageDescriptor;
};
#pragma pack(pop)

static void DecodeTGA(
    const std::wstring& filename,
    std::vector<uint8_t>& outRGBA,
    UINT& outWidth,
    UINT& outHeight)
{
    std::ifstream fin(filename, std::ios::binary);
    if (!fin.is_open())
        throw std::runtime_error("Cannot open TGA file.");

    TGAHeader hdr{};
    fin.read(reinterpret_cast<char*>(&hdr), sizeof(TGAHeader));

    if (!fin)
        throw std::runtime_error("Failed to read TGA header.");

    if (hdr.colorMapType != 0)
        throw std::runtime_error("Color mapped TGA is not supported.");

    if (!(hdr.imageType == 2 || hdr.imageType == 10))
        throw std::runtime_error("Only uncompressed/RLE true-color TGA is supported.");

    if (!(hdr.pixelDepth == 24 || hdr.pixelDepth == 32))
        throw std::runtime_error("Only 24-bit and 32-bit TGA is supported.");

    if (hdr.idLength > 0)
        fin.seekg(hdr.idLength, std::ios::cur);

    outWidth = hdr.width;
    outHeight = hdr.height;

    const int bytesPerPixel = hdr.pixelDepth / 8;
    const size_t pixelCount = static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight);

    std::vector<uint8_t> raw(pixelCount * bytesPerPixel);

    if (hdr.imageType == 2)
    {
        fin.read(reinterpret_cast<char*>(raw.data()), raw.size());
        if (!fin)
            throw std::runtime_error("Failed to read uncompressed TGA pixel data.");
    }
    else
    {
        size_t currentPixel = 0;
        while (currentPixel < pixelCount)
        {
            uint8_t packetHeader = 0;
            fin.read(reinterpret_cast<char*>(&packetHeader), 1);
            if (!fin)
                throw std::runtime_error("Failed to read TGA RLE packet header.");

            const uint32_t count = (packetHeader & 0x7F) + 1;

            if (packetHeader & 0x80)
            {
                std::vector<uint8_t> pixel(bytesPerPixel);
                fin.read(reinterpret_cast<char*>(pixel.data()), bytesPerPixel);
                if (!fin)
                    throw std::runtime_error("Failed to read TGA RLE pixel.");

                for (uint32_t i = 0; i < count; ++i)
                {
                    if (currentPixel >= pixelCount)
                        break;

                    std::memcpy(&raw[currentPixel * bytesPerPixel], pixel.data(), bytesPerPixel);
                    ++currentPixel;
                }
            }
            else
            {
                const size_t blockSize = static_cast<size_t>(count) * bytesPerPixel;
                fin.read(reinterpret_cast<char*>(&raw[currentPixel * bytesPerPixel]), blockSize);
                if (!fin)
                    throw std::runtime_error("Failed to read TGA raw packet.");
                currentPixel += count;
            }
        }
    }

    outRGBA.resize(pixelCount * 4);

    const bool topOrigin = (hdr.imageDescriptor & 0x20) != 0;

    for (UINT y = 0; y < outHeight; ++y)
    {
        UINT srcY = topOrigin ? y : (outHeight - 1 - y);

        for (UINT x = 0; x < outWidth; ++x)
        {
            size_t srcIndex = (static_cast<size_t>(srcY) * outWidth + x) * bytesPerPixel;
            size_t dstIndex = (static_cast<size_t>(y) * outWidth + x) * 4;

            uint8_t b = raw[srcIndex + 0];
            uint8_t g = raw[srcIndex + 1];
            uint8_t r = raw[srcIndex + 2];
            uint8_t a = (bytesPerPixel == 4) ? raw[srcIndex + 3] : 255;

            outRGBA[dstIndex + 0] = r;
            outRGBA[dstIndex + 1] = g;
            outRGBA[dstIndex + 2] = b;
            outRGBA[dstIndex + 3] = a;
        }
    }
}

HRESULT CreateTGATextureFromFile12(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const std::wstring& filename,
    ComPtr<ID3D12Resource>& texture,
    ComPtr<ID3D12Resource>& textureUploadHeap)
{
    try
    {
        std::vector<uint8_t> rgbaData;
        UINT width = 0;
        UINT height = 0;

        DecodeTGA(filename, rgbaData, width, height);

        auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R8G8B8A8_UNORM,
            width,
            height,
            1, 1);

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(texture.GetAddressOf())));

        UINT64 uploadBufferSize = GetRequiredIntermediateSize(texture.Get(), 0, 1);

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(textureUploadHeap.GetAddressOf())));

        D3D12_SUBRESOURCE_DATA subResourceData = {};
        subResourceData.pData = rgbaData.data();
        subResourceData.RowPitch = static_cast<LONG_PTR>(width * 4);
        subResourceData.SlicePitch = subResourceData.RowPitch * height;

        UpdateSubresources(cmdList, texture.Get(), textureUploadHeap.Get(), 0, 0, 1, &subResourceData);

        cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
            texture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

        return S_OK;
    }
    catch (...)
    {
        return E_FAIL;
    }
}