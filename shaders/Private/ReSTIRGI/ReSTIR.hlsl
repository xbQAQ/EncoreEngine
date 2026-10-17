#include "Common.hlsli"

// 样本定义在次级表面积上，不依赖当前接收点的方向半球。
float3 AreaContribution(Receiver receiver, Reservoir sample)
{
    float3 delta = sample.secondaryPositionPdf.xyz - receiver.position;
    float distanceSquared = dot(delta, delta);
    if (distanceSquared < 1e-6) return 0.0;
    float3 direction = delta * rsqrt(distanceSquared);
    // 重连接必须处于接收点真实几何表面的反射侧。
    if (dot(receiver.geometricNormal, direction) <= 0) return 0;
    float receiverCosine = saturate(dot(receiver.normal, direction));
    float secondaryCosine = saturate(dot(sample.secondaryNormalTarget.xyz, -direction));
    float geometry = receiverCosine * secondaryCosine / distanceSquared;
    return EvaluateBsdf(receiver.material, receiver.normal, receiver.outgoing, direction) * sample.radianceWeightSum.xyz * geometry;
}
float Target(Receiver receiver, Reservoir sample)
{
    float target = Luminance(max(AreaContribution(receiver, sample), 0.0));
    return isfinite(target) ? max(target, 0.0) : 0.0;
}
bool ReservoirValid(Reservoir reservoir)
{
    return reservoir.receiverPositionM.w > 0.0 && reservoir.receiverNormalW.w > 0.0 &&
        reservoir.secondaryNormalTarget.w > 0.0 && all(isfinite(reservoir.radianceWeightSum)) &&
        all(isfinite(reservoir.secondaryPositionPdf));
}
void ReservoirUpdate(inout Reservoir reservoir, Reservoir sample, float weight, float count, inout uint state)
{
    float oldSum = reservoir.radianceWeightSum.w;
    float oldCount = reservoir.receiverPositionM.w;
    weight = isfinite(weight) ? max(weight, 0.0) : 0.0;
    count = isfinite(count) ? max(count, 0.0) : 0.0;
    float newSum = oldSum + weight;
    if (weight > 0.0 && Random(state) * newSum < weight) reservoir = sample;
    reservoir.radianceWeightSum.w = newSum;
    // 零权重未命中也计入提议，丢弃会使估计偏亮。
    reservoir.receiverPositionM.w = oldCount + count;
}
void FinalizeReservoir(inout Reservoir reservoir, Receiver receiver)
{
    float target = Target(receiver, reservoir);
    float count = reservoir.receiverPositionM.w;
    float weightSum = reservoir.radianceWeightSum.w;
    float maximumCount = max(Restir.x, 1.0);
    if (count > maximumCount)
    {
        // 同步缩放两项统计以保持 W，并限制相关历史的影响。
        weightSum *= maximumCount / count;
        count = maximumCount;
    }
    float weight = count > 0.0 && target > 1e-12 ? weightSum / (count * target) : 0.0;
    if (!isfinite(weight) || weight < 0.0) weight = 0.0;
    reservoir.secondaryNormalTarget.w = target;
    reservoir.radianceWeightSum.w = weightSum;
    reservoir.receiverPositionM = float4(receiver.position, count);
    reservoir.receiverNormalW = float4(receiver.normal, weight);
    float3 delta = reservoir.secondaryPositionPdf.xyz - receiver.position;
    reservoir.directionDistance = float4(SafeNormalize(delta), length(delta));
}
float ReconnectionJacobian(Receiver receiver, Reservoir source)
{
    float3 oldDelta = source.secondaryPositionPdf.xyz - source.receiverPositionM.xyz;
    float3 newDelta = source.secondaryPositionPdf.xyz - receiver.position;
    float oldDistanceSquared = max(dot(oldDelta, oldDelta), 1e-10);
    float newDistanceSquared = max(dot(newDelta, newDelta), 1e-10);
    float oldCosine = saturate(dot(source.secondaryNormalTarget.xyz, -oldDelta * rsqrt(oldDistanceSquared)));
    float newCosine = saturate(dot(source.secondaryNormalTarget.xyz, -newDelta * rsqrt(newDistanceSquared)));
    return (newCosine * oldDistanceSquared) / max(oldCosine * newDistanceSquared, 1e-12);
}
void MergeReservoir(inout Reservoir destination, Reservoir source, Receiver receiver, inout uint state)
{
    if (!ReservoirValid(source)) return;
    float jacobian = ReconnectionJacobian(receiver, source);
    // 面积概率 q_A 与贡献 f_A 均包含 cosSecondary/r² 的相同转换。
    // 同一次级点的恒等移动具有单位面积 Jacobian。
    // 再次乘立体角 Jacobian 会重复应用几何因子。
    // 极端重连接作为有偏稳定性启发被拒绝。
    if (!isfinite(jacobian) || jacobian < 0.05 || jacobian > 20.0) return;
    float targetHere = Target(receiver, source);
    float sourceCount = min(source.receiverPositionM.w, max(Restir.x, 1.0));
    float weight = targetHere * source.receiverNormalW.w * sourceCount;
    source.secondaryNormalTarget.w = targetHere;
    ReservoirUpdate(destination, source, weight, sourceCount, state);
}
bool CompatibleNeighbor(Receiver receiver, uint2 neighborPixel)
{
    float4 position = PositionBuffer.Load(int3(neighborPixel, 0));
    float4 normal = NormalBuffer.Load(int3(neighborPixel, 0));
    float3 delta = position.xyz - receiver.position;
    float depth = max(distance(receiver.position, CameraPosition.xyz), 1.0);
    // 切向距离可随半径扩大，平面深度与材质仍须兼容。
    return position.w > 0.0 && abs(position.w - float(receiver.materialId + 1u)) < 0.25 &&
        dot(receiver.normal, normal.xyz) > Restir.z &&
        abs(dot(delta, receiver.normal)) < max(0.03, Restir.w * depth) &&
        distance(position.xyz, receiver.position) < max(0.25, depth * 0.35) &&
        abs(normal.w - receiver.material.baseRoughness.w) < 0.2;
}

