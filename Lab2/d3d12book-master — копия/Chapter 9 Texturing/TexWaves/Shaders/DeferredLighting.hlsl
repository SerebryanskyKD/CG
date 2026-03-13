//***************************************************************************************
// DeferredLighting.hlsl
// Fullscreen deferred lighting pass (ambient + directional only).
//***************************************************************************************

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

Texture2D gGBuffer0 : register(t0); // albedo.rgb + roughness
Texture2D gGBuffer1 : register(t1); // encoded normal.xyz
Texture2D gDepthMap : register(t2); // depth buffer as SRV

SamplerState gsamPointWrap : register(s0);
SamplerState gsamPointClamp : register(s1);
SamplerState gsamLinearWrap : register(s2);
SamplerState gsamLinearClamp : register(s3);
SamplerState gsamAnisotropicWrap : register(s4);
SamplerState gsamAnisotropicClamp : register(s5);

cbuffer cbPass : register(b0)
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

struct VSOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

VSOut VS(uint vid : SV_VertexID)
{
    VSOut vout;

    float2 pos[3] =
    {
        float2(-1.0f, -1.0f),
        float2(-1.0f, 3.0f),
        float2(3.0f, -1.0f)
    };

    float2 uv[3] =
    {
        float2(0.0f, 1.0f),
        float2(0.0f, -1.0f),
        float2(2.0f, 1.0f)
    };

    vout.PosH = float4(pos[vid], 0.0f, 1.0f);
    vout.TexC = uv[vid];

    return vout;
}

float3 DecodeNormal(float3 enc)
{
    return normalize(enc * 2.0f - 1.0f);
}

float3 ReconstructWorldPos(float2 texC, float depthNdc)
{
    float2 ndcXY = float2(
        texC.x * 2.0f - 1.0f,
        1.0f - texC.y * 2.0f);

    float4 posH = float4(ndcXY, depthNdc, 1.0f);
    float4 posW = mul(posH, gInvViewProj);
    posW /= posW.w;
    return posW.xyz;
}

float4 PS(VSOut pin) : SV_Target
{
    float4 g0 = gGBuffer0.Sample(gsamPointClamp, pin.TexC);
    float4 g1 = gGBuffer1.Sample(gsamPointClamp, pin.TexC);
    float depth = gDepthMap.Sample(gsamPointClamp, pin.TexC).r;

    float3 albedo = g0.rgb;
    float roughness = g0.a;
    float3 normalW = DecodeNormal(g1.xyz);

    float3 posW = ReconstructWorldPos(pin.TexC, depth);
    float3 toEyeW = normalize(gEyePosW - posW);

    float4 diffuseAlbedo = float4(albedo, 1.0f);
    float4 ambient = gAmbientLight * diffuseAlbedo;

    const float shininess = 1.0f - roughness;
    Material mat = { diffuseAlbedo, float3(0.04f, 0.04f, 0.04f), shininess };
    float3 shadowFactor = 1.0f;

    float4 directLight = ComputeLighting(gLights, mat, posW, normalW, toEyeW, shadowFactor);

    float4 litColor = ambient + directLight;
    litColor.a = 1.0f;

    return litColor;
}