#ifndef RENDER_LTC_HLSLI
#define RENDER_LTC_HLSLI

// 矩阵与振幅遵循 selfshadow/ltc_code 的 64² GGX 表布局，原始许可见 ThirdParty/LTC。
// 算法出处：Heitz 等，Real-Time Polygonal-Light Shading with Linearly Transformed Cosines，SIGGRAPH 2016。
struct LtcPolygonStream
{
    float3 firstInput; // 几何半球裁剪后，多边形的第一个变换顶点。
    float3 lastInput; // 用于余弦半球裁剪的上一变换顶点。
    float3 firstVisible; // 余弦半球内第一个单位球面顶点。
    float3 lastVisible; // 用于球面边积分的上一单位球面顶点。
    float integral; // 有向积分，已除以 2π。
    uint inputCount; // 第一次输入前避免使用未初始化的闭合边。
    uint visibleCount; // 完全位于地平线下的多边形保持零贡献。
};
struct LtcShadingClip
{
    float3 first; // 着色半球裁剪前的第一个世界空间相对顶点。
    float3 last; // 着色半球裁剪前的上一世界空间相对顶点。
    uint count; // 闭合世界空间多边形时识别空输入。
};

// 球面边的解析积分保留符号；最终由发光面朝向检查决定单面发射，而非顶点绕序。
float LtcEdge(float3 a, float3 b)
{
    float3 perpendicular = cross(a, b);
    float sine = length(perpendicular);
    return sine > 1e-7 ? perpendicular.z * atan2(sine, clamp(dot(a, b), -1, 1)) / (2 * PI * sine) : 0;
}
void LtcVisibleVertex(inout LtcPolygonStream stream, float3 vertex)
{
    vertex = SafeNormalize(vertex);
    if (stream.visibleCount == 0u) stream.firstVisible = vertex;
    else stream.integral += LtcEdge(stream.lastVisible, vertex);
    stream.lastVisible = vertex;
    ++stream.visibleCount;
}
void LtcStreamVertex(inout LtcPolygonStream stream, float3 vertex)
{
    // 流式 Sutherland–Hodgman 裁剪只保留边端点，圆盘不需要每像素的大数组。
    if (stream.inputCount == 0u) {
        stream.firstInput = vertex;
        if (vertex.z > 0) LtcVisibleVertex(stream, vertex);
    } else {
        bool previousInside = stream.lastInput.z > 0;
        bool inside = vertex.z > 0;
        if (previousInside != inside) {
            float t = stream.lastInput.z / (stream.lastInput.z - vertex.z);
            LtcVisibleVertex(stream, lerp(stream.lastInput, vertex, t));
        }
        if (inside) LtcVisibleVertex(stream, vertex);
    }
    stream.lastInput = vertex;
    ++stream.inputCount;
}
float3 LtcVertex(Light light, uint index, uint count)
{
    float2 local;
    if (count == 4u) {
        local = index == 0u ? float2(-0.5, -0.5) : index == 1u ? float2(0.5, -0.5) :
            index == 2u ? float2(0.5, 0.5) : float2(-0.5, 0.5);
    } else {
        float angle = float(index) * (2 * PI / 64);
        local = 0.5 * float2(cos(angle), sin(angle));
    }
    return light.positionType.xyz + light.tangentWidth.xyz * (local.x * light.tangentWidth.w) +
        light.bitangentHeight.xyz * (local.y * light.bitangentHeight.w);
}
float3 LtcTransform(float3 relative, float3 tangent, float3 bitangent, float3 normal, float3x3 inverseMatrix)
{
    return mul(inverseMatrix, float3(dot(relative, tangent), dot(relative, bitangent), dot(relative, normal)));
}
void LtcShadingVertex(inout LtcShadingClip clip, inout LtcPolygonStream stream, float3 vertex,
    float3 tangent, float3 bitangent, float3 normal, float3x3 inverseMatrix)
{
    // LTC 拟合的支撑半球与真实着色半球并不完全重合，法线贴图时需要显式保留两者交集。
    float current = dot(vertex, normal);
    if (clip.count == 0u) clip.first = vertex;
    else {
        float previous = dot(clip.last, normal);
        if ((previous > 0) != (current > 0)) {
            float3 intersection = lerp(clip.last, vertex, previous / (previous - current));
            LtcStreamVertex(stream, LtcTransform(intersection, tangent, bitangent, normal, inverseMatrix));
        }
    }
    if (current > 0) LtcStreamVertex(stream, LtcTransform(vertex, tangent, bitangent, normal, inverseMatrix));
    clip.last = vertex;
    ++clip.count;
}
float LtcPolygon(Light light, float3 position, float3 normal, float3 geometricNormal, float3 outgoing, float3x3 inverseMatrix)
{
    // 依次裁实际三角形、着色法线和变换后的余弦半球，避免映射后的波瓣穿过几何表面。
    float3 tangent = outgoing - normal * dot(outgoing, normal);
    if (dot(tangent, tangent) < 1e-10)
        tangent = cross(abs(normal.z) < 0.999 ? float3(0, 0, 1) : float3(0, 1, 0), normal);
    tangent = SafeNormalize(tangent);
    float3 bitangent = cross(normal, tangent);
    uint count = uint(light.positionType.w + 0.5) == 3u ? 64u : 4u;
    LtcPolygonStream stream = (LtcPolygonStream)0;
    LtcShadingClip shadingClip = (LtcShadingClip)0;
    [loop] for (uint index = 0u; index < count; ++index) {
        float3 a = LtcVertex(light, index, count) - position;
        float3 b = LtcVertex(light, (index + 1u) % count, count) - position;
        float da = dot(a, geometricNormal), db = dot(b, geometricNormal);
        if (da >= 0) LtcShadingVertex(shadingClip, stream, a, tangent, bitangent, normal, inverseMatrix);
        if ((da >= 0) != (db >= 0)) {
            float3 intersection = lerp(a, b, da / (da - db));
            LtcShadingVertex(shadingClip, stream, intersection, tangent, bitangent, normal, inverseMatrix);
        }
    }
    if (shadingClip.count > 0u)
        LtcShadingVertex(shadingClip, stream, shadingClip.first, tangent, bitangent, normal, inverseMatrix);
    if (stream.inputCount == 0u) return 0;
    LtcStreamVertex(stream, stream.firstInput);
    if (stream.visibleCount < 3u) return 0;
    return min(1, abs(stream.integral + LtcEdge(stream.lastVisible, stream.firstVisible)));
}

