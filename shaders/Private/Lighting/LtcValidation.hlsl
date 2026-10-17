#include "Common.hlsli"
#include "LTC.hlsli"

struct LtcValidationResult
{
    float4 approximation; // 实际 LTC 加 Disney 补偿的 RGB 与球面积分基线。
    float4 reference; // 独立高密度面积积分的 RGB 与参考基线。
};
RWStructuredBuffer<LtcValidationResult> LtcResults : register(u4);

void LtcValidationCase(uint index, out Receiver receiver, out Light light)
{
    // 基准与变体只使用已经求值的材质，隔离 LUT 布局、积分和材质瓣的数值行为。
    receiver = (Receiver)0;
    receiver.valid = 1u;
    receiver.normal = receiver.geometricNormal = receiver.outgoing = float3(0, 0, 1);
    receiver.material.baseRoughness = float4(0.7, 0.45, 0.25, 0.6);
    receiver.material.transmissionIor = float4(0, 1.5, 0, 0.35);
    receiver.material.principled = float4(0.5, 0, 0, 0);
    receiver.material.tangentFrame = float4(1, 0, 0, 1);
    light = (Light)0;
    light.positionType = float4(0.35, 0.15, 2, 2);
    light.directionRange = float4(0, 0, -1, 0);
    light.colorIntensity = 1;
    light.tangentWidth = float4(1, 0, 0, 3);
    light.bitangentHeight = float4(0, 1, 0, 2);
    if (index == 2u) { receiver.material.emissionMetallic.w = 1; receiver.material.baseRoughness.w = 0.35; }
    if (index == 3u) receiver.material.baseRoughness.w = 0.95;
    if (index == 4u) receiver.outgoing = SafeNormalize(float3(1, 0, 0.15));
    if (index == 5u) { light.positionType.xy = float2(1.8, -0.7); receiver.outgoing = SafeNormalize(float3(-0.5, 0.3, 1)); }
    if (index == 6u || index == 7u) light.positionType.w = 3;
    if (index == 7u) { receiver.outgoing = SafeNormalize(float3(1, 0.1, 0.15)); receiver.material.emissionMetallic.w = 1; }
    if (index == 8u) {
        light.positionType.xyz = float3(1.5, 0, 0.1);
        light.directionRange.xyz = float3(-1, 0, 0);
        light.tangentWidth = float4(0, 1, 0, 3);
        light.bitangentHeight = float4(0, 0, 1, 2);
    }
    if (index == 9u) light.directionRange.xyz = float3(0, 0, 1);
    if (index == 10u) light.positionType.xyz = float3(5, 1, 12);
    if (index == 11u) receiver.material.transmissionIor.z = 1;
    if (index == 12u) { receiver.material.sheen.xy = float2(1, 1); receiver.material.principled.w = 0.5; }
    if (index == 13u) {
        receiver.material.emissionMetallic.w = 1;
        receiver.material.principled.z = 0.8;
        receiver.material.extras.x = 0.7;
    }
    if (index == 14u || index == 15u || index == 17u) {
        light.positionType.xyz = float3(0, 0, -2);
        light.directionRange.xyz = float3(0, 0, 1);
    }
    if (index == 14u) { receiver.material.transmissionIor.x = 1; receiver.material.baseRoughness.w = 0.7; }
    if (index == 15u) { receiver.material.flags.x = 1u; receiver.material.sheen.w = 1; }
    if (index == 16u || index == 17u) receiver.material.baseRoughness.w = 0;
    if (index == 16u) { receiver.material.emissionMetallic.w = 1; light.positionType.xy = 0; }
    if (index == 17u) receiver.material.transmissionIor.x = 1;
    if (index == 18u) {
        receiver.normal = SafeNormalize(float3(0.8, 0, 1));
        receiver.outgoing = receiver.normal;
        light.positionType.xyz = float3(-1.2, 0, 0.3);
    }
    if (index == 19u) {
        light.tangentWidth.w = 10;
        light.bitangentHeight.w = 10;
        light.positionType.xyz = float3(0, 0, 0.5);
    }
}

float3 LtcValidationReference(Receiver receiver, Light light, uint index)
{
    // 独立均匀面积积分不读取 LTC 表、不调用球面边积分，也不使用生产补偿求积的节点。
    float3 result = 0;
    [loop] for (uint y = 0u; y < 192u; ++y) {
        [loop] for (uint x = 0u; x < 192u; ++x) {
            float3 delta = SampleEmitterPoint(light, (float2(x, y) + 0.5) / 192) - receiver.position;
            float distanceSquared = max(dot(delta, delta), 1e-12);
            float3 direction = delta * rsqrt(distanceSquared);
            if (dot(receiver.normal, direction) * dot(receiver.geometricNormal, direction) <= 0) continue;
            float cosine = abs(dot(receiver.normal, direction));
            float3 bsdf = index == 0u ? (1.0 / PI).xxx : EvaluateBsdf(receiver.material, receiver.normal, receiver.outgoing, direction);
            result += bsdf * (cosine * max(0, -dot(light.directionRange.xyz, direction)) * LightArea(light) / distanceSquared);
        }
    }
    result /= (192 * 192);
    // 连续面积积分不能表示离散分布，两个正入射极限另用 Fresnel 解析式验证。
    if (index == 16u) result += receiver.material.baseRoughness.rgb;
    if (index == 17u) {
        float eta = receiver.material.transmissionIor.y;
        float r = (eta - 1) / (eta + 1);
        result += sqrt(receiver.material.baseRoughness.rgb) * ((1 - r * r) / (eta * eta));
    }
    return result;
}

[numthreads(1, 1, 1)]
void LtcValidationMain(uint3 dispatchId : SV_DispatchThreadID)
{
    uint index = dispatchId.x;
    if (index >= 20u) return;
    Receiver receiver;
    Light light;
    LtcValidationCase(index, receiver, light);
    LtcValidationResult result;
    float3x3 identity = float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);
    float lambert = LtcPolygon(light, receiver.position, receiver.normal, receiver.geometricNormal, receiver.outgoing, identity);
    float3 integral = index == 0u ? lambert.xxx : LtcAreaIntegral(light, receiver);
    result.approximation = float4(integral, lambert);
    result.reference = float4(LtcValidationReference(receiver, light, index), 0);
    LtcResults[index] = result;
}
