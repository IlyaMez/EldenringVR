#pragma once
#include "vrmath.h"

namespace ervr {

inline Vec3 WeaponShaftAxis(Quat calibration, bool handle) {
    return calibration.Rotate(handle ? Vec3{0, 1, 0} : Vec3{0, 0, 1}).Normalized();
}

inline Quat WeaponTwist(Quat q, Vec3 axis) {
    q = q.Normalized(); axis = axis.Normalized();
    const Vec3 v = axis * Vec3{q.x, q.y, q.z}.Dot(axis);
    return Quat{v.x, v.y, v.z, q.w}.Normalized();
}

inline Quat PreserveWeaponWristTwist(Quat orientation, Quat wrist, Vec3 shaftAxis) {
    const Quat relative = (wrist.Conj() * orientation).Normalized();
    return (wrist * relative * WeaponTwist(relative, shaftAxis).Conj()).Normalized();
}

inline Vec3 WeaponRotationVector(Quat q) {
    q = q.Normalized();
    if (q.w < 0.f) q = {-q.x, -q.y, -q.z, -q.w};
    const Vec3 v{q.x, q.y, q.z};
    const float n = v.Length();
    return n > 1e-6f ? v * (2.f * std::atan2(n, q.w) / n) : Vec3{};
}

inline Vec3 WeaponTipVelocity(Vec3 velocity, Vec3 angularVelocity, Quat orientation,
                              Vec3 shaftAxis, float reach) {
    return velocity + angularVelocity.Cross(orientation.Rotate(shaftAxis) * reach);
}

// Grip translation is tracked directly. Weight resists bending the shaft about
// the grip; rolling about the shaft stays under the main wrist's control.
// A monotone angular response has no stored spring velocity to fling the hands
// past the controller when it stops or reverses direction.
inline Quat StepWeaponOrientation(Quat current, Quat target, Vec3 shaftAxis, float dt,
                                 float weight, float reach, float com, float weightRef,
                                 float comRef, float springHz, float angularAccel,
                                 float torque, float brake, float maxLag) {
    target = target.Normalized();
    if (!(dt > 0.f && dt <= 0.1f) || weight <= 0.f) return target;
    Quat lag = (target.Conj() * current).Normalized();
    // Decompose in weapon space so the long lever does not resist edge alignment.
    lag = (lag * WeaponTwist(lag, shaftAxis).Conj()).Normalized();
    const Vec3 bend = WeaponRotationVector(lag);
    const float angle = bend.Length();
    if (angle < 1e-6f) return target;
    // Include distributed mass: a centre-gripped twinblade still resists bending.
    const float moment = std::fmax(com * com, reach * reach / 12.f);
    const float resistance = 1.f + std::fmax(weight, 0.f) / std::fmax(weightRef, 0.1f) *
                                  moment / (std::fmax(comRef, 0.05f) * std::fmax(comRef, 0.05f));
    const float assistance = std::fmax(torque, 0.1f);
    const float response = 6.2831853f * std::fmax(springHz, 0.5f) * std::sqrt(assistance / resistance);
    const float accel = std::fmax(angularAccel, 1.f) * assistance / resistance;
    const float step = std::fmin(angle * (1.f - std::exp(-response * dt)),
                                 std::sqrt(2.f * std::fmax(brake, 1.f) * accel * angle) * dt);
    // Old INIs allowed >110 degrees of separation. Keep the grip usable even
    // with those values; the tracking constraint wins over simulated resistance.
    const float remaining = Clamp(angle - step, 0.f, Clamp(maxLag, 0.1f, Deg2Rad(45.f)));
    return (target * Quat::AxisAngle(bend, remaining)).Normalized();
}

} // namespace ervr
