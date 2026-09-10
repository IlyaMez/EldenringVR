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

} // namespace ervr
