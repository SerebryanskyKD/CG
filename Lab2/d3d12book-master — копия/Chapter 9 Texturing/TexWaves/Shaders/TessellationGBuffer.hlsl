#ifndef NUM_DIR_LIGHTS
    #define NUM_DIR_LIGHTS 3
#endif

#ifndef NUM_POINT_LIGHTS
    #define NUM_POINT_LIGHTS 8
#endif

#ifndef NUM_SPOT_LIGHTS
    #define NUM_SPOT_LIGHTS 4
#endif

#include "LightingUtil.hlsl"

Texture2D gDiffuseMap : register(t0);
Texture2D gNormalMap : register(t1);
Texture2D gDisplacementMap : register(t2);

SamplerState gsamPointWrap : register(s0);
SamplerState gsamPointClamp : register(s1);
SamplerState gsamLinearWrap : register(s2);
SamplerState gsamLinearClamp : register(s3);
SamplerState gsamAnisotropicWrap : register(s4);
SamplerState gsamAnisotropicClamp : register(s5);

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gTexTransform;
};

cbuffer cbPass : register(b1)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float3 gEyePosW;
    float cbPerObjectPad1;
    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;
    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;
    float4 gAmbientLight;
    Light gLights[MaxLights];
};

cbuffer cbMaterial : register(b2)
{
    float4 gDiffuseAlbedo;
    float3 gFresnelR0;
    float gRoughness;
    float4x4 gMatTransform;
    float gDisplacementScale;
    float3 gMaterialPad;
};

struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

struct VertexOut
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    vout.PosL = vin.PosL;
    vout.NormalL = vin.NormalL;
    vout.TexC = vin.TexC;
    return vout;
}

struct PatchTess
{
    float EdgeTess[3] : SV_TessFactor;
    float InsideTess : SV_InsideTessFactor;
};

PatchTess ConstantHS(InputPatch<VertexOut, 3> patch, uint patchID : SV_PrimitiveID)
{
    PatchTess pt;

    float3 centerL = (patch[0].PosL + patch[1].PosL + patch[2].PosL) / 3.0f;
    float3 centerW = mul(float4(centerL, 1.0f), gWorld).xyz;
    float d = distance(centerW, gEyePosW);

    const float d0 = 15.0f;
    const float d1 = 90.0f;
    float tess = lerp(16.0f, 1.0f, saturate((d - d0) / (d1 - d0)));

    pt.EdgeTess[0] = tess;
    pt.EdgeTess[1] = tess;
    pt.EdgeTess[2] = tess;
    pt.InsideTess = tess;

    return pt;
}

struct HullOut
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("ConstantHS")]
[maxtessfactor(16.0f)]
HullOut HS(InputPatch<VertexOut, 3> patch, uint i : SV_OutputControlPointID, uint patchId : SV_PrimitiveID)
{
    HullOut hout;
    hout.PosL = patch[i].PosL;
    hout.NormalL = patch[i].NormalL;
    hout.TexC = patch[i].TexC;
    return hout;
}

struct DomainOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
};

[domain("tri")]
DomainOut DS(PatchTess patchTess, float3 bary : SV_DomainLocation, const OutputPatch<HullOut, 3> tri)
{
    DomainOut dout = (DomainOut)0.0f;

    float3 posL =
        bary.x * tri[0].PosL +
        bary.y * tri[1].PosL +
        bary.z * tri[2].PosL;

    float3 normalL = normalize(
        bary.x * tri[0].NormalL +
        bary.y * tri[1].NormalL +
        bary.z * tri[2].NormalL);

    float2 texC =
        bary.x * tri[0].TexC +
        bary.y * tri[1].TexC +
        bary.z * tri[2].TexC;

    float4 texCoord = mul(float4(texC, 0.0f, 1.0f), gTexTransform);
    texC = mul(texCoord, gMatTransform).xy;

    float height = gDisplacementMap.SampleLevel(gsamLinearWrap, texC, 0).r;
    float displacement = (height - 0.5f) * 2.0f * gDisplacementScale;
    posL += normalL * displacement;

    float4 posW = mul(float4(posL, 1.0f), gWorld);
    dout.PosW = posW.xyz;
    dout.NormalW = normalize(mul(normalL, (float3x3)gWorld));
    dout.TexC = texC;
    dout.PosH = mul(posW, gViewProj);

    return dout;
}

struct GBufferOut
{
    float4 Target0 : SV_Target0;
    float4 Target1 : SV_Target1;
    float2 Target2 : SV_Target2;
};

GBufferOut PS(DomainOut pin)
{
    GBufferOut gout;

    float4 diffuseAlbedo = gDiffuseMap.Sample(gsamAnisotropicWrap, pin.TexC) * gDiffuseAlbedo;
    float3 normalSample = gNormalMap.Sample(gsamAnisotropicWrap, pin.TexC).xyz;
    float3 normalW = NormalSampleToWorldSpace(normalSample, normalize(pin.NormalW), pin.PosW, pin.TexC);

    gout.Target0 = float4(diffuseAlbedo.rgb, gRoughness);
    gout.Target1 = float4(normalW * 0.5f + 0.5f, 1.0f);
    gout.Target2 = float2(0.0f, 0.0f);

    return gout;
}
