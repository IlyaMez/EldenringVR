#pragma once
// A melee weapon's skill projectile in VR (Moonveil's wave, Reduvia's blood blades): where it leaves, which way it
// flies and how it is rolled. Pure math, no game memory - tested in tools/test_combat_moves.cpp.
//
// The game spawns a projectile from a 4x4 frame (CSBulletManager::SpawnBullet's spawn data, rows right / up /
// forward / position): the dummy poly of its own animation - the animated sword, or a point in front of the chest.
// ERVR draws your blade instead, so that frame is somewhere you are not swinging: the wave came out of your body and
// flew along your view (2026-11-11 test 4). The rewrite moves it to your blade, points it along the aim ERVR already
// computes (SkillAim) and, for a slash, turns the frame about its new forward so the plane the game's OWN animation
// slashed in lies in the plane YOUR swing swept. That works without knowing how the wave is built: a horizontal
// wave (Moonveil's light follow-up) comes from a horizontal animated slash and a vertical one from a vertical slash,
// so mapping the animation's slash plane onto yours turns either into your swing's - the light/heavy choice (your
// swing's speed) no longer decides whether the wave is upright or flat.
#include "vrmath.h"

namespace ervr {

// A blade direction (unit, world frame) at time t.
struct AxisSample { double t = 0.0; Vec3 axis{}; };

// The plane a blade swept from t0 to t1, as the sum of each step's rotation axis (cross of consecutive blade
// directions, |cross| = sin of the step's angle): the fast part of the swing, which turns furthest per step, decides
// it, and a wind-up turning the other way subtracts. Steps across a gap longer than maxGap (lost tracking, a pause
// in the samples) are skipped. Samples are in time order. *swept (optional) = the summed step angles, radians.
inline Vec3 SweptPlaneNormal(const AxisSample* s, int n, double t0, double t1, double maxGap = 0.05, float* swept = nullptr) {
    Vec3 sum{}; float ang = 0.f;
    for (int i = 1; i < n; ++i) {
        if (s[i - 1].t < t0 || s[i].t > t1) continue;
        if (s[i].t - s[i - 1].t > maxGap || s[i].t <= s[i - 1].t) continue;
        const Vec3 c = s[i - 1].axis.Cross(s[i].axis);
        sum = sum + c; ang += c.Length();
    }
    if (swept) *swept = ang;
    return sum;
}

struct ProjectileFrame { Vec3 right{}, up{}, fwd{}, pos{}; };
struct ProjectileTurn { float aimDeg = 0.f, rollDeg = 0.f; bool rolled = false; };

// A slash plane counts when it turned at least this far (radians) and lies along the flight (its normal at least
// this much across the forward): a thrust or a stab sweeps no plane the projectile could take.
constexpr float kMinSweptRad = 0.35f;   // ~20 degrees
constexpr float kMinAcross = 0.5f;

// The game's spawn frame `g` turned to fly along `aim` (unit; zero length = keep the game's direction) by the
// smallest rotation, then - when both are known - about the new forward so the game's slash plane (gamePlane, the
// normal SweptPlaneNormal gave for the animation) lies in your swing's (swingPlane). A plane has no side, so the
// roll is the smaller of the two, within +-90 degrees. Every row turns together: a fan of bullets stays a fan, and
// the position row is the caller's. Returns the turned frame; *turn says how far it turned.
inline ProjectileFrame TurnProjectile(const ProjectileFrame& g, const Vec3& aim, const Vec3& gamePlane, const Vec3& swingPlane, ProjectileTurn* turn = nullptr) {
    ProjectileTurn t;
    const Vec3 f0 = g.fwd.Normalized();
    const Vec3 f1 = aim.Length() > 0.5f ? aim.Normalized() : f0;
    Quat r = f0.Length() > 0.5f ? Quat::FromTo(f0, f1) : Quat{};
    t.aimDeg = Rad2Deg(std::acos(Clamp(f0.Dot(f1), -1.f, 1.f)));
    const float gl = gamePlane.Length(), sl = swingPlane.Length();
    const Vec3 gp = gamePlane - f0 * gamePlane.Dot(f0);     // across the game's flight
    const Vec3 sp = swingPlane - f1 * swingPlane.Dot(f1);   // across the new flight
    if (f0.Length() > 0.5f && gl >= kMinSweptRad && sl >= kMinSweptRad && gp.Length() >= kMinAcross * gl && sp.Length() >= kMinAcross * sl) {
        const Vec3 g1 = r.Rotate(gp.Normalized());
        Vec3 s1 = sp.Normalized();
        if (g1.Dot(s1) < 0.f) s1 = -s1;
        const float roll = std::atan2(g1.Cross(s1).Dot(f1), g1.Dot(s1));
        r = (Quat::AxisAngle(f1, roll) * r).Normalized();
        t.rollDeg = Rad2Deg(roll); t.rolled = true;
    }
    ProjectileFrame o;
    o.right = r.Rotate(g.right); o.up = r.Rotate(g.up); o.fwd = r.Rotate(g.fwd); o.pos = g.pos;
    if (turn) *turn = t;
    return o;
}

}
