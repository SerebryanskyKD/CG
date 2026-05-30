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

ByteAddressBuffer gAliveCountIn : register(t0);
ConsumeStructuredBuffer<Particle> gParticlesIn : register(u0);
AppendStructuredBuffer<Particle> gParticlesOut : register(u1);
RWByteAddressBuffer gAliveCountOut : register(u2);
RWStructuredBuffer<Particle> gParticlesDirectOut : register(u3);

cbuffer cbParticleUpdate : register(b0)
{
    float gDeltaTime;
    float gTotalTime;
    float3 gEmitterPos;
    float gEmissionRate;
    float gMaxParticles;
    float gSpawnRadius;
    float2 gParticleUpdatePad;
    float3 gObstacleCenter;
    float gObstaclePad0;
    float3 gObstacleExtents;
    float gObstaclePad1;
};

float Hash11(float n)
{
    return frac(sin(n) * 43758.5453f);
}

[numthreads(1, 1, 1)]
void CS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint aliveCount = gAliveCountIn.Load(0);
    uint outCount = 0;

    [loop]
    for (uint i = 0; i < aliveCount; ++i)
    {
        Particle particle = gParticlesIn.Consume();
        particle.Age += gDeltaTime;

        if (particle.Age < particle.LifeTime)
        {
            float3 previousPosition = particle.Position;
            particle.Position += particle.Velocity * gDeltaTime;

            float3 obstacleMin = gObstacleCenter - gObstacleExtents;
            float3 obstacleMax = gObstacleCenter + gObstacleExtents;
            bool insideObstacleXZ =
                particle.Position.x >= obstacleMin.x && particle.Position.x <= obstacleMax.x &&
                particle.Position.z >= obstacleMin.z && particle.Position.z <= obstacleMax.z;
            bool hitsObstacleBottom =
                insideObstacleXZ &&
                particle.Position.y >= obstacleMin.y &&
                previousPosition.y <= obstacleMin.y + 0.03f;

            if (hitsObstacleBottom)
            {
                float2 awayFromCenter = particle.Position.xz - gObstacleCenter.xz;
                if (dot(awayFromCenter, awayFromCenter) < 0.0001f)
                {
                    float angle = Hash11(particle.Age * 91.0f + particle.LifeTime * 37.0f) * 6.2831853f;
                    awayFromCenter = float2(cos(angle), sin(angle));
                }

                particle.Position.y = obstacleMin.y - 0.03f;
                particle.Velocity.y = 0.0f;
                particle.Velocity.xz += normalize(awayFromCenter) * 0.42f;
            }

            particle.Velocity += float3(0.0f, 0.6f, 0.0f) * gDeltaTime;
            particle.Color.rgb *= 0.992f;
            gParticlesDirectOut[outCount] = particle;
            gParticlesOut.Append(particle);
            outCount++;
        }
    }

    uint emitCount = (uint)gEmissionRate;
    if (outCount + emitCount > (uint)gMaxParticles)
        emitCount = ((uint)gMaxParticles > outCount) ? ((uint)gMaxParticles - outCount) : 0;

    [loop]
    for (uint i = 0; i < emitCount; ++i)
    {
        float seed = gTotalTime * 37.0f + i * 17.0f;
        float angle = Hash11(seed) * 6.2831853f;
        float radius = Hash11(seed + 13.0f) * gSpawnRadius;

        Particle particle;
        particle.Position = gEmitterPos + float3(cos(angle) * radius, 0.0f, sin(angle) * radius);
        particle.Age = 0.0f;
        particle.Velocity = float3(
            lerp(-0.7f, 0.7f, Hash11(seed + 29.0f)),
            lerp(2.8f, 4.4f, Hash11(seed + 53.0f)),
            lerp(-0.7f, 0.7f, Hash11(seed + 71.0f)));
        particle.LifeTime = lerp(2.8f, 4.2f, Hash11(seed + 89.0f));
        particle.Color = float4(
            lerp(0.98f, 1.00f, Hash11(seed + 101.0f)),
            lerp(0.55f, 0.85f, Hash11(seed + 131.0f)),
            lerp(0.06f, 0.14f, Hash11(seed + 151.0f)),
            1.0f);
        particle.Size = lerp(1.10f, 1.80f, Hash11(seed + 173.0f));
        particle.Pad = 0.0f;

        gParticlesDirectOut[outCount] = particle;
        gParticlesOut.Append(particle);
        outCount++;
    }

    gAliveCountOut.Store(0, outCount);
}
