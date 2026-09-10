#include "weapon_motion.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using namespace ervr;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}
float Error(Quat a, Quat b) { return WeaponRotationVector(a * b.Conj()).Length(); }
Quat Step(Quat current, Quat target, float dt, float weight = 23.f, float com = 0.9f, float torque = 1.f) {
    return StepWeaponOrientation(current, target, {0, 1, 0}, dt, weight, 1.6f, com,
                                 2.f, 0.5f, 8.f, 300.f, torque, 3.f, 0.6f);
}
int main() {
    for (float dt : {1.f / 20.f, 1.f / 30.f, 1.f / 60.f, 1.f / 90.f, 1.f / 120.f}) {
        const Quat target = Quat::AxisAngle({1, 0, 0}, 0.5f);
        Check(Error(Step({}, target, dt, 0), target) < 0.0001f, "empty hands must track directly");
        Check(Error(Step({}, target, dt, 23, 1.1f), target) > Error(Step({}, target, dt, 23, 0.25f), target),
              "head-heavy weapons must resist bending more than balanced weapons of the same weight");
        Check(Error(Step({}, target, dt, 23, 0.9f, 2.f), target) < Error(Step({}, target, dt), target),
              "two-handed leverage must reduce angular lag");
        Quat q;
        float last = Error(q, target);
        for (int i = 0; i < 180; ++i) {
            q = Step(q, target, dt);
            const float error = Error(q, target);
            Check(error <= last + 0.00001f, "a stopped hand must settle monotonically without rubberbanding");
            Check(WeaponRotationVector(q).x <= 0.50001f, "the blade must never overshoot a stopped hand");
            last = error;
        }
        Check(last < 0.0001f, "heavy weapons must settle completely");
        for (int i = 0; i < 120; ++i) {
            Quat reverse = Quat::AxisAngle({1, 0, 0}, i % 20 < 10 ? -0.9f : 0.9f);
            q = Step(q, reverse, dt);
            Check(Error(q, reverse) <= 0.6001f, "direction reversals must obey the angular grip constraint");
            Check(std::fabs(q.Norm() - 1.f) < 0.0001f, "rapid reversals must stay finite and normalized");
        }
        for (float angle : {-2.9f, -0.8f, 0.8f, 2.9f}) {
            Quat twist = Quat::AxisAngle({0, 1, 0}, angle);
            Check(Error(Step({}, twist, dt), twist) < 0.0001f,
                  "a long head-heavy weapon must not resist rolling its edge around the shaft");
        }
        Check(Error(Step(q, target, 0.f), target) < 0.0001f, "tracking resets must discard old lag");
    }
    for (bool handle : {false, true}) {
        Quat calibration = Quat::FromEulerYXZ(0.2f, Deg2Rad(45.f), -0.1f);
        const Vec3 axis = WeaponShaftAxis(calibration, handle);
        Check(WeaponTipVelocity({}, axis * 5.f, {}, axis, 1.6f).Length() < 0.0001f,
              "spinning about the calibrated shaft must not invent a blade-tip swing");
        const Vec3 omega = axis.Cross({1, 0, 0}).Normalized() * 2.f;
        Check(std::fabs(WeaponTipVelocity({}, omega, {}, axis, 1.6f).Length() - 3.2f) < 0.0001f,
              "bending about the calibrated shaft must measure the full visible tip speed");
        const Quat wrist = Quat::FromEulerYXZ(0.9f, -0.4f, 0.7f);
        const Quat twoBends = wrist * Quat::AxisAngle(axis.Cross({1, 0, 0}), 0.5f) *
                                      Quat::AxisAngle(axis.Cross({0, 1, 1}), -0.4f);
        const Quat result = PreserveWeaponWristTwist(twoBends, wrist, axis);
        Check(Error(WeaponTwist(wrist.Conj() * result, axis), {}) < 0.0001f,
              "combined two-hand steering and weight must not add their own shaft twist");
    }
    std::puts("Weapon motion regression checks passed.");
}
