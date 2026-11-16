#pragma once
// Locked on, which way the room faces.
//
// Locked on, the game picks the direction (the target) and ERVR turns the play space so the target holds still in
// the room. Up to 0.9.1 that held the target at the play area's own forward direction, the one the runtime took
// when the play area was drawn: a player facing anywhere else in the room found the target pinned off to the side
// (2026-11-15 report, PSVR2 with its play area drawn toward a side wall: every lock put the enemy 90 degrees to the
// left, "if I face that wall it works fine"). The target now stays where the player looked when the lock engaged:
// the head's yaw in the play space at the lock's first frame is kept for the whole lock.
#include "vrmath.h"

namespace ervr {

struct LockOnFacing {
    bool  latched = false;
    float facing = 0.f;   // play-space yaw the target is held at (game convention, radians, positive = right)

    // Every frame build. `locked`: a target is locked. `headYaw`/`headValid`: the head's yaw in the play space.
    // `hmdView`: the view carries the head's yaw (CameraRotation=hmd); without it the view looks along the play
    // space's forward, so that is where the target belongs. Returns the yaw to hold the target at (0 unlocked).
    float Update(bool locked, float headYaw, bool headValid, bool hmdView) {
        if (!locked) { latched = false; facing = 0.f; return 0.f; }
        if (!latched) { facing = (headValid && hmdView) ? WrapPi(headYaw) : 0.f; latched = true; }
        return facing;
    }
};

// The play space's world yaw that puts a target lying along `targetYaw` (world) at play-space yaw `facing`.
inline float LockOnPlaySpaceYaw(float targetYaw, float facing) { return WrapPi(targetYaw - facing); }

}
