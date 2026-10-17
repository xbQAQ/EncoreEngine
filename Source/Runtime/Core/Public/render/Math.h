#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace render {
struct Vec3 {
    float x{}; // Horizontal coordinate or red component.
    float y{}; // Vertical coordinate or green component.
    float z{}; // Depth coordinate or blue component.

    constexpr float operator[](std::size_t axis) const { return axis == 0 ? x : axis == 1 ? y : z; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    constexpr Vec3& operator+=(const Vec3& other) { x += other.x; y += other.y; z += other.z; return *this; }
    constexpr Vec3& operator-=(const Vec3& other) { x -= other.x; y -= other.y; z -= other.z; return *this; }
    constexpr Vec3& operator*=(float scale) { x *= scale; y *= scale; z *= scale; return *this; }
    constexpr Vec3& operator/=(float scale) { x /= scale; y /= scale; z /= scale; return *this; }
};

constexpr Vec3 operator+(Vec3 left, const Vec3& right) { return left += right; }
constexpr Vec3 operator-(Vec3 left, const Vec3& right) { return left -= right; }
constexpr Vec3 operator*(Vec3 value, float scale) { return value *= scale; }
constexpr Vec3 operator*(float scale, Vec3 value) { return value *= scale; }
constexpr Vec3 operator/(Vec3 value, float scale) { return value /= scale; }
constexpr float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(const Vec3& value) { return std::sqrt(Dot(value, value)); }
inline bool IsFinite(const Vec3& value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
inline Vec3 Normalize(const Vec3& value) {
    // Scaling first avoids overflowing the length of otherwise finite directions.
    const float scale = std::max({std::abs(value.x), std::abs(value.y), std::abs(value.z)});
    if (scale == 0 || !std::isfinite(scale)) return {};
    const Vec3 scaled = value / scale;
    return scaled / Length(scaled);
}

struct CameraBasis {
    Vec3 forward; // Unit view direction, +Z at identity.
    Vec3 right; // Unit horizontal view axis, +X at identity.
    Vec3 up; // Unit vertical view axis, +Y at identity.
};

inline CameraBasis MakeCameraBasis(float pitchDegrees, float yawDegrees, float rollDegrees) {
    constexpr double radians = 3.14159265358979323846 / 180.0;
    const double sx = std::sin(pitchDegrees * radians), cx = std::cos(pitchDegrees * radians);
    const double sy = std::sin(yawDegrees * radians), cy = std::cos(yawDegrees * radians);
    const double sz = std::sin(rollDegrees * radians), cz = std::cos(rollDegrees * radians);
    // Match mesh transforms: right-handed Rx, then Ry, then Rz; camera scale never changes its basis.
    const Vec3 forward{static_cast<float>(cz * sy * cx + sz * sx),
        static_cast<float>(sz * sy * cx - cz * sx), static_cast<float>(cy * cx)};
    const Vec3 right{static_cast<float>(cz * cy), static_cast<float>(sz * cy), static_cast<float>(-sy)};
    return {forward, right, Cross(forward, right)};
}

struct Ray {
    Vec3 origin; // World-space start of the ray.
    Vec3 direction; // Ray direction; unit length makes distance world-space meters.
    float tMin{0.0001f}; // Inclusive lower distance bound, excluding self intersections by default.
    float tMax{std::numeric_limits<float>::infinity()}; // Inclusive upper distance bound.
};

struct Hit {
    float distance{std::numeric_limits<float>::infinity()}; // Closest ray parameter; infinity denotes a miss.
    Vec3 position; // World-space intersection position.
    Vec3 normal; // Unit geometric normal oriented against the incoming ray.
    std::uint32_t materialIndex{}; // Index into the scene material array.
    std::uint32_t triangleIndex{}; // Index into the reordered triangle buffer.

    bool Found() const { return std::isfinite(distance); }
};

struct Aabb {
    Vec3 minimum{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()}; // Inclusive lower corner; default represents an empty box.
    Vec3 maximum{-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}; // Inclusive upper corner.

    void Expand(const Vec3& point) {
        minimum = {std::min(minimum.x, point.x), std::min(minimum.y, point.y), std::min(minimum.z, point.z)};
        maximum = {std::max(maximum.x, point.x), std::max(maximum.y, point.y), std::max(maximum.z, point.z)};
    }

    bool Intersects(const Ray& ray, float closestDistance) const {
        // The infinity-initialized empty box must not become a valid slab after swapping endpoints.
        if (minimum.x > maximum.x || minimum.y > maximum.y || minimum.z > maximum.z) return false;
        float nearDistance = ray.tMin;
        float farDistance = std::min(ray.tMax, closestDistance);
        // Explicit parallel slabs avoid the undefined zero-times-infinity case at box edges.
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (ray.direction[axis] == 0) {
                if (ray.origin[axis] < minimum[axis] || ray.origin[axis] > maximum[axis]) return false;
                continue;
            }
            const double inverse = 1.0 / ray.direction[axis];
            double nearAxis = (double(minimum[axis]) - ray.origin[axis]) * inverse;
            double farAxis = (double(maximum[axis]) - ray.origin[axis]) * inverse;
            if (nearAxis > farAxis) std::swap(nearAxis, farAxis);
            nearDistance = static_cast<float>(std::max(double(nearDistance), nearAxis));
            farDistance = static_cast<float>(std::min(double(farDistance), farAxis));
            if (nearDistance > farDistance) return false;
        }
        return nearDistance <= farDistance;
    }
};
}
