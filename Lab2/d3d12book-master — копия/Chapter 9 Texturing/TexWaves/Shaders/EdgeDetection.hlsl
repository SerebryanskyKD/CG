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

float Luma(float3 c)
{
    return dot(c, float3(0.299f, 0.587f, 0.114f));
}

float4 PS(VSOut pin) : SV_Target
{
    float2 t = gInvRenderTargetSize;

    float tl = Luma(gInputMap.Sample(gsamLinearClamp, pin.TexC + t * float2(-1.0f, -1.0f)).rgb);
    float tc = Luma(gInputMap.Sample(gsamLinearClamp, pin.TexC + t * float2(0.0f, -1.0f)).rgb);
    float tr = Luma(gInputMap.Sample(gsamLinearClamp, pin.TexC + t * float2(1.0f, -1.0f)).rgb);
    float ml = Luma(gInputMap.Sample(gsamLinearClamp, pin.TexC + t * float2(-1.0f, 0.0f)).rgb);
    float mr = Luma(gInputMap.Sample(gsamLinearClamp, pin.TexC + t * float2(1.0f, 0.0f)).rgb);
    float bl = Luma(gInputMap.Sample(gsamLinearClamp, pin.TexC + t * float2(-1.0f, 1.0f)).rgb);
    float bc = Luma(gInputMap.Sample(gsamLinearClamp, pin.TexC + t * float2(0.0f, 1.0f)).rgb);
    float br = Luma(gInputMap.Sample(gsamLinearClamp, pin.TexC + t * float2(1.0f, 1.0f)).rgb);

    float gx = -tl - 2.0f * ml - bl + tr + 2.0f * mr + br;
    float gy = -tl - 2.0f * tc - tr + bl + 2.0f * bc + br;
    float edge = saturate(sqrt(gx * gx + gy * gy) * 1.8f);

    float3 color = gInputMap.Sample(gsamLinearClamp, pin.TexC).rgb;
    color = lerp(color, color * 0.28f, edge);
    return float4(color, 1.0f);
}
