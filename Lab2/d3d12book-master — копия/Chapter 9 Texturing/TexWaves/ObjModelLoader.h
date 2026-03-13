#pragma once

#include <DirectXMath.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>

class ObjModelLoader
{
public:
    struct Vertex
    {
        DirectX::XMFLOAT3 Pos = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 Normal = { 0.0f, 1.0f, 0.0f };
        DirectX::XMFLOAT2 TexC = { 0.0f, 0.0f };
    };

    struct MaterialInfo
    {
        std::string Name;
        std::string DiffuseMap;   // map_Kd
        DirectX::XMFLOAT3 Kd = { 1.0f, 1.0f, 1.0f };
        float Ns = 32.0f;
    };

    struct SubmeshInfo
    {
        std::string Name;               // usually material name
        std::string MaterialName;
        uint32_t IndexCount = 0;
        uint32_t StartIndexLocation = 0;
        int32_t BaseVertexLocation = 0;
    };

    struct ModelData
    {
        std::vector<Vertex> Vertices;
        std::vector<uint32_t> Indices32;
        std::vector<SubmeshInfo> Submeshes;
        std::unordered_map<std::string, MaterialInfo> Materials;
        std::string MtlFile;
    };

public:
    static ModelData LoadFromFile(const std::filesystem::path& objPath);

private:
    struct ObjIndex
    {
        int v = -1;
        int vt = -1;
        int vn = -1;

        bool operator==(const ObjIndex& rhs) const
        {
            return v == rhs.v && vt == rhs.vt && vn == rhs.vn;
        }
    };

    struct ObjIndexHasher
    {
        size_t operator()(const ObjIndex& x) const
        {
            size_t h1 = std::hash<int>{}(x.v);
            size_t h2 = std::hash<int>{}(x.vt);
            size_t h3 = std::hash<int>{}(x.vn);

            size_t seed = h1;
            seed ^= h2 + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            seed ^= h3 + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            return seed;
        }
    };

private:
    static void LoadMtlFile(
        const std::filesystem::path& mtlPath,
        std::unordered_map<std::string, MaterialInfo>& outMaterials);

    static ObjIndex ParseObjIndexToken(const std::string& token);
    static int ResolveObjIndex(int objIndex, int count);
    static std::vector<std::string> Split(const std::string& s);
    static std::string Trim(const std::string& s);
};