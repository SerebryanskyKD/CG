Texture2D gInputMap : register(t0);

SamplerState gsamPointWrap : register(s0);
SamplerState gsamPointClamp : register(s1);
SamplerState gsamLinearWrap : register(s2);
SamplerState gsamLinearClamp : register(s3);
SamplerState gsamAnisotropicWrap : register(s4);
SamplerState gsamAnisotropicClamp : register(s5);

cbuffer cbPost : register(b0)
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

float Rand(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

float4 PS(VSOut pin) : SV_Target
{
    float2 uv = pin.TexC;
    float wobble = sin(uv.y * 90.0f + gTotalTime * 5.0f) * 0.0012f;
    float2 warpedUv = uv + float2(wobble, 0.0f);

    float chroma = 1.5f * gInvRenderTargetSize.x;
    float3 color;
    color.r = gInputMap.Sample(gsamLinearClamp, warpedUv + float2(chroma, 0.0f)).r;
    color.g = gInputMap.Sample(gsamLinearClamp, warpedUv).g;
    color.b = gInputMap.Sample(gsamLinearClamp, warpedUv - float2(chroma, 0.0f)).b;

    float scanline = 0.94f + 0.06f * sin(uv.y * gRenderTargetSize.y * 3.14159f);
    float noise = Rand(floor(uv * gRenderTargetSize) + floor(gTotalTime * 24.0f)) - 0.5f;
    float2 centered = uv * 2.0f - 1.0f;
    float vignette = saturate(1.12f - dot(centered, centered) * 0.22f);

    color *= scanline * vignette;
    color += noise * 0.025f;

    return float4(saturate(color), 1.0f);
}
