#include "ObjModelLoader.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <algorithm>

using namespace DirectX;

std::string ObjModelLoader::Trim(const std::string& s)
{
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])))
        ++begin;

    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])))
        --end;

    return s.substr(begin, end - begin);
}

std::vector<std::string> ObjModelLoader::Split(const std::string& s)
{
    std::istringstream iss(s);
    std::vector<std::string> parts;
    std::string part;
    while (iss >> part)
        parts.push_back(part);
    return parts;
}

int ObjModelLoader::ResolveObjIndex(int objIndex, int count)
{
    if (objIndex > 0)
        return objIndex - 1;

    if (objIndex < 0)
        return count + objIndex;

    return -1;
}

ObjModelLoader::ObjIndex ObjModelLoader::ParseObjIndexToken(const std::string& token)
{
    ObjIndex idx;

    size_t p1 = token.find('/');
    if (p1 == std::string::npos)
    {
        idx.v = std::stoi(token);
        return idx;
    }

    size_t p2 = token.find('/', p1 + 1);

    std::string s0 = token.substr(0, p1);
    std::string s1 = (p2 == std::string::npos) ? token.substr(p1 + 1) : token.substr(p1 + 1, p2 - p1 - 1);
    std::string s2 = (p2 == std::string::npos) ? "" : token.substr(p2 + 1);

    if (!s0.empty()) idx.v = std::stoi(s0);
    if (!s1.empty()) idx.vt = std::stoi(s1);
    if (!s2.empty()) idx.vn = std::stoi(s2);

    return idx;
}

void ObjModelLoader::LoadMtlFile(
    const std::filesystem::path& mtlPath,
    std::unordered_map<std::string, MaterialInfo>& outMaterials)
{
    std::ifstream fin(mtlPath);
    if (!fin.is_open())
        return;

    MaterialInfo* current = nullptr;
    std::string line;

    while (std::getline(fin, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == '#')
            continue;

        auto parts = Split(line);
        if (parts.empty())
            continue;

        const std::string& cmd = parts[0];

        if (cmd == "newmtl")
        {
            if (parts.size() < 2)
                continue;

            MaterialInfo mat;
            mat.Name = parts[1];
            outMaterials[mat.Name] = mat;
            current = &outMaterials[mat.Name];
        }
        else if (cmd == "Kd" && current && parts.size() >= 4)
        {
            current->Kd = XMFLOAT3(
                std::stof(parts[1]),
                std::stof(parts[2]),
                std::stof(parts[3]));
        }
        else if (cmd == "Ns" && current && parts.size() >= 2)
        {
            current->Ns = std::stof(parts[1]);
        }
        else if (cmd == "map_Kd" && current && parts.size() >= 2)
        {
            // «абираем остаток строки после map_Kd, чтобы корректно пережить пробелы в пути
            size_t pos = line.find("map_Kd");
            std::string tex = Trim(line.substr(pos + 6));
            current->DiffuseMap = tex;
        }
    }
}

