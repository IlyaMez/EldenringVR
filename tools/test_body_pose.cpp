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
    std::puts("Body/finger pose regression checks passed.");
}
