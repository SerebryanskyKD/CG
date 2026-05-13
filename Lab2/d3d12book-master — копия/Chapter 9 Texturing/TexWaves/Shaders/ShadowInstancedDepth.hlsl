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
    float4x4 gShadowTransform[4];
    float4 gCascadeSplits;
};

struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

float4 VS(VertexIn vin, uint instanceID : SV_InstanceID) : SV_POSITION
{
    float phase = gTotalTime * 2.1f + instanceID * 0.17f;
    float pulse = 0.5f + 0.5f * sin(phase);
    float heightScale = lerp(0.68f, 1.22f, pulse);
    float sideScale = lerp(1.16f, 0.92f, pulse);
    float3 localScale = float3(sideScale, heightScale, sideScale);

    float3 animatedPosL = vin.PosL * localScale;
    float4 posW = mul(float4(animatedPosL, 1.0f), gInstanceData[instanceID].World);
    return mul(posW, gViewProj);
}
