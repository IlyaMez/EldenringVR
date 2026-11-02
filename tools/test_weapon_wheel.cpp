#include "weapon_wheel_logic.h"
#include <cstdio>
#include <cstdlib>

using namespace ervr;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}
int main() {
    Check(!WheelSlotFilled(110000) && !WheelSlotFilled(0) && !WheelSlotFilled(-1) && WheelSlotFilled(2000000),
          "an empty slot holds Unarmed (110000), a weapon slot anything else");
    {
        const bool one[3] = {false, false, true}, none[3] = {false, false, false};
        Check(WheelHasWeapons(one) && !WheelHasWeapons(none), "one weapon is enough to open the wheel; none is not");
    }
    // The game's cycle: next filled slot in slot order, wrapping; a jump sets the slot before the target.
    {
        const bool all[3] = {true, true, true}, gap[3] = {true, false, true};
        Check(WheelCycleSteps(0, 1, all) == 1 && WheelCycleSteps(0, 2, all) == 2 && WheelCycleSteps(2, 0, all) == 1 &&
              WheelCycleSteps(1, 0, all) == 2 && WheelCycleSteps(1, 1, all) == 0, "presses around a full hand");
        Check(WheelCycleSteps(0, 2, gap) == 1 && WheelCycleSteps(0, 1, gap) == -1, "the game skips an empty slot");
        Check(WheelSlotBefore(2, all) == 1 && WheelSlotBefore(0, all) == 2 && WheelSlotBefore(2, gap) == 0,
              "a jump puts the hand one filled slot before its target");
        // From any slot to any target: after the jump one press always lands on the target.
        for (int from = 0; from < 3; ++from)
            for (int to = 0; to < 3; ++to)
                if (from != to) Check(WheelCycleSteps(WheelSlotBefore(to, all), to, all) == 1, "one press after a jump");
    }
    // Slot directions: 1 up, 2 outward-down, 3 inward-down; mirrored for the left hand.
    const bool all[3] = {true, true, true};
    Check(WheelPick(0.f, 0.08f, all, true, 0.035f, -1) == 0, "straight up picks slot 1");
    Check(WheelPick(0.07f, -0.04f, all, true, 0.035f, -1) == 1, "the right hand's slot 2 is down and to the right");
    Check(WheelPick(-0.07f, -0.04f, all, true, 0.035f, -1) == 2, "the right hand's slot 3 is down and to the left");
    Check(WheelPick(-0.07f, -0.04f, all, false, 0.035f, -1) == 1, "the left hand's slot 2 is down and to the left");
    Check(WheelPick(0.01f, 0.01f, all, true, 0.035f, -1) == -1, "inside the dead zone nothing is picked");
    Check(WheelPick(0.f, 0.03f, all, true, 0.035f, 0) == 0, "a pick survives a little way back into the dead zone");
    {
        const bool gap[3] = {true, false, true};
        Check(WheelPick(0.07f, -0.04f, gap, true, 0.035f, -1) != 1, "an empty slot is never picked");
        // Exactly between slot 1 (0 deg) and slot 2 (120 deg) with slot 3 empty: the current pick holds.
        Check(WheelPick(0.069f, 0.04f, all, true, 0.035f, 0) == 0 && WheelPick(0.069f, 0.04f, all, true, 0.035f, 1) == 1,
              "a hand resting on a border keeps whatever it had");
    }
    // The plane faces the eye and stays upright.
    {
        const WheelFrame f = WheelFrameFacing({0.f, 1.2f, -0.4f}, {0.f, 1.6f, 0.f}, {0.f, 1.f, 0.f});
        Check(f.normal.Dot(Vec3{0.f, 0.4f, 0.4f}.Normalized()) > 0.999f, "the wheel faces the eyes");
        Check(f.right.x > 0.99f && f.up.y > 0.7f, "x is the viewer's right and y leans up");
        float x = 0.f, y = 0.f;
        WheelProject(f, f.center + f.right * 0.05f + f.normal * 0.2f, x, y);
        Check(std::fabs(x - 0.05f) < 1e-4f && std::fabs(y) < 1e-4f, "moving toward the eyes does not move the pick");
        const Quat q = WheelOrientation(f);
        Check((q.Rotate({0.f, 0.f, 1.f}) - f.normal).Length() < 1e-3f && (q.Rotate({1.f, 0.f, 0.f}) - f.right).Length() < 1e-3f,
              "the quad's +Z is the plane normal and +X its right");
        const WheelFrame below = WheelFrameFacing({0.f, 1.0f, 0.f}, {0.f, 1.6f, 0.f}, {0.f, 0.f, -1.f});
        Check(below.up.Length() > 0.99f && std::fabs(below.up.Dot(below.normal)) < 1e-3f,
              "a hand straight below the eyes still gets an upright frame");
    }
    // Hold timing: only a still hold opens it.
    {
        double since = 0.0;
        Check(!WheelHoldReady(since, 0.2, 0.1f, 0.8f, 0.35f) && WheelHoldReady(since, 0.36, 0.1f, 0.8f, 0.35f),
              "a still hold opens after HoldSeconds");
        since = 0.0;
        Check(!WheelHoldReady(since, 0.3, 2.5f, 0.8f, 0.35f) && !WheelHoldReady(since, 0.5, 0.1f, 0.8f, 0.35f) &&
              WheelHoldReady(since, 0.66, 0.1f, 0.8f, 0.35f), "a fast hand restarts the count");
    }
    // Gates.
    {
        WheelGate g;
        Check(WheelBlocked(g) == nullptr, "the open gate");
        g.twoHanded = true; Check(WheelBlocked(g) != nullptr && WheelBlockExpected(WheelBlocked(g), g), "two-handing blocks the wheel"); g.twoHanded = false;
        g.grabbing = true; Check(WheelBlocked(g) != nullptr, "a hand that took the other weapon's hilt is two-handing it"); g.grabbing = false;
        g.belt = true; Check(WheelBlocked(g) != nullptr && WheelBlockExpected(WheelBlocked(g), g), "a hand holding a belt item never opens the wheel"); g.belt = false;
        g.menu = true; Check(WheelBlocked(g) != nullptr, "menus block the wheel"); g.menu = false;
        g.weapons = false; Check(WheelBlocked(g) != nullptr && !WheelBlockExpected(WheelBlocked(g), g),
                                 "an empty hand blocks the wheel, and says so in the log");
    }
    // Commit: slot 0 -> slot 2 through slot 1, the game answering each press.
    {
        WheelCommitTiming t; WheelCommit c; c.Start(2);
        int active = 0; double now = 0.0; int presses = 0;
        bool done = false;
        for (int i = 0; i < 400 && !done; ++i, now += 0.01) {
            const WheelStep s = WheelCommitStep(c, active, now, t);
            if (s == WheelStep::Press && c.phase == 1 && now == c.t) { ++presses; }
            if (s == WheelStep::Wait && c.phase == 2 && now - c.t > 0.15 && active == c.activeAtPress) active = (active + 1) % 3;
            done = s == WheelStep::Done;
            Check(s != WheelStep::Failed, "a game that answers every press must reach the slot");
        }
        Check(done && active == 2 && presses == 2, "two presses walk slot 1 to slot 3");
    }
    // The game skipping an empty middle slot: one press lands on the target.
    {
        WheelCommitTiming t; WheelCommit c; c.Start(2);
        int active = 0; double now = 0.0; int presses = 0; bool done = false;
        for (int i = 0; i < 400 && !done; ++i, now += 0.01) {
            const WheelStep s = WheelCommitStep(c, active, now, t);
            if (s == WheelStep::Press && now == c.t) ++presses;
            if (s == WheelStep::Wait && now - c.t > 0.15 && active == c.activeAtPress) active = 2;
            done = s == WheelStep::Done;
        }
        Check(done && presses == 1, "one press when the game skips the empty slot");
    }
    // A game that never answers: two unanswered presses end it.
    {
        WheelCommitTiming t; WheelCommit c; c.Start(1);
        double now = 0.0; WheelStep s = WheelStep::Idle; int presses = 0;
        for (int i = 0; i < 1000; ++i, now += 0.01) {
            s = WheelCommitStep(c, 0, now, t);
            if (s == WheelStep::Press && now == c.t) ++presses;
            if (s == WheelStep::Failed) break;
        }
        Check(s == WheelStep::Failed && presses == 2 && c.target < 0, "two ignored presses fail the switch");
    }
    // Already there: nothing pressed.
    {
        WheelCommitTiming t; WheelCommit c; c.Start(1);
        Check(WheelCommitStep(c, 1, 0.0, t) == WheelStep::Done, "picking the weapon in hand presses nothing");
    }
    // The D-pad is released between presses for at least the gap.
    {
        WheelCommitTiming t; WheelCommit c; c.Start(2);
        int active = 0; double now = 0.0, lastUp = -1.0; bool wasDown = false;
        for (int i = 0; i < 400; ++i, now += 0.005) {
            const WheelStep s = WheelCommitStep(c, active, now, t);
            const bool down = s == WheelStep::Press;
            if (down && !wasDown && lastUp >= 0.0) Check(now - lastUp >= t.gap - 1e-9, "presses are separated by the gap");
            if (!down && wasDown) { lastUp = now; active = (active + 1) % 3; }   // the game answers on release
            wasDown = down;
            if (s == WheelStep::Done) break;
        }
    }
    std::puts("weapon wheel: ok");
    return 0;
}