// 求指定方向与真实矩形或六十四边圆盘的交点，供确定性瓣积分与离散镜面事件使用。
bool LtcEmitterHit(Light light, float3 position, float3 direction)
{
    float denominator = dot(light.directionRange.xyz, direction);
    if (denominator >= -1e-7) return false;
    float distance = dot(light.positionType.xyz - position, light.directionRange.xyz) / denominator;
    if (distance <= RAY_EPSILON) return false;
    float3 delta = position + direction * distance - light.positionType.xyz;
    float2 local = float2(dot(delta, light.tangentWidth.xyz) / max(light.tangentWidth.w, 1e-8),
        dot(delta, light.bitangentHeight.xyz) / max(light.bitangentHeight.w, 1e-8));
    if (uint(light.positionType.w + 0.5) != 3u) return all(abs(local) <= 0.5);
    float angle = atan2(local.y, local.x);
    float middle = (floor(angle * (64 / (2 * PI))) + 0.5) * (2 * PI / 64);
    return dot(local, float2(cos(middle), sin(middle))) <= 0.5 * cos(PI / 64);
}
float2 LtcQuadrature(uint index, uint count)
{
    // 固定低差异积分节点与帧序号、像素种子和每帧采样数无关。
    return float2((float(index) + 0.5) / float(count), float(reversebits(index)) * 2.3283064365386963e-10);
}
bool LtcAcceptedDirection(Light light, Receiver receiver, float3 direction)
{
    return dot(receiver.normal, direction) * dot(receiver.geometricNormal, direction) > 0 &&
        LtcEmitterHit(light, receiver.position, direction);
}

