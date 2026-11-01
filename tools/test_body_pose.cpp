#include "body_pose.h"
#include "head_anchor.h"
#include <array>
#include <cstdio>
#include <cstdlib>

using namespace ervr;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}
int main() {
    constexpr std::array names{"R_Hand", "R_Finger0", "R_Finger02", "L_Finger1", "L_Finger12", "R_Weapon", "Spine", "L_Thigh"};
    std::array<uint8_t, names.size()> fingers{};
    for (size_t i = 0; i < names.size(); ++i) fingers[i] = IsFingerBone(names[i]);
    Check(!fingers[0] && fingers[1] && fingers[2] && fingers[3] && fingers[4] && !fingers[5],
          "finger selection must cover both hands and joint depths without taking over the wrist or weapon");
    std::array<int, names.size()> stance{};
    for (int animationGrip : {1, 2, 3, 2, 1}) {
        auto live = stance;
        live.fill(animationGrip);
        ApplyBodyPoseTemplate<int>(live, stance, fingers);
        for (size_t i = 0; i < live.size(); ++i)
            Check(live[i] == (fingers[i] ? animationGrip : 0),
                  "current equipment grip must survive a frozen open-hand stance while the body stays fixed");
        const auto previous = live;
        ApplyBodyPoseTemplate<int>(live, stance, fingers);
        Check(live == previous, "repeated pose hooks must preserve the current grip");
    }
    BodyPoseCaptureGuard capture;
    constexpr uintptr_t player = 1, respawn = 2;
    capture.Update(player, false, 1.0, true);
    Check(capture.CanCapture(player), "initial on-foot capture must not wait for movement-cancel flags or stationary time");
    const auto walking = stance;
    auto applyFrame = [&](bool menu, double now, bool simulationPass, int animationPose) {
        capture.Update(player, menu, now, simulationPass);
        std::array<int, names.size()> live;
        live.fill(animationPose);
        // Simulate an already settled candidate: even a long seated loop must
        // never be eligible to replace the saved walking stance.
        if (simulationPass && capture.CanCapture(player)) stance = live;
        ApplyBodyPoseTemplate<int>(live, stance, fingers);
        return live;
    };
    for (int i = 0; i < 120; ++i) {
        auto seated = applyFrame(true, 2.0 + i, true, 7);
        Check(seated[7] == walking[7] && stance == walking, "a long grace rest must retain the VR body and saved walking stance");
    }
    Check(applyFrame(false, 121.01, true, 8)[7] == walking[7] && stance == walking,
          "closing the menu must keep the walking pose even with a stale seated buffer");
    applyFrame(false, 125.0, false, 9);
    Check(!capture.CanCapture(player), "the second eye must not end the capture cooldown");
    for (double now : {121.1, 122.0, 123.9}) {
        const auto standingUp = applyFrame(false, now, true, 8);
        Check(!capture.CanCapture(player) && standingUp[7] == walking[7] && standingUp[1] == 8,
              "stand-up cannot be captured or turn off VR posing; live fingers still follow equipment");
    }
    capture.Update(player, false, 124.01, true);
    Check(capture.CanCapture(player), "capture must recover after a bounded cooldown even without movement readiness");
    capture.Update(player, true, 125.0, false);
    Check(capture.CanCapture(player), "second-eye menu changes must not alter the simulation's capture decision");
    capture.Update(player, true, 125.0, true);
    capture.Update(player, false, 126.0, true);
    Check(!capture.CanCapture(player), "reopening a menu must restart the exit protection");
    capture.Update(respawn, false, 126.01, true);
    Check(capture.CanCapture(respawn) && !capture.CanCapture(player),
          "a new on-foot skeleton must initialize immediately without inheriting the old menu cooldown");
    capture.Update(0, false, 126.02, true);
    Check(!capture.CanCapture(0), "world teardown must never permit capture");

    // Reproduce the reported regression: before, during and after a menu, walk
    // and roll evaluations must still be overwritten and cannot move the head
    // anchor or replace its standing calibration. Only finger pose remains live.
    const std::array<float, 3> bodyStance{1.56f, 0.3f, 0.f}; // head height, leg pose, grip
    const std::array<uint8_t, 3> bodyFingers{0, 0, 1};
    HeadAnchorState anchor;
    for (int frame = 0; frame < 600; ++frame) {
        const double now = 200.0 + frame / 60.0;
        capture.Update(player, frame >= 120 && frame < 240, now, true);
        std::array<float, 3> live{frame % 2 ? 0.4f : 1.45f, 0.8f, 0.5f}; // roll/walk
        ApplyBodyPoseTemplate<float>(live, bodyStance, bodyFingers);
        Check(live == std::array<float, 3>{1.56f, 0.3f, 0.5f}, "walk/roll cannot pose the VR head or legs at any point in a menu cycle");
        const Vec3 root{frame * 0.01f, frame * 0.002f, 0.f};
        const Vec3 head = root + Vec3{0.f, live[0], 0.f};
        const Vec3 eye = anchor.Update(root, head, 1.60f, 1.f, 0.06f, true, now, frame);
        Check(std::fabs(eye.y - head.y - 0.04f) < 0.0001f && std::fabs(anchor.restHeight - 1.56f) < 0.0001f,
              "VR pose suppression must preserve the original eye-to-head offset through menus, walking and rolling");
        Check(!anchor.riding, "on-foot menus must never select the mounted head-anchor path");
    }

    // Stance judge (2026-11-01, "dropped to the ground / became really short"): the camera rides the template's
    // head, and a template could be any pose - a post-respawn retry, a knockdown held still. Rig as in c0000:
    // Master -> RootPos (hip height) -> Spine -> Head.
    struct Bone { float t[4]; float q[4]; float s[4]; };
    const std::array<int16_t, 4> parents{-1, 0, 1, 2};
    auto rig = [](float hip, Quat rootPosQ) {
        std::array<Bone, 4> p{};
        for (Bone& b : p) { b.q[3] = 1.f; b.s[0] = b.s[1] = b.s[2] = 1.f; }
        p[1].t[1] = hip; p[1].q[0] = rootPosQ.x; p[1].q[1] = rootPosQ.y; p[1].q[2] = rootPosQ.z; p[1].q[3] = rootPosQ.w;
        p[2].t[1] = 0.30f; p[3].t[1] = 0.25f;
        return p;
    };
    auto height = [&](const std::array<Bone, 4>& p) { return BoneHeight<Bone>(p, parents, 3); };
    const float reference = height(rig(1.05f, Quat{}));
    const auto standing = rig(1.0f, Quat{}), crouch = rig(0.60f, Quat{});
    const auto lying = rig(0.25f, Quat::AxisAngle({1.f, 0.f, 0.f}, 1.5708f));
    Check(std::fabs(reference - 1.60f) < 1e-4f && std::fabs(height(standing) - 1.55f) < 1e-4f,
          "head height walks the parent chain from the rig root");
    Check(std::fabs(height(lying) - 0.25f) < 0.01f, "a rotated hip folds the chain flat onto the ground");
    Check(JudgeStance(height(standing), reference) == BodyStance::Standing &&
          JudgeStance(height(crouch), reference) == BodyStance::Low &&
          JudgeStance(height(lying), reference) == BodyStance::Down,
          "standing, crouch and a body on the ground are told apart by head height");
    Check(JudgeStance(0.3f, 0.05f) == BodyStance::Standing && JudgeStance(0.3f, 9.f) == BodyStance::Standing,
          "an implausible reference pose judges nothing (old behaviour)");
    Check(JudgeTemplate(BodyStance::Down, false, false) == TemplateVerdict::Rejected,
          "a knockdown held still is never captured as the stance");
    Check(JudgeTemplate(BodyStance::Low, true, false) == TemplateVerdict::Rejected &&
          JudgeTemplate(BodyStance::Standing, true, false) == TemplateVerdict::Standing,
          "a post-load retry waits for a standing pose instead of snapshotting the respawn kneel");
    Check(JudgeTemplate(BodyStance::Low, false, false) == TemplateVerdict::Low,
          "an in-game crouch held still still reaches the VR body");
    Check(JudgeTemplate(BodyStance::Down, true, true) == TemplateVerdict::Low &&
          JudgeTemplate(BodyStance::Standing, true, true) == TemplateVerdict::Standing,
          "a misread reference cannot keep VR posing off once the wait is overdue");

    StandUpWatch watch;
    Check(!watch.Update(true, false, 10.0) && !watch.Update(true, true, 10.1) && !watch.Update(false, false, 10.2),
          "standing up needs time, and echo frames do not reset it");
    Check(!watch.Update(true, true, 10.30) && watch.Update(true, true, 10.36), "0.25 s of fresh standing frames ends a low stance");
    watch.Reset();
    Check(!watch.Update(true, true, 20.0) && !watch.Update(true, false, 20.2) && !watch.Update(true, true, 20.3) &&
          !watch.Update(true, true, 20.5) && watch.Update(true, true, 20.56),
          "a dip back down (getting-up animation) restarts the stand-up timer");

    // Eye height through the reported sequence. The calibration is preset from the standing template before a
    // low template is seen; calibrating from the low head instead put the eye 0.5 m too high after standing up.
    HeadAnchorState eyeAnchor;
    const Vec3 feet{2.f, 7.f, -1.f};
    eyeAnchor.restHeight = height(standing);
    const Vec3 low = eyeAnchor.Update(feet, feet + Vec3{0.f, height(crouch), 0.f}, 1.60f, 1.f, 0.06f, true, 30.0, 300);
    Check(std::fabs(low.y - feet.y - (1.60f - (height(standing) - height(crouch)))) < 1e-4f,
          "a low template takes the camera down by exactly the head's drop");
    const Vec3 up = eyeAnchor.Update(feet, feet + Vec3{0.f, height(standing), 0.f}, 1.60f, 1.f, 0.06f, true, 30.1, 301);
    Check(std::fabs(up.y - feet.y - 1.60f) < 1e-4f, "restoring the standing template puts the eye back at EyeHeight");
    std::puts("Body/finger pose regression checks passed.");
}
