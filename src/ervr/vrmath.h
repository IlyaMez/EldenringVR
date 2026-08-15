#pragma once
#include <cmath>

namespace ervr {

struct Vec3 { float x = 0, y = 0, z = 0;
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float Length() const { return std::sqrt(x * x + y * y + z * z); }
    float Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 Normalized() const { float l = Length(); return l > 1e-6f ? (*this) * (1.f / l) : Vec3{}; }
};

// Unit quaternion, (x,y,z,w), right-handed, Y-up (Oculus/OpenVR convention: -Z forward).
struct Quat { float x = 0, y = 0, z = 0, w = 1;
    Vec3 Rotate(const Vec3& v) const {
        // v' = v + 2w(q x v) + 2 q x (q x v)
        Vec3 q{x, y, z};
        Vec3 t{ 2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x) };
        Vec3 qxt{ q.y * t.z - q.z * t.y, q.z * t.x - q.x * t.z, q.x * t.y - q.y * t.x };
        return v + t * w + qxt;
    }
    Vec3 Forward() const { return Rotate({0, 0, -1}); }
    Vec3 Up() const { return Rotate({0, 1, 0}); }
    // Yaw about +Y in radians, in [-pi, pi]; 0 = facing -Z.
    float Yaw() const { Vec3 f = Forward(); return std::atan2(-f.x, -f.z); }
    float Pitch() const { Vec3 f = Forward(); return std::asin(std::fmax(-1.f, std::fmin(1.f, f.y))); }
};

struct Pose { Vec3 pos; Quat rot; Vec3 linVel; Vec3 angVel; bool valid = false; };

inline float WrapPi(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }
inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float Deg2Rad(float d) { return d * 0.017453292f; }
inline float Rad2Deg(float r) { return r * 57.29578f; }
}
