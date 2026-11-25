#pragma once

#include <cmath>
#include <cstdint>

namespace hd {

constexpr float PI = 3.14159265358979f;
constexpr float DEG_TO_RAD = PI / 180.0f;
constexpr float RAD_TO_DEG = 180.0f / PI;

/** A point or direction on the ground plane. x is right, z is forward (Jet's world axes). */
struct Vec2 {
    float x = 0.0f;
    float z = 0.0f;

    Vec2 operator+(const Vec2& o) const { return { x + o.x, z + o.z }; }
    Vec2 operator-(const Vec2& o) const { return { x - o.x, z - o.z }; }
    Vec2 operator*(float s) const { return { x * s, z * s }; }
    Vec2& operator+=(const Vec2& o) { x += o.x; z += o.z; return *this; }
    Vec2& operator-=(const Vec2& o) { x -= o.x; z -= o.z; return *this; }
    Vec2& operator*=(float s) { x *= s; z *= s; return *this; }
};

inline float dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.z * b.z; }
inline float length(const Vec2& v) { return std::sqrt(v.x * v.x + v.z * v.z); }
inline float lengthSquared(const Vec2& v) { return v.x * v.x + v.z * v.z; }

inline Vec2 normalised(const Vec2& v) {
    const float len = length(v);
    return len > 1e-6f ? v * (1.0f / len) : Vec2 { 0.0f, 1.0f };
}

/**
 * Headings are in radians, clockwise when seen from above: 0 faces +z, PI/2 faces +x.
 * This matches Jet's camera yaw.
 */
inline Vec2 headingToDirection(float heading) { return { std::sin(heading), std::cos(heading) }; }
inline float directionToHeading(const Vec2& dir) { return std::atan2(dir.x, dir.z); }
/** The direction 90 degrees clockwise (to the right) of a direction. */
inline Vec2 rightOf(const Vec2& dir) { return { dir.z, -dir.x }; }

/** Wraps an angle into [-PI, PI). */
inline float wrapAngle(float a) {
    while (a >= PI) a -= 2.0f * PI;
    while (a < -PI) a += 2.0f * PI;
    return a;
}

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

/** Moves value towards target by at most maxStep. */
inline float approach(float value, float target, float maxStep) {
    if (value < target) return value + maxStep > target ? target : value + maxStep;
    return value - maxStep < target ? target : value - maxStep;
}

/** Small deterministic PRNG (xorshift32), so races replay identically for a given seed. */
struct Random {
    uint32_t state = 0x9E3779B9u;

    explicit Random(uint32_t seed = 0x9E3779B9u) : state(seed != 0 ? seed : 0x9E3779B9u) {}

    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    /** @return a value in [0, 1) */
    float unit() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }
    /** @return a value in [lo, hi) */
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
};

} // namespace hd
