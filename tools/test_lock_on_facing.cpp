#include "lock_on_facing.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
using ervr::LockOnFacing;
using ervr::LockOnPlaySpaceYaw;
using ervr::WrapPi;

constexpr float kDeg = 3.14159265f / 180.f;

void Near(float actual, float expected, const char* behavior) {
    if (!std::isfinite(actual) || std::fabs(WrapPi(actual - expected)) > 0.0001f) {
        std::fprintf(stderr, "%s: expected %.4f deg, got %.4f deg\n", behavior, expected / kDeg, actual / kDeg);
        std::exit(1);
    }
}

// The view's world yaw is the play space's yaw plus the head's (CameraRotation=hmd).
float ViewYaw(float playSpaceYaw, float headYaw) { return WrapPi(playSpaceYaw + headYaw); }

void PlayAreaDrawnTowardASideWall() {
    // The 2026-11-15 report: the player faces 90 degrees right of the play area's forward and locks onto an enemy
    // straight ahead of them. The enemy must stay ahead, not swing to the play area's forward (their left wall).
    LockOnFacing lock;
    const float head = 90.f * kDeg, enemy = 30.f * kDeg;
    const float facing = lock.Update(true, head, true, true);
    const float playSpace = LockOnPlaySpaceYaw(enemy, facing);
    Near(ViewYaw(playSpace, head), enemy, "looking where you locked, the enemy is straight ahead");
    Near(ViewYaw(playSpace, 0.f), enemy - 90.f * kDeg, "the play area's forward is the side wall, not the enemy");
}

void HeldForTheWholeLock() {
    LockOnFacing lock;
    lock.Update(true, 20.f * kDeg, true, true);
    // Looking around during the lock does not move where the enemy is held.
    Near(lock.Update(true, -70.f * kDeg, true, true), 20.f * kDeg, "a glance keeps the latched facing");
    Near(lock.Update(true, 160.f * kDeg, true, true), 20.f * kDeg, "turning away keeps the latched facing");
    // The enemy moves around the player: the room turns so it stays at the latched facing.
    for (float enemy : {0.f, 75.f, -120.f, 179.f}) {
        const float playSpace = LockOnPlaySpaceYaw(enemy * kDeg, lock.facing);
        Near(ViewYaw(playSpace, 20.f * kDeg), enemy * kDeg, "the enemy stays at the latched facing");
    }
}

void ReleaseAndRelock() {
    LockOnFacing lock;
    lock.Update(true, 40.f * kDeg, true, true);
    Near(lock.Update(false, 40.f * kDeg, true, true), 0.f, "unlocked: no facing");
    if (lock.latched) { std::fprintf(stderr, "release must clear the latch\n"); std::exit(1); }
    Near(lock.Update(true, -15.f * kDeg, true, true), -15.f * kDeg, "the next lock latches the head anew");
}

void FacingForwardIsUnchanged() {
    // A player facing the play area's forward (most seated play) gets what 0.9.1 gave: the target straight ahead.
    LockOnFacing lock;
    const float enemy = -100.f * kDeg;
    Near(LockOnPlaySpaceYaw(enemy, lock.Update(true, 0.f, true, true)), enemy, "facing forward: the play space faces the enemy");
}

void NoHeadOrNoHeadInTheView() {
    LockOnFacing a;
    Near(a.Update(true, 60.f * kDeg, false, true), 0.f, "no head tracking: the play area's forward");
    LockOnFacing b;
    Near(b.Update(true, 60.f * kDeg, true, false), 0.f, "the view without the head's yaw: the play area's forward");
}

void WrapsAroundBehind() {
    LockOnFacing lock;
    const float facing = lock.Update(true, 190.f * kDeg, true, true);
    Near(facing, -170.f * kDeg, "the latched facing is wrapped");
    const float playSpace = LockOnPlaySpaceYaw(175.f * kDeg, facing);
    Near(ViewYaw(playSpace, facing), 175.f * kDeg, "an enemy behind the play area's forward is still ahead of you");
}
}

int main() {
    PlayAreaDrawnTowardASideWall();
    HeldForTheWholeLock();
    ReleaseAndRelock();
    FacingForwardIsUnchanged();
    NoHeadOrNoHeadInTheView();
    WrapsAroundBehind();
    std::puts("lock_on_facing: ok");
    return 0;
}
