#ifndef MATH_H
#define MATH_H

#include <cmath>
#include <algorithm>

// ======================================================
// Math Constants & Helpers
// ======================================================

static constexpr double PI = 3.14159265358979323846;
static constexpr float DEG_TO_RAD = float(PI / 180.0);
static constexpr float RAD_TO_DEG = float(180.0 / PI);

// ======================================================
// Vector2
// ======================================================

struct Vector2 {
    float x = 0, y = 0;

    Vector2 operator+(const Vector2& o) const { return { x + o.x, y + o.y }; }
    Vector2 operator-(const Vector2& o) const { return { x - o.x, y - o.y }; }
    Vector2 operator*(float s) const { return { x * s, y * s }; }
    Vector2 operator/(float s) const { return { x / s, y / s }; }
    Vector2& operator+=(const Vector2& o) { x += o.x; y += o.y; return *this; }

    static float Distance(Vector2 a, Vector2 b) {
        return std::hypot(b.x - a.x, b.y - a.y);
    }

    static Vector2 Normalize(Vector2 v) {
        float len = std::hypot(v.x, v.y);
        return len < 1e-6f ? Vector2{ 0,0 } : Vector2{ v.x / len, v.y / len };
    }

    static float Length(Vector2 v) {
        return std::hypot(v.x, v.y);
    }

    static Vector2 Lerp(Vector2 a, Vector2 b, float t) {
        return {
            a.x + t * (b.x - a.x),
            a.y + t * (b.y - a.y)
        };
    }

    static Vector2 FromAngleDegrees(float deg) {
        float r = deg * DEG_TO_RAD;
        return { std::cos(r), std::sin(r) };
    }

    Vector2 Rotate(float deg) const {
        float rad = deg * DEG_TO_RAD;
        float c = std::cos(rad);
        float s = std::sin(rad);
        return {
            x * c - y * s,
            x * s + y * c
        };
    }
};

// ======================================================
// Scalar Helpers
// ======================================================

inline float Lerp(float a, float b, float t) {
    return a + t * (b - a);
}

inline float CubicEaseInOut(float p) {
    if (p < 0.5f) return 4.0f * p * p * p;
    float f = (2.0f * p) - 2.0f;
    return 0.5f * f * f * f + 1.0f;
}

// ======================================================
// Added Helpers (Safe Extensions)
// ======================================================

inline float Clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float Dot(const Vector2& a, const Vector2& b) {
    return a.x * b.x + a.y * b.y;
}

#endif // MATH_H