[numthreads(8, 8, 1)]
void CandidateMain(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (!InBounds(pixel)) return;
    Receiver receiver = LoadReceiver(pixel);
    Reservoir reservoir = (Reservoir)0;
    // 路径深度包含主可见表面在内的全部顶点。
    if (receiver.valid == 0u || Settings.w <= 1u || UsesPathFallback(receiver.material))
    {
        // 完整路径像素没有可重连接 reservoir，避免生成不会使用的次级路径。
        OutputReservoir[PixelIndex(pixel)] = reservoir;
        return;
    }
    uint state = Seed(pixel, 23u);
    // SPP 控制各积分器的新路径数量，Candidates 表示每采样的 RIS 提议数。
    uint candidates = clamp(Counts.w, 1u, 32u) * clamp(uint(Sampling.x), 1u, 16u);
    Hit primary = (Hit)0;
    primary.normal = receiver.normal;
    primary.geometricNormal = receiver.geometricNormal;
    [loop] for (uint candidateIndex = 0u; candidateIndex < candidates; ++candidateIndex)
    {
        Reservoir candidate = (Reservoir)0;
        // 锐利金属及清漆波瓣使用与路径追踪一致的完整混合 VNDF 提议。
        BsdfSample proposal = SampleBsdf(receiver.material, primary, receiver.outgoing, state);
        float3 direction = proposal.direction;
        // 空 BSDF 事件仍计数，反复采样至有效方向会使 RIS 归一化有偏。
        Hit secondary = (Hit)0;
        if (proposal.pdf > 0 && proposal.delta == 0u && dot(direction, receiver.geometricNormal) > 0)
            secondary = TraceScene(RayOrigin(receiver.position, receiver.geometricNormal, direction), direction, MAX_RAY_DISTANCE);
        float weight = 0.0;
        // 自发光表面也可散射，DiffuseOutgoing 只减去 NEE 覆盖的首次解析发射。
        if (secondary.found != 0u)
        {
            float distanceSquared = max(dot(secondary.position - receiver.position, secondary.position - receiver.position), 1e-10);
            float directionPdf = proposal.pdf;
            float secondaryCosine = saturate(dot(secondary.geometricNormal, -direction));
            float areaPdf = directionPdf * secondaryCosine / distanceSquared;
            candidate.secondaryPositionPdf = float4(secondary.position, areaPdf);
            // 表面积密度使用几何法线，着色法线只用于 BSDF 求值。
            candidate.secondaryNormalTarget = float4(secondary.geometricNormal, 0.0);
            candidate.radianceWeightSum = float4(DiffuseOutgoing(secondary, min(Settings.w, 16u) - 1u, state), 0.0);
            candidate.receiverPositionM = float4(receiver.position, 0.0);
            candidate.receiverNormalW = float4(receiver.normal, 0.0);
            candidate.directionDistance = float4(direction, sqrt(distanceSquared));
            float target = Target(receiver, candidate);
            candidate.secondaryNormalTarget.w = target;
            if (areaPdf > 1e-12) weight = target / areaPdf;
        }
        ReservoirUpdate(reservoir, candidate, weight, 1.0, state);
    }
    FinalizeReservoir(reservoir, receiver);
    OutputReservoir[PixelIndex(pixel)] = reservoir;
}

[numthreads(8, 8, 1)]
void TemporalMain(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (!InBounds(pixel)) return;
    uint index = PixelIndex(pixel);
    Receiver receiver = LoadReceiver(pixel);
    Reservoir reservoir = CurrentReservoir[index];
    if (receiver.valid == 0u) { OutputReservoir[index] = (Reservoir)0; return; }
    uint state = Seed(pixel, 29u);
    if (Flags.x == 0u && Flags.y != 0u)
    {
        float2 previousUv;
        float previousDepth;
        if (ProjectPosition(receiver.position, true, previousUv, previousDepth))
        {
            uint2 oldPixel = min(uint2(previousUv * Resolution.xy), uint2(Resolution.xy) - 1u);
            if (CompatibleHistory(receiver, oldPixel)) MergeReservoir(reservoir, PreviousReservoir[PixelIndex(oldPixel)], receiver, state);
        }
    }
    FinalizeReservoir(reservoir, receiver);
    OutputReservoir[index] = reservoir;
}

