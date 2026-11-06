#pragma once
#include "vrmath.h"

namespace ervr {

// Capture on the worker when the grab registers, before the game's two-hand
// chord or the inertial weapon catches up. Keep this offset until the next grab.
struct TwoHandGripOrientation {
    Quat relative; // supporting controller in weapon-controller space, VR axes

    void Capture(Quat weapon, Quat support) {
        relative = (weapon.Normalized().Conj() * support.Normalized()).Normalized();
    }

    // weapon is the rendered, inertial controller in game-world axes, BEFORE
    // hand calibration. Reflect the captured VR offset into the game frame and
    // apply calibration last, just as for an independently tracked hand.
    Quat InGame(Quat weapon, Quat calibration) const {
        const Quat offset{-relative.x, -relative.y, relative.z, relative.w};
        return (weapon * offset * calibration).Normalized();
    }
};

// Where the supporting hand's WRIST (its hand bone) goes so that its PALM lands on the weapon's shaft. A palm is the
// point the hand's weapon bone marks (R_Weapon / L_Weapon: where a held weapon sits), `grip` in that hand's own
// frame. The shaft runs through the weapon hand's palm along `axis` (up the blade); the supporting palm sits `along`
// m toward the pommel from it, plus `sideOffset` m along `side`. Placing the wrist instead (the weapon hand's wrist
// slid down the hilt) only put the palm on the shaft while both hands pointed the same way: with the supporting
// wrist kept at its own grab-time angle (2026-09-10) the palm swung up to two palm lengths off it (2026-11-06,
// "pinned to the shaft but with a weird offset").
inline Vec3 TwoHandPinnedWrist(const Vec3& weaponWrist, const Quat& weaponRot, const Vec3& weaponGrip, const Vec3& axis,
                               float along, const Vec3& side, float sideOffset, const Quat& supportRot,
                               const Vec3& supportGrip) {
    const Vec3 shaft = weaponWrist + weaponRot.Rotate(weaponGrip);   // the weapon hand's palm, on the shaft
    const Vec3 palm = shaft - axis * along + side * sideOffset;      // the supporting palm's place on it
    return palm - supportRot.Rotate(supportGrip);
}

} // namespace ervr