float3 LtcDiffuseCorrection(Light light, Receiver receiver, DisneyParameters p, DisneyFrame frame, float3 wo)
{
    // LTC 精确积分 Lambert 基线；固定面积求积补入 Burley 逆反射、局部次表面和绒毛。
    float3 result = 0;
    float3 baseline = p.color * (p.diffuseWeight * (1 - p.diffuseTransmission) / PI);
    [loop] for (uint index = 0u; index < 64u; ++index) {
        float3 delta = SampleEmitterPoint(light, float2((float(index % 8u) + 0.5) / 8, (float(index / 8u) + 0.5) / 8)) - receiver.position;
        float distanceSquared = max(dot(delta, delta), 1e-12);
        float3 direction = delta * rsqrt(distanceSquared);
        float3 wi = DisneyToLocal(frame, direction);
        if (wi.z <= 1e-7 || dot(receiver.geometricNormal, direction) <= 0) continue;
        float cosineHalf = saturate(dot(wo, SafeNormalize(wo + wi)));
        float3 correction = DisneyDiffuse(p, wo, wi, cosineHalf) - baseline;
        result += correction * (wi.z * max(0, -dot(light.directionRange.xyz, direction)) * LightArea(light) / distanceSquared);
    }
    return result / 64;
}
float3 LtcReflectionCorrection(Light light, Receiver receiver, DisneyParameters p, DisneyFrame frame, float3 wo,
    bool fittedBase, float3x3 inverseMatrix, float3 fittedAmplitude)
{
    float3 result = 0;
    float gOut = DisneySmithG1(wo, p.alpha);
    float3 tangent = receiver.outgoing - receiver.normal * dot(receiver.outgoing, receiver.normal);
    if (dot(tangent, tangent) < 1e-10)
        tangent = cross(abs(receiver.normal.z) < 0.999 ? float3(0, 0, 1) : float3(0, 1, 0), receiver.normal);
    tangent = SafeNormalize(tangent);
    float3 bitangent = cross(receiver.normal, tangent);
    float inverseDeterminant = abs(determinant(inverseMatrix));
    // 解析 LTC 是控制变量；直接积分 Disney 与实际拟合分布之差，同时补偿形状、Fresnel 与 Smith 差异。
    // 仅减物理相关 Smith 基线会漏掉上游拟合误差，圆盘掠射时该误差可超过真实反射的百分之八。
    // 各向异性不冒用各向同性表，其整个基础 GGX 瓣由同一确定性 VNDF 求积保留。
    [loop] for (uint index = 0u; index < 128u; ++index) {
        float3 h = DisneySampleVisibleNormal(wo, p.alpha, LtcQuadrature(index, 128u));
        float3 wi = reflect(-wo, h);
        if (wi.z <= 1e-7) continue;
        float3 direction = DisneyToWorld(frame, wi);
        if (!LtcAcceptedDirection(light, receiver, direction)) continue;
        float gIn = DisneySmithG1(wi, p.alpha);
        float3 weight = DisneyReflectionFresnel(p, dot(wo, h)) * gIn;
        if (fittedBase) {
            float3 transformed = LtcTransform(direction, tangent, bitangent, receiver.normal, inverseMatrix);
            float lengthSquared = max(dot(transformed, transformed), 1e-20);
            float density = inverseDeterminant * max(transformed.z, 0) / (PI * lengthSquared * lengthSquared);
            float pdf = DisneyGgxD(h, p.alpha) * gOut / max(4 * wo.z, 1e-20);
            weight -= fittedAmplitude * (density / max(pdf, 1e-20));
        }
        result += weight;
    }
    return result / 128;
}
float3 LtcCoatIntegral(Light light, Receiver receiver, DisneyParameters p, DisneyFrame frame, float3 wo)
{
    // 清漆是独立 GTR1 瓣，不能将 GGX 振幅表直接当作清漆表。
    float result = 0;
    [loop] for (uint index = 0u; index < 128u; ++index) {
        float3 h = DisneySampleGtr1(p.coatAlpha, LtcQuadrature(index, 128u));
        float oh = dot(wo, h);
        float3 wi = reflect(-wo, h);
        if (oh <= 1e-8 || wi.z <= 1e-7 || !LtcAcceptedDirection(light, receiver, DisneyToWorld(frame, wi))) continue;
        float g = DisneySmithG1(wo, float2(0.25, 0.25)) * DisneySmithG1(wi, float2(0.25, 0.25));
        result += p.coat * (0.04 + 0.96 * DisneyPow5(1 - oh)) * g * oh / max(wo.z * h.z, 1e-10);
    }
    return (result / 128).xxx;
}
float3 LtcTransmissionIntegral(Light light, Receiver receiver, DisneyParameters p, DisneyFrame frame, float3 wo)
{
    // 粗糙 BTDF 按自身折射 VNDF 求积；绝不将反射 LTC 贴到透射半球。
    float3 result = 0;
    [loop] for (uint index = 0u; index < 128u; ++index) {
        float3 h = DisneySampleVisibleNormal(wo, p.transmissionAlpha, LtcQuadrature(index, 128u));
        float3 wi = refract(-wo, h, rcp(p.eta));
        if (dot(wi, wi) < 0.5 || wi.z >= -1e-7 || !LtcAcceptedDirection(light, receiver, DisneyToWorld(frame, wi))) continue;
        float fresnel = DielectricFresnel(dot(wo, h), p.etaI, p.etaT);
        result += p.transmissionColor * ((1 - p.metallic) * p.specularTransmission * (1 - fresnel) *
            DisneySmithG1(wi, p.transmissionAlpha) / (p.eta * p.eta));
    }
    return result / 128;
}
float3 LtcDeltaIntegral(Light light, Receiver receiver, DisneyParameters p, DisneyFrame frame, float3 wo)
{
    // 零粗糙度是离散分布；直接测试镜面和 Snell 方向，避免粗糙表或面积网格漏掉尖峰。
    float3 result = 0;
    float3 reflected = DisneyToWorld(frame, float3(-wo.xy, wo.z));
    if (LtcAcceptedDirection(light, receiver, reflected)) result += DisneyReflectionFresnel(p, wo.z);
    float3 wi = refract(-wo, float3(0, 0, 1), rcp(p.eta));
    if (p.specularTransmission > 0 && p.metallic < 1 && dot(wi, wi) > 0.5 &&
        LtcAcceptedDirection(light, receiver, DisneyToWorld(frame, wi)))
        result += p.transmissionColor * ((1 - p.metallic) * p.specularTransmission *
            (1 - DielectricFresnel(wo.z, p.etaI, p.etaT)) / (p.eta * p.eta));
    return result;
}
float3 LtcAreaIntegral(Light light, Receiver receiver)
{
    // 整个平面共用发射侧，背面和退化发光形状在任何求积之前统一拒绝。
    if (dot(light.directionRange.xyz, receiver.position - light.positionType.xyz) <= 1e-6 ||
        min(light.tangentWidth.w, light.bitangentHeight.w) <= 1e-6) return 0;
    DisneyFrame frame = DisneyMakeFrame(receiver.material, receiver.normal);
    DisneyParameters p = DisneyGetParameters(receiver.material);
    float3 wo = DisneyToLocal(frame, receiver.outgoing);
    if (wo.z <= 1e-7) return 0;
    float3x3 identity = float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);
    float diffuse = LtcPolygon(light, receiver.position, receiver.normal, receiver.geometricNormal, receiver.outgoing, identity);
    float3 result = p.color * (p.diffuseWeight * (1 - p.diffuseTransmission) * diffuse);
    if (p.diffuseTransmission > 0)
        result += p.color * (p.diffuseWeight * p.diffuseTransmission *
            LtcPolygon(light, receiver.position, -receiver.normal, -receiver.geometricNormal, receiver.outgoing, identity));
    bool fittedBase = p.smooth == 0u && abs(receiver.material.principled.z) < 1e-5;
    float3x3 inverseMatrix = identity;
    float3 fittedAmplitude = 0;
    if (fittedBase) {
        float2 uv = float2(p.roughness, sqrt(saturate(1 - wo.z))) * (63.0 / 64.0) + (0.5 / 64.0);
        float4 transformData = LtcMatrix.SampleLevel(LinearClamp, uv, 0);
        float4 amplitude = LtcAmplitude.SampleLevel(LinearClamp, uv, 0);
        // 上游 GLSL 按列构造；这里显式写成 HLSL 的行，不能直接照抄以免交换离轴分量。
        inverseMatrix = float3x3(transformData.x, 0, transformData.z, 0, 1, 0, transformData.y, 0, transformData.w);
        float specular = LtcPolygon(light, receiver.position, receiver.normal, receiver.geometricNormal, receiver.outgoing, inverseMatrix);
        float3 f0 = lerp(p.dielectricR0, p.color, p.metallic);
        fittedAmplitude = f0 * amplitude.x + (1 - f0) * amplitude.y;
        result += specular * fittedAmplitude;
    }
    if (p.diffuseWeight > 0 || MaxComponent(p.sheenColor) > 0) result += LtcDiffuseCorrection(light, receiver, p, frame, wo);
    if (p.smooth != 0u) result += LtcDeltaIntegral(light, receiver, p, frame, wo);
    else result += LtcReflectionCorrection(light, receiver, p, frame, wo, fittedBase, inverseMatrix, fittedAmplitude);
    if (p.coat > 0) result += LtcCoatIntegral(light, receiver, p, frame, wo);
    if (p.smooth == 0u && p.specularTransmission > 0 && p.metallic < 1)
        result += LtcTransmissionIntegral(light, receiver, p, frame, wo);
    // 额外瓣使用有限确定性求积，仍存在离散化误差；夹取只处理表拟合与差额求积的微小负值。
    return max(0, result);
}