[numthreads(8, 8, 1)]
void SpatialMain(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (!InBounds(pixel)) return;
    uint index = PixelIndex(pixel);
    Receiver receiver = LoadReceiver(pixel);
    Reservoir reservoir = TemporalReservoir[index];
    if (receiver.valid == 0u) { OutputReservoir[index] = (Reservoir)0; return; }
    uint state = Seed(pixel, 31u);
    if (Flags.z != 0u)
    {
        float rotation = Random(state) * 2.0 * PI;
        [unroll] for (uint neighborIndex = 0u; neighborIndex < 6u; ++neighborIndex)
        {
            float angle = rotation + float(neighborIndex) * 2.39996323;
            float radius = max(Restir.y, 1.0) * sqrt((float(neighborIndex) + 0.5) / 6.0);
            int2 neighborPixel = int2(pixel) + int2(round(float2(cos(angle), sin(angle)) * radius));
            if (any(neighborPixel < 0) || any(neighborPixel >= int2(Resolution.xy))) continue;
            if (!CompatibleNeighbor(receiver, uint2(neighborPixel))) continue;
            // 邻居全部来自不可变时间缓冲，避免原地反馈竞争。
            MergeReservoir(reservoir, TemporalReservoir[PixelIndex(uint2(neighborPixel))], receiver, state);
        }
    }
    FinalizeReservoir(reservoir, receiver);
    OutputReservoir[index] = reservoir;
}

[numthreads(8, 8, 1)]
void EvaluateMain(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchId.xy;
    if (!InBounds(pixel)) return;
    Receiver receiver = LoadReceiver(pixel);
    Reservoir reservoir = CurrentReservoir[PixelIndex(pixel)];
    float3 result = 0.0;
    if (receiver.valid != 0u && ReservoirValid(reservoir))
    {
        // 每次复用后重新计算贡献与接收点至次级表面的可见性。
        if (SegmentVisible(receiver.position, receiver.geometricNormal, reservoir.secondaryPositionPdf.xyz))
            result = AreaContribution(receiver, reservoir) * reservoir.receiverNormalW.w;
    }
    if (receiver.valid != 0u && UsesPathFallback(receiver.material))
    {
        // 锐利反射、清漆和透射使用完整 BSDF 路径，避免错误漫反射重连接。
        uint state = Seed(pixel, 41u);
        uint samples = clamp(uint(Sampling.x), 1u, 16u);
        result = 0.0;
        [loop] for (uint i = 0u; i < samples; ++i)
            // 与 PT 一样积分像素足迹，包含亚像素光泽细节与轮廓。
            result += TracePath(CameraPosition.xyz, CameraRay(float2(pixel) + Random2(state)), clamp(Settings.w, 1u, 16u), state);
        result /= float(samples);
    }
    else if (receiver.valid != 0u && Sky.x > 0.5 && Settings.w > 1u && uint(Sampling.w) == 0u)
    {
        // 无限远天空不属于表面积候选域，使用独立 BSDF 未命中估计而非错误的面积 PDF。
        uint environmentState = Seed(pixel, 97u);
        uint environmentSamples = clamp(uint(Sampling.x), 1u, 16u);
        Hit primary = (Hit)0;
        primary.normal = receiver.normal;
        primary.geometricNormal = receiver.geometricNormal;
        [loop] for (uint sampleIndex = 0u; sampleIndex < environmentSamples; ++sampleIndex)
        {
            BsdfSample sample = SampleBsdf(receiver.material, primary, receiver.outgoing, environmentState);
            if (sample.pdf <= 0.0 || dot(sample.direction, receiver.geometricNormal) <= 0.0) continue;
            Hit environmentHit = TraceScene(RayOrigin(receiver.position, receiver.geometricNormal, sample.direction),
                sample.direction, MAX_RAY_DISTANCE);
            if (environmentHit.found == 0u)
                result += sample.weight * SkyRadiance(sample.direction) / float(environmentSamples);
        }
    }
    uint debugView = uint(Sampling.w);
    if (debugView == 1u)
    {
        float m = saturate(reservoir.receiverPositionM.w / max(Restir.x, 1.0));
        result = float3(m, m * m, 1.0 - m);
    }
    else if (debugView == 2u)
    {
        float w = saturate(log2(1.0 + max(reservoir.receiverNormalW.w, 0.0)) / 12.0);
        result = float3(w, 0.2 * (1.0 - w), 1.0 - w);
    }
    else if (debugView == 3u) result = receiver.valid != 0u ? receiver.normal * 0.5 + 0.5 : 0.0;
    Output0[pixel] = float4(all(isfinite(result)) ? max(result, 0.0) : 0.0, reservoir.receiverPositionM.w);
}
