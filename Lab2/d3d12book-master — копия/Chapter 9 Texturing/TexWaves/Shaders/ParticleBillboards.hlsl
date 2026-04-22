#include "LightingUtil.hlsl"

struct Particle
{
    float3 Position;
    float Age;
    float3 Velocity;
    float LifeTime;
    float4 Color;
    float Size;
    float3 Pad;
};

StructuredBuffer<Particle> gParticles : register(t0);
ByteAddressBuffer gAliveCount : register(t1);

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

struct VSOut
{
    float3 PosW : POSITION;
    float4 Color : COLOR;
    float Size : PSIZE;
    uint Alive : ALIVE;
};

struct GSOut
{
    float4 PosH : SV_POSITION;
    float4 Color : COLOR;
    float2 TexC : TEXCOORD;
};

VSOut VS(uint vertexID : SV_VertexID)
{
    VSOut vout = (VSOut)0.0f;

    uint aliveCount = gAliveCount.Load(0);
    if (vertexID >= aliveCount)
    {
        vout.Alive = 0;
        return vout;
    }

    Particle particle = gParticles[vertexID];
    float ageRatio = saturate(particle.Age / max(particle.LifeTime, 0.001f));

    vout.PosW = particle.Position;
    vout.Color = float4(lerp(particle.Color.rgb, float3(0.15f, 0.05f, 0.02f), ageRatio), 1.0f);
    vout.Size = lerp(particle.Size, particle.Size * 0.45f, ageRatio);
    vout.Alive = 1;
    return vout;
}

[maxvertexcount(4)]
void GS(point VSOut gin[1], inout TriangleStream<GSOut> triStream)
{
    if (gin[0].Alive == 0)
        return;

    float3 look = normalize(gEyePosW - gin[0].PosW);
    float3 upAxis = float3(0.0f, 1.0f, 0.0f);
    float3 right = cross(upAxis, look);
    if (dot(right, right) < 1e-4f)
        right = float3(1.0f, 0.0f, 0.0f);
    right = normalize(right);
    float3 up = normalize(cross(look, right));

    float halfSize = 0.5f * gin[0].Size;
    float3 corners[4] =
    {
        gin[0].PosW + (-right - up) * halfSize,
        gin[0].PosW + (-right + up) * halfSize,
        gin[0].PosW + ( right - up) * halfSize,
        gin[0].PosW + ( right + up) * halfSize
    };
    float2 uvs[4] =
    {
        float2(0.0f, 1.0f),
        float2(0.0f, 0.0f),
        float2(1.0f, 1.0f),
        float2(1.0f, 0.0f)
    };

    GSOut gout;
    gout.Color = gin[0].Color;

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        gout.PosH = mul(float4(corners[i], 1.0f), gViewProj);
        gout.TexC = uvs[i];
        triStream.Append(gout);
    }
}

float4 PS(GSOut pin) : SV_Target
{
    float2 centered = pin.TexC * 2.0f - 1.0f;
    float radius2 = dot(centered, centered);
    clip(1.0f - radius2);

    float edge = saturate(1.0f - radius2);
    float glow = edge * edge;
    return float4(pin.Color.rgb * lerp(0.55f, 1.0f, glow), 1.0f);
}
