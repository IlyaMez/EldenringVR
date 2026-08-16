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
    Vec3 Cross(const Vec3& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
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

    Quat operator*(const Quat& b) const {   // this * b  (apply b first, then this)
        return { w * b.x + x * b.w + y * b.z - z * b.y,
                 w * b.y - x * b.z + y * b.w + z * b.x,
                 w * b.z + x * b.y - y * b.x + z * b.w,
                 w * b.w - x * b.x - y * b.y - z * b.z };
    }
    Quat Conj() const { return {-x, -y, -z, w}; }
    float Norm() const { return std::sqrt(x * x + y * y + z * z + w * w); }
    Quat Normalized() const { float n = Norm(); return n > 1e-8f ? Quat{x / n, y / n, z / n, w / n} : Quat{}; }
    static Quat AxisAngle(const Vec3& axisIn, float rad) {
        Vec3 a = axisIn.Normalized(); float s = std::sin(rad * 0.5f);
        return {a.x * s, a.y * s, a.z * s, std::cos(rad * 0.5f)};
    }
    static Quat YawQ(float rad) { return AxisAngle({0, 1, 0}, rad); }
    // Minimal rotation taking unit vector a onto unit vector b.
    static Quat FromTo(const Vec3& a, const Vec3& b) {
        float d = a.Dot(b);
        if (d > 0.999999f) return {};
        if (d < -0.999999f) {
            Vec3 axis = Vec3{1, 0, 0}.Cross(a); if (axis.Length() < 1e-4f) axis = Vec3{0, 1, 0}.Cross(a);
            return AxisAngle(axis, 3.14159265f);
        }
        Vec3 c = a.Cross(b);
        Quat q{c.x, c.y, c.z, 1.f + d};
        return q.Normalized();
    }
    static Quat FromEulerYXZ(float yaw, float pitch, float roll) { return YawQ(yaw) * AxisAngle({1, 0, 0}, pitch) * AxisAngle({0, 0, 1}, roll); }
    // Quaternion whose Rotate() of the unit axes gives the orthonormal basis X,Y,Z.
    static Quat FromBasis(const Vec3& X, const Vec3& Y, const Vec3& Z) {
        float m00 = X.x, m10 = X.y, m20 = X.z, m01 = Y.x, m11 = Y.y, m21 = Y.z, m02 = Z.x, m12 = Z.y, m22 = Z.z;
        float tr = m00 + m11 + m22; Quat q;
        if (tr > 0) { float s = std::sqrt(tr + 1.f) * 2.f; q.w = 0.25f * s; q.x = (m21 - m12) / s; q.y = (m02 - m20) / s; q.z = (m10 - m01) / s; }
        else if (m00 > m11 && m00 > m22) { float s = std::sqrt(1.f + m00 - m11 - m22) * 2.f; q.w = (m21 - m12) / s; q.x = 0.25f * s; q.y = (m01 + m10) / s; q.z = (m02 + m20) / s; }
        else if (m11 > m22) { float s = std::sqrt(1.f + m11 - m00 - m22) * 2.f; q.w = (m02 - m20) / s; q.x = (m01 + m10) / s; q.y = 0.25f * s; q.z = (m12 + m21) / s; }
        else { float s = std::sqrt(1.f + m22 - m00 - m11) * 2.f; q.w = (m10 - m01) / s; q.x = (m02 + m20) / s; q.y = (m12 + m21) / s; q.z = 0.25f * s; }
        return q.Normalized();
    }
    // Rotation mapping the local pair (f,u) onto the target pair (F,U): R*f = F, R*u ~ U.
    static Quat MapPairs(Vec3 f, Vec3 u, Vec3 F, Vec3 U) {
        f = f.Normalized(); u = (u - f * u.Dot(f)).Normalized(); Vec3 w = f.Cross(u);
        F = F.Normalized(); U = (U - F * U.Dot(F)).Normalized(); Vec3 W = F.Cross(U);
        return (FromBasis(F, U, W) * FromBasis(f, u, w).Conj()).Normalized();
    }
    // Rotation matrix rows (basis vectors): X = Rotate(1,0,0) etc.
};

struct Pose { Vec3 pos; Quat rot; Vec3 linVel; Vec3 angVel; bool valid = false; };

inline float WrapPi(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }
inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float Deg2Rad(float d) { return d * 0.017453292f; }
inline float Rad2Deg(float r) { return r * 57.29578f; }
}
