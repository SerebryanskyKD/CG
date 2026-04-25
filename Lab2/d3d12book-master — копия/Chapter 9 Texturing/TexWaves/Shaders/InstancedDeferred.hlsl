//***************************************************************************************
// InstancedDeferred.hlsl
// Deferred GBuffer pass for CPU-culled instanced scatter boxes.
//***************************************************************************************

#include "LightingUtil.hlsl"

struct InstanceData
{
    float4x4 World;
};

StructuredBuffer<InstanceData> gInstanceData : register(t0);

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

cbuffer cbMaterial : register(b1)
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
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
};

struct GBufferOut
{
    float4 Target0 : SV_Target0;
    float4 Target1 : SV_Target1;
    float2 Target2 : SV_Target2;
};

VertexOut VS(VertexIn vin, uint instanceID : SV_InstanceID)
{
    VertexOut vout = (VertexOut)0.0f;

    float phase = gTotalTime * 2.1f + instanceID * 0.17f;
    float pulse = 0.5f + 0.5f * sin(phase);
    float heightScale = lerp(0.68f, 1.22f, pulse);
    float sideScale = lerp(1.16f, 0.92f, pulse);
    float3 localScale = float3(sideScale, heightScale, sideScale);

    float3 animatedPosL = vin.PosL * localScale;
    float3 animatedNormalL = normalize(vin.NormalL / localScale);

    float4x4 world = gInstanceData[instanceID].World;
    float4 posW = mul(float4(animatedPosL, 1.0f), world);

    vout.PosW = posW.xyz;
    vout.NormalW = mul(animatedNormalL, (float3x3)world);
    vout.PosH = mul(posW, gViewProj);

    return vout;
}

GBufferOut PS(VertexOut pin)
{
    GBufferOut gout;

    float3 normalW = normalize(pin.NormalW);
    float3 encodedNormal = normalW * 0.5f + 0.5f;

    gout.Target0 = float4(gDiffuseAlbedo.rgb, gRoughness);
    gout.Target1 = float4(encodedNormal, 1.0f);
    gout.Target2 = float2(0.0f, 0.0f);

    return gout;
}
