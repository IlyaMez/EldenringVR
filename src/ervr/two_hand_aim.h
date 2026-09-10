#pragma once
#include "weapon_motion.h"

namespace ervr {

// All vectors are in play space. The separation at grab time includes the physical
// controllers' lateral offset: taking hold must not rotate the weapon to remove it.
// Owned by the inertial arm and stepped under its lock, once per rendered frame.
struct TwoHandAimState {
    bool held = false;
    Vec3 gripLine;
    Quat correction;

    static Vec3 RotationVector(Quat q) {
        q = q.Normalized();
        if (q.w < 0.f) q = {-q.x, -q.y, -q.z, -q.w};
        const Vec3 v{q.x, q.y, q.z};
        const float n = v.Length();
        return n > 1e-6f ? v * (2.f * std::atan2(n, q.w) / n) : Vec3{};
    }

    Quat Update(Quat wrist, Vec3 line, bool grabbed, float dt, Vec3 shaftAxis = {0, 1, 0}) {
        wrist = wrist.Normalized();
        if (!(dt > 0.f && dt <= 0.1f)) { held = false; correction = {}; }
        const float length = line.Length();
        if (!grabbed) held = false;
        // Coincident controllers do not define an aiming direction. Keep the last
        // correction through a short overlap; acquire only once they separate.
        if (grabbed && !held && length >= 0.06f) {
            gripLine = (wrist * correction).Conj().Rotate(line * (1.f / length));
            held = true;
        }
        Quat target = grabbed ? correction : Quat{};
        if (grabbed && held && length >= 0.06f) {
            Vec3 direction = wrist.Conj().Rotate(line * (1.f / length));
            // Near an antipodal line the shortest rotation has no stable axis.
            // Hold through it instead of choosing an arbitrary 180-degree flip.
            if (gripLine.Dot(direction) > -0.95f) {
                Quat steer = Quat::FromTo(gripLine, direction);
                // Hand positions define bending, never edge/shaft twist. The
                // lateral controller offset used to contribute a roll correction
                // that fought the main wrist. Remove it in the actual shaft frame.
                steer = (steer * WeaponTwist(steer, shaftAxis).Conj()).Normalized();
                Vec3 rv = RotationVector(steer);
                const float angle = rv.Length();
                if (angle > 1e-6f)
                    target = Quat::AxisAngle(rv, Clamp(angle, 0.f, Deg2Rad(80.f)));
                else target = {};
            }
        }
        // Smooth both steering and release even with inertia disabled or a light
        // weapon. Limit correction speed, while ordinary wrist motion stays 1:1.
        if (dt > 0.f && dt <= 0.1f) {
            const Vec3 delta = RotationVector(target * correction.Conj());
            const float angle = delta.Length();
            if (angle > 1e-6f) {
                const float step = Clamp(angle * (1.f - std::exp(-dt / 0.045f)), 0.f, Deg2Rad(360.f) * dt);
                correction = (Quat::AxisAngle(delta, step) * correction).Normalized();
            } else correction = target;
        }
        correction = (correction * WeaponTwist(correction, shaftAxis).Conj()).Normalized();
        return (wrist * correction).Normalized();
    }

    bool Active() const { return held || RotationVector(correction).Length() > 1e-5f; }
};

} // namespace ervr
