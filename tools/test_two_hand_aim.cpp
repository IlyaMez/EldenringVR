#include "two_hand_aim.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using namespace ervr;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}
float Error(Quat a, Quat b) { return TwoHandAimState::RotationVector(a * b.Conj()).Length(); }

int main() {
    for (float dt : {1.f / 30.f, 1.f / 60.f, 1.f / 90.f}) {
        for (Vec3 line : {Vec3{0.12f, 0.09f, 0.f}, Vec3{-0.12f, 0.09f, 0.f},
                          Vec3{0.1f, -0.08f, 0.f}, Vec3{0.12f, 0.f, 0.09f}}) {
            TwoHandAimState s;
            Quat wrist = Quat::FromEulerYXZ(0.8f, 0.4f, -0.3f);
            Vec3 worldLine = wrist.Rotate(line);
            for (int i = 0; i < 20; ++i)
                Check(Error(s.Update(wrist, worldLine, true, dt), wrist) < 0.0001f,
                      "taking hold with sideways or guard-side separation must preserve alignment");
            Quat turn = Quat::AxisAngle({0.3f, 0.7f, -0.2f}, 1.3f);
            Check(Error(s.Update(turn * wrist, turn.Rotate(worldLine), true, dt), turn * wrist) < 0.0001f,
                  "moving both controllers together must retain the calibrated wrist orientation");
            Quat steer = Quat::FromTo(worldLine.Normalized(), Quat::YawQ(0.4f).Rotate(worldLine).Normalized());
            Quat prev = turn * wrist;
            for (int i = 0; i < 80; ++i) {
                Quat q = s.Update(wrist, steer.Rotate(worldLine), true, dt);
                if (i > 0) Check(Error(q, prev) <= Deg2Rad(360.f) * dt + 0.001f, "steering correction must have bounded speed");
                prev = q;
            }
            Check(Error(prev, wrist) > 0.01f, "the supporting hand must actually steer the weapon");
            Check(Error(WeaponTwist(wrist.Conj() * prev, {0, 1, 0}), {}) < 0.0001f,
                  "support steering must not contribute shaft twist");
            Check(Error(s.Update(wrist, {}, true, dt), prev) < 0.0001f, "overlapping controllers must not snap the weapon");
            for (int i = 0; i < 80; ++i) {
                Quat q = s.Update(wrist, wrist.Rotate({0.12f, i % 2 ? 0.049f : 0.051f, 0.f}), true, dt);
                Check(Error(q, prev) <= Deg2Rad(360.f) * dt + 0.001f, "crossing the old 5 cm cutoff must stay continuous");
                prev = q;
            }
            for (int i = 0; i < 80; ++i) {
                Quat q = s.Update(wrist, {}, false, dt);
                Check(Error(q, prev) <= Deg2Rad(360.f) * dt + 0.001f, "release/tracking loss must blend back to wrist aim");
                prev = q;
            }
            Check(Error(prev, wrist) < 0.0001f, "one-handed aim must settle back onto the controller");
            Check(Error(s.Update(wrist, -worldLine, true, 0.f), wrist) < 0.0001f, "a tracking gap must discard stale grab calibration");
        }
        // The main wrist owns twist for either grip axis, including calibration
        // and the lateral separation needed to fit real controllers together.
        for (Vec3 axis : {Vec3{0, 1, 0}, Vec3{0, 0, 1},
                          WeaponShaftAxis(Quat::FromEulerYXZ(0.2f, 0.8f, -0.3f), true)}) {
            for (float side : {-1.f, 1.f}) {
                TwoHandAimState s;
                const Vec3 line = axis * 0.2f + axis.Cross({1, 0, 0}).Normalized() * (side * 0.12f);
                s.Update({}, line, true, dt, axis);
                for (int i = 0; i < 180; ++i) {
                    Quat wrist = Quat::AxisAngle(axis, i * 0.025f);
                    Quat result = s.Update(wrist, line, true, dt, axis);
                    Check(Error(WeaponTwist(result, axis), WeaponTwist(wrist, axis)) < 0.0002f,
                          "rolling the main wrist must retain its full twist with the support position fixed");
                }
            }
        }
    }
    std::puts("Two-hand aim regression checks passed.");
}
