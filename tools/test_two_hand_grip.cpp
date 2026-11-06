#include "two_hand_grip.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using namespace ervr;

void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}
// Compare transformed axes, including for q and -q (the same orientation).
bool SameRotation(Quat a, Quat b) {
    for (Vec3 axis : {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}})
        if ((a.Rotate(axis) - b.Rotate(axis)).Length() > 0.0001f) return false;
    return true;
}
Quat Game(Quat q) { return {-q.x, -q.y, q.z, q.w}; }

int main() {
    const Quat hands[2] = {Quat::FromEulerYXZ(-0.7f, 0.35f, 1.1f),
                           Quat::FromEulerYXZ(0.45f, -0.6f, -0.3f)};
    for (int weaponHand : {0, 1}) {
        const Quat weapon = hands[weaponHand], support = hands[1 - weaponHand];
        for (Quat calibration : {Quat{}, Quat::FromEulerYXZ(0.2f, -0.85f, 0.4f)}) {
            TwoHandGripOrientation grip;
            grip.Capture(weapon, support);
            Check(SameRotation(grip.InGame(Game(weapon), calibration), Game(support) * calibration),
                  "taking hold must preserve the supporting wrist's own angle for either weapon hand");
            Check(!SameRotation(grip.InGame(Game(weapon), calibration), Game(weapon) * calibration),
                  "the supporting hand must not copy the main hand's orientation");

            const Quat bodyTurn = Quat::YawQ(1.2f);
            for (Quat turn : {Quat{}, Quat::FromEulerYXZ(-0.8f, 0.5f, 0.9f),
                              Quat::AxisAngle({0.3f, -0.4f, 0.7f}, Deg2Rad(179.f))}) {
                // The weapon may turn before the game acknowledges the grab,
                // or lag/steer afterwards. Follow that rendered turn using the
                // original grip, including across repeated stereo/pose calls.
                const Quat renderedWeapon = bodyTurn * Game(turn * weapon);
                const Quat expectedSupport = bodyTurn * Game(turn * support) * calibration;
                for (int pass = 0; pass < 4; ++pass)
                    Check(SameRotation(grip.InGame(renderedWeapon, calibration), expectedSupport),
                          "the captured grip must follow weapon motion without recapturing or accumulating rotation");

                // Each arm still needs its own bone axes mapped to this target.
                const Vec3 boneForward{0, 1, 0};
                const Vec3 boneThumb{weaponHand == 0 ? 1.f : -1.f, 0, 0};
                const Quat target = grip.InGame(renderedWeapon, calibration);
                const Quat bone = Quat::MapPairs(boneForward, boneThumb,
                                                 target.Rotate({0, 0, 1}), target.Rotate({0, 1, 0}));
                Check((bone.Rotate(boneForward) - expectedSupport.Rotate({0, 0, 1})).Length() < 0.0001f &&
                      (bone.Rotate(boneThumb) - expectedSupport.Rotate({0, 1, 0})).Length() < 0.0001f,
                      "either hand's bone axes must follow the captured grip and hand calibration");
            }

            // A fresh grab replaces the previous wrist angle completely.
            const Quat nextWeapon = Quat::FromEulerYXZ(-0.2f, 0.9f, -0.4f);
            const Quat nextSupport = Quat::FromEulerYXZ(1.0f, -0.4f, 0.6f);
            grip.Capture(nextWeapon, nextSupport);
            Check(SameRotation(grip.InGame(Game(nextWeapon), calibration), Game(nextSupport) * calibration),
                  "re-grabbing must capture the new supporting wrist angle");

            grip.Capture(weapon, {-support.x, -support.y, -support.z, -support.w});
            Check(SameRotation(grip.InGame(Game(weapon), calibration), Game(support) * calibration),
                  "quaternion sign changes must not flip the captured grip");
        }
    }
    // The pinned hand's palm on the shaft, whatever angle the supporting wrist holds.
    {
        const Vec3 wrist{0.3f, 1.2f, 0.4f};
        const Quat weaponRot = Quat::FromEulerYXZ(0.4f, -0.3f, 0.2f);
        const Vec3 axis = weaponRot.Rotate({0, 1, 0});                     // up the blade
        const Vec3 side = weaponRot.Rotate({0, 0, 1}).Cross(axis).Normalized();
        const Vec3 rightGrip{0.06f, -0.01f, 0.015f}, leftGrip{0.06f, -0.01f, -0.015f};   // mirrored palms
        const Vec3 shaft = wrist + weaponRot.Rotate(rightGrip);
        const float along = 0.12f;
        // How far a palm sits from the shaft line, and how far down it.
        auto offShaft = [&](const Vec3& palm) { const Vec3 d = palm - shaft; return (d - axis * d.Dot(axis)).Length(); };
        auto downShaft = [&](const Vec3& palm) { return -(palm - shaft).Dot(axis); };
        for (Quat supportRot : {weaponRot, weaponRot * Quat::AxisAngle({0, 1, 0}, 3.14159265f),
                                weaponRot * Quat::FromEulerYXZ(0.9f, 0.4f, -1.1f)}) {
            const Vec3 pinned = TwoHandPinnedWrist(wrist, weaponRot, rightGrip, axis, along, side, 0.f, supportRot,
                                                   leftGrip);
            const Vec3 palm = pinned + supportRot.Rotate(leftGrip);
            Check(offShaft(palm) < 1e-4f, "the supporting palm must sit on the shaft for any wrist angle");
            Check(std::fabs(downShaft(palm) - along) < 1e-4f, "the supporting palm must sit `along` toward the pommel");
            // The 0.6.0-0.7.2 placement: the weapon hand's wrist slid down the hilt.
            const Vec3 oldPalm = wrist - axis * along + supportRot.Rotate(leftGrip);
            if (!SameRotation(supportRot, weaponRot))
                Check(offShaft(oldPalm) > 0.05f, "the old wrist placement must be the one that leaves the shaft");
        }
        const Vec3 pinned = TwoHandPinnedWrist(wrist, weaponRot, rightGrip, axis, along, side, 0.02f, weaponRot,
                                               leftGrip);
        Check(std::fabs(offShaft(pinned + weaponRot.Rotate(leftGrip)) - 0.02f) < 1e-4f,
              "TwoHandPinSideOffset moves the palm that far off the shaft");
    }
    std::puts("Two-hand grip orientation regression checks passed.");
}
