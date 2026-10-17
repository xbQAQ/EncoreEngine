#ifndef RENDER_SCENE_QUERIES_HLSLI
#define RENDER_SCENE_QUERIES_HLSLI

// 纯光栅排列完全移除遍历，避免通道隐式回退到 BVH 或 DXR 查询。
#if !RASTER_ONLY
bool AcceptTriangle(Triangle primitiveData, float2 barycentric, float3 direction)
{
    Material material = GetMaterial(uint(primitiveData.a.w + 0.5));
    float3 geometric = cross(primitiveData.b.xyz - primitiveData.a.xyz, primitiveData.c.xyz - primitiveData.a.xyz);
    if (material.flags.y == 0u && material.transmissionIor.x <= 0 && dot(geometric, direction) >= 0) return false;
    return material.flags.z != 1u || SampleMaterialAlpha(primitiveData, barycentric) >= material.extras.z;
}
bool IntersectBox(float3 origin, float3 direction, float3 minimum, float3 maximum, float limit)
{
    // RayOrigin 已偏移次级光线，再加近裁剪会漏掉紧邻的遮挡面。
    float nearest = 0.0, farthest = limit;
    // 显式处理平行轴，避免包围盒边界出现零乘无穷大的未定义结果。
    [unroll] for (uint axis = 0; axis < 3; ++axis) {
        if (abs(direction[axis]) < 1e-8) { if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis]) return false; }
        else {
            float2 interval = float2(minimum[axis] - origin[axis], maximum[axis] - origin[axis]) / direction[axis];
            nearest = max(nearest, min(interval.x, interval.y));
            farthest = min(farthest, max(interval.x, interval.y));
            if (nearest > farthest) return false;
        }
    }
    return true;
}
bool IntersectTriangle(float3 origin, float3 direction, Triangle primitiveData, float limit, out float distance, out float2 barycentric)
{
    float3 edge1 = primitiveData.b.xyz - primitiveData.a.xyz, edge2 = primitiveData.c.xyz - primitiveData.a.xyz;
    float3 p = cross(direction, edge2), t = origin - primitiveData.a.xyz;
    float determinant = dot(edge1, p);
    distance = limit; barycentric = 0;
    if (abs(determinant) < 1e-8) return false;
    float inverse = rcp(determinant);
    float3 q = cross(t, edge1);
    barycentric = float2(dot(t, p), dot(direction, q)) * inverse;
    if (any(barycentric < 0) || barycentric.x + barycentric.y > 1) return false;
    distance = dot(edge2, q) * inverse;
    // 与 DXR TMin 一样包含零距离边界，接缝处相邻面可能恰好触及偏移光线。
    return distance >= 0.0 && distance < limit && AcceptTriangle(primitiveData, barycentric, direction);
}
Hit TraceScene(float3 origin, float3 direction, float limit)
{
    Hit hit = (Hit)0;
    hit.distance = limit;
    float2 barycentric = 0;
#if USE_DXR
    RayDesc ray;
    ray.Origin = origin; ray.Direction = direction; ray.TMin = 0.0; ray.TMax = limit;
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(SceneTlas, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed()) {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            AcceptTriangle(Triangles[query.CandidatePrimitiveIndex()], query.CandidateTriangleBarycentrics(), direction)) query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        hit.found = 1u; hit.distance = query.CommittedRayT(); hit.primitive = query.CommittedPrimitiveIndex();
        barycentric = query.CommittedTriangleBarycentrics();
    }
#else
    uint stack[64]; uint count = 0u;
    bool overflow = false;
    if (Counts.y > 0u) stack[count++] = 0u;
    [loop] while (count > 0u) {
        uint index = stack[--count];
        if (index >= Counts.y) { overflow = true; break; }
        BvhNode node = BvhNodes[index];
        if (!IntersectBox(origin, direction, node.minimum.xyz, node.maximum.xyz, hit.distance)) continue;
        if (node.links.w > 0u) {
            [loop] for (uint j = 0u; j < node.links.w; ++j) {
                uint primitive = node.links.z + j;
                float distance; float2 weights;
                if (primitive < Counts.x && IntersectTriangle(origin, direction, Triangles[primitive], hit.distance, distance, weights)) {
                    hit.found = 1u; hit.distance = distance; hit.primitive = primitive; barycentric = weights;
                }
            }
        } else {
            if (count + 2u > 64u) { overflow = true; break; }
            stack[count++] = node.links.x; stack[count++] = node.links.y;
        }
    }
    // 异常或超深树回退到遍历全部原语，避免静默漏掉几何。
    if (overflow || Counts.y == 0u) {
        [loop] for (uint primitive = 0u; primitive < Counts.x; ++primitive) {
            float distance; float2 weights;
            if (IntersectTriangle(origin, direction, Triangles[primitive], hit.distance, distance, weights)) {
                hit.found = 1u; hit.distance = distance; hit.primitive = primitive; barycentric = weights;
            }
        }
    }
#endif
    if (hit.found != 0u) {
        Triangle primitiveData = Triangles[hit.primitive];
        float3 geometric = SafeNormalize(cross(primitiveData.b.xyz - primitiveData.a.xyz, primitiveData.c.xyz - primitiveData.a.xyz));
        hit.frontFace = dot(geometric, direction) < 0 ? 1u : 0u;
        hit.geometricNormal = hit.frontFace != 0u ? geometric : -geometric;
        hit.normal = geometric;
        hit.tangent = ToWorld(float3(1, 0, 0), geometric);
        hit.handedness = 1;
        hit.uv = TriangleUV(primitiveData, barycentric);
        if (primitiveData.b.w >= 1 && primitiveData.b.w <= float(Assets.x)) {
            TriangleAttributes attribute = Attributes[uint(primitiveData.b.w - 1)];
            hit.normal = SafeNormalize(attribute.normal0.xyz * (1 - barycentric.x - barycentric.y) + attribute.normal1.xyz * barycentric.x + attribute.normal2.xyz * barycentric.y);
            if (dot(hit.normal, geometric) < 0) hit.normal = -hit.normal;
            hit.tangent = attribute.tangent.xyz;
            hit.handedness = attribute.tangent.w;
        }
        if (hit.frontFace == 0u) hit.normal = -hit.normal;
        // 光线参数重建也可能越过相邻面，沿用光栅表面位置的边界约束。
        hit.position = TrianglePosition(primitiveData, float3(1 - barycentric.x - barycentric.y, barycentric));
        hit.material = uint(primitiveData.a.w + 0.5);
        hit.normal = EvaluateNormal(GetMaterial(hit.material), hit.uv, hit.normal, float4(hit.tangent, hit.handedness), hit.geometricNormal);
    }
    return hit;
}
bool SegmentVisible(float3 position, float3 normal, float3 target)
{
    if (Flags.w == 0u) return true;
    float3 delta = target - position;
    float distance = length(delta);
    if (distance < 4 * RAY_EPSILON) return false;
    float3 direction = delta / distance;
    return TraceScene(RayOrigin(position, normal, direction), direction, distance - 3 * RAY_EPSILON).found == 0u;
}
#endif
#endif