ObjModelLoader::ModelData ObjModelLoader::LoadFromFile(const std::filesystem::path& objPath)
{
    std::ifstream fin(objPath);
    if (!fin.is_open())
        throw std::runtime_error("Cannot open OBJ file: " + objPath.string());

    std::vector<XMFLOAT3> positions;
    std::vector<XMFLOAT2> texcoords;
    std::vector<XMFLOAT3> normals;

    ModelData model;

    std::unordered_map<ObjIndex, uint32_t, ObjIndexHasher> vertexLut;

    struct TempSubmesh
    {
        std::string MaterialName;
        std::vector<uint32_t> Indices;
    };

    std::vector<TempSubmesh> tempSubmeshes;
    tempSubmeshes.push_back({});
    tempSubmeshes.back().MaterialName = "default";

    int currentSubmesh = 0;

    std::string line;
    while (std::getline(fin, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == '#')
            continue;

        auto parts = Split(line);
        if (parts.empty())
            continue;

        const std::string& cmd = parts[0];

        if (cmd == "v")
        {
            if (parts.size() < 4) continue;
            positions.emplace_back(
                std::stof(parts[1]),
                std::stof(parts[2]),
                std::stof(parts[3]));
        }
        else if (cmd == "vt")
        {
            if (parts.size() < 3) continue;
            texcoords.emplace_back(
                std::stof(parts[1]),
                1.0f - std::stof(parts[2])); // переворачиваем V под D3D
        }
        else if (cmd == "vn")
        {
            if (parts.size() < 4) continue;
            normals.emplace_back(
                std::stof(parts[1]),
                std::stof(parts[2]),
                std::stof(parts[3]));
        }
        else if (cmd == "mtllib")
        {
            if (parts.size() < 2) continue;
            model.MtlFile = parts[1];
            auto mtlPath = objPath.parent_path() / model.MtlFile;
            LoadMtlFile(mtlPath, model.Materials);
        }
        else if (cmd == "usemtl")
        {
            std::string matName = (parts.size() >= 2) ? parts[1] : "default";

            bool found = false;
            for (size_t i = 0; i < tempSubmeshes.size(); ++i)
            {
                if (tempSubmeshes[i].MaterialName == matName && tempSubmeshes[i].Indices.empty())
                {
                    currentSubmesh = static_cast<int>(i);
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                tempSubmeshes.push_back({});
                tempSubmeshes.back().MaterialName = matName;
                currentSubmesh = static_cast<int>(tempSubmeshes.size()) - 1;
            }
        }
        else if (cmd == "f")
        {
            if (parts.size() < 4)
                continue;

            std::vector<uint32_t> polyIndices;
            polyIndices.reserve(parts.size() - 1);

            for (size_t i = 1; i < parts.size(); ++i)
            {
                ObjIndex raw = ParseObjIndexToken(parts[i]);

                ObjIndex key;
                key.v = ResolveObjIndex(raw.v, static_cast<int>(positions.size()));
                key.vt = ResolveObjIndex(raw.vt, static_cast<int>(texcoords.size()));
                key.vn = ResolveObjIndex(raw.vn, static_cast<int>(normals.size()));

                auto it = vertexLut.find(key);
                if (it == vertexLut.end())
                {
                    Vertex v{};

                    if (key.v >= 0 && key.v < (int)positions.size())
                        v.Pos = positions[key.v];

                    if (key.vt >= 0 && key.vt < (int)texcoords.size())
                        v.TexC = texcoords[key.vt];

                    if (key.vn >= 0 && key.vn < (int)normals.size())
                        v.Normal = normals[key.vn];
                    else
                        v.Normal = XMFLOAT3(0.0f, 1.0f, 0.0f);

                    uint32_t newIndex = static_cast<uint32_t>(model.Vertices.size());
                    model.Vertices.push_back(v);
                    vertexLut[key] = newIndex;
                    polyIndices.push_back(newIndex);
                }
                else
                {
                    polyIndices.push_back(it->second);
                }
            }

            // fan triangulation
            for (size_t i = 1; i + 1 < polyIndices.size(); ++i)
            {
                tempSubmeshes[currentSubmesh].Indices.push_back(polyIndices[0]);
                tempSubmeshes[currentSubmesh].Indices.push_back(polyIndices[i]);
                tempSubmeshes[currentSubmesh].Indices.push_back(polyIndices[i + 1]);
            }
        }
    }

    uint32_t runningStart = 0;
    for (const auto& ts : tempSubmeshes)
    {
        if (ts.Indices.empty())
            continue;

        SubmeshInfo sm;
        sm.Name = ts.MaterialName.empty() ? "default" : ts.MaterialName;
        sm.MaterialName = sm.Name;
        sm.StartIndexLocation = runningStart;
        sm.IndexCount = static_cast<uint32_t>(ts.Indices.size());
        sm.BaseVertexLocation = 0;

        model.Indices32.insert(model.Indices32.end(), ts.Indices.begin(), ts.Indices.end());
        model.Submeshes.push_back(sm);

        runningStart += sm.IndexCount;
    }

    return model;
}