LightSample LtcCenterSample(Light light, uint index, float3 position)
{
    LightSample sample = (LightSample)0;
    sample.index = index;
    sample.radiance = max(light.colorIntensity.xyz, 0) * max(light.colorIntensity.w, 0);
    uint type = uint(light.positionType.w + 0.5);
    if (type == 0u) {
        sample.direction = -SafeNormalize(light.directionRange.xyz);
        sample.distance = MAX_RAY_DISTANCE;
    } else {
        float3 delta = light.positionType.xyz - position;
        float distanceSquared = dot(delta, delta);
        if (distanceSquared <= 1e-8) return sample;
        sample.distance = sqrt(distanceSquared);
        sample.direction = delta / sample.distance;
        if (type == 1u || type == 4u)
            sample.radiance *= RangeAttenuation(sample.distance, light.directionRange.w) / distanceSquared;
        if (type == 4u) {
            float factor = saturate((dot(light.directionRange.xyz, -sample.direction) - light.parameters.y) /
                max(light.parameters.z - light.parameters.y, 1e-6));
            sample.radiance *= factor * factor;
        }
    }
    sample.valid = MaxComponent(sample.radiance) > 0 ? 1u : 0u;
    return sample;
}
float3 LtcDirectLighting(Receiver receiver)
{
    // 全光源枚举没有随机光源选择、面积位置或逐帧种子，点/方向/聚光仍求完整 Disney BSDF。
    float3 result = 0;
    [loop] for (uint index = 0u; index < Lighting.x; ++index) {
        Light light = Lights[index];
        uint type = uint(light.positionType.w + 0.5);
        LightSample center = LtcCenterSample(light, index, receiver.position);
        if (center.valid == 0u) continue;
        float3 integral;
        if (type == 2u || type == 3u) integral = LtcAreaIntegral(light, receiver);
        else {
            if (dot(receiver.normal, center.direction) * dot(receiver.geometricNormal, center.direction) <= 0) continue;
            integral = EvaluateBsdf(receiver.material, receiver.normal, receiver.outgoing, center.direction) * abs(dot(receiver.normal, center.direction));
        }
        // PCF/PCSS/VSM 是中心方向可见度乘完整面积积分的有偏近似，并非对面积可见性的积分。
        result += integral * center.radiance * LightVisibility(center, receiver.position, receiver.geometricNormal);
    }
    return result;
}
#endif
