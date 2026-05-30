#ifndef NUM_DIR_LIGHTS
    #define NUM_DIR_LIGHTS 3
#endif

#ifndef NUM_POINT_LIGHTS
    #define NUM_POINT_LIGHTS 32
#endif

#ifndef NUM_SPOT_LIGHTS
    #define NUM_SPOT_LIGHTS 4
#endif

#include "LightingUtil.hlsl"

Texture2D gGBuffer0 : register(t0);
Texture2D gGBuffer1 : register(t1);
Texture2D gDepthMap : register(t2);
Texture2D gGBuffer2 : register(t3);
Texture2DArray gShadowMap : register(t4);
Texture2DArray gTexturedShadowMap : register(t5);
Texture2D gTexturedShadowPattern : register(t6);

SamplerState gsamPointWrap : register(s0);
SamplerState gsamPointClamp : register(s1);
SamplerState gsamLinearWrap : register(s2);
SamplerState gsamLinearClamp : register(s3);
SamplerState gsamAnisotropicWrap : register(s4);
SamplerState gsamAnisotropicClamp : register(s5);
SamplerComparisonState gsamShadow : register(s6);

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

int SelectCascade(float viewDepth)
{
    int cascadeIndex = 0;
    cascadeIndex += (viewDepth > gCascadeSplits.x);
    cascadeIndex += (viewDepth > gCascadeSplits.y);
    cascadeIndex += (viewDepth > gCascadeSplits.z);
    return min(cascadeIndex, 3);
}

float CalcShadowFactor(float3 posW)
{
    float viewDepth = mul(float4(posW, 1.0f), gView).z;
    int cascadeIndex = SelectCascade(viewDepth);

    float4 shadowPosH = mul(float4(posW, 1.0f), gShadowTransform[cascadeIndex]);
    shadowPosH.xyz /= shadowPosH.w;

    if (shadowPosH.x < 0.0f || shadowPosH.x > 1.0f ||
        shadowPosH.y < 0.0f || shadowPosH.y > 1.0f ||
        shadowPosH.z < 0.0f || shadowPosH.z > 1.0f)
    {
        return 1.0f;
    }

    uint width;
    uint height;
    uint elements;
    gShadowMap.GetDimensions(width, height, elements);

    float2 texelSize = 1.0f / float2(width, height);
    float depth = shadowPosH.z - 0.0015f;
    float percentLit = 0.0f;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 uv = shadowPosH.xy + float2(x, y) * texelSize;
            percentLit += gShadowMap.SampleCmpLevelZero(gsamShadow, float3(uv, cascadeIndex), depth);
        }
    }

    return percentLit / 9.0f;
}

float CalcTexturedShadowAmount(float3 posW)
{
    float viewDepth = mul(float4(posW, 1.0f), gView).z;
    int cascadeIndex = SelectCascade(viewDepth);

    float4 shadowPosH = mul(float4(posW, 1.0f), gShadowTransform[cascadeIndex]);
    shadowPosH.xyz /= shadowPosH.w;

    if (shadowPosH.x < 0.0f || shadowPosH.x > 1.0f ||
        shadowPosH.y < 0.0f || shadowPosH.y > 1.0f ||
        shadowPosH.z < 0.0f || shadowPosH.z > 1.0f)
    {
        return 0.0f;
    }

    float depth = shadowPosH.z - 0.0015f;
    float percentLit = gTexturedShadowMap.SampleCmpLevelZero(
        gsamShadow,
        float3(shadowPosH.xy, cascadeIndex),
        depth);

    return 1.0f - percentLit;
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
    float3 shadowFactor = float3(CalcShadowFactor(posW), 1.0f, 1.0f);

    float4 directLight = ComputeLighting(gLights, mat, posW, normalW, toEyeW, shadowFactor);

    float4 litColor = ambient + directLight;
    float texturedShadowAmount = CalcTexturedShadowAmount(posW);
    float3 texturedShadow = gTexturedShadowPattern.Sample(gsamLinearWrap, posW.xz * 1.0f).rgb;
    litColor.rgb = lerp(litColor.rgb, texturedShadow * 0.82f, texturedShadowAmount * 0.92f);
    litColor.a = 1.0f;

    return litColor;
}
