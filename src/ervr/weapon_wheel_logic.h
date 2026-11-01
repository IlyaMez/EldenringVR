#pragma once
// The weapon wheel's geometry and decisions, kept free of game and GPU state so the regression test
// (tools/test_weapon_wheel.cpp) can drive them. weapon_wheel.cpp is the only production user.
#include "vrmath.h"

namespace ervr {

constexpr int kWheelSlots = 3;   // ChrAsm keeps three weapon slots per hand

// A weapon slot holds something to switch to. ELDEN RING fills an empty slot with the Unarmed weapon
// (EquipParamWeapon 110000), not with 0.
inline bool WheelSlotFilled(int paramId) { return paramId > 0 && !(paramId >= 110000 && paramId < 110100); }

// Worth opening: a weapon to show. One alone still opens - a held grip that does nothing read as broken
// (2026-11-01: a right hand with only slot 3 filled) - and picking the weapon in hand just closes it.
inline bool WheelHasWeapons(const bool filled[kWheelSlots]) {
    for (int k = 0; k < kWheelSlots; ++k) if (filled[k]) return true;
    return false;
}

// Presses the game needs from slot `from` to slot `to`: each one moves the hand to its next filled slot
// in slot order, wrapping (empty slots are skipped). 0 = already there, -1 = `to` is empty.
inline int WheelCycleSteps(int from, int to, const bool filled[kWheelSlots]) {
    if (to < 0 || to >= kWheelSlots || !filled[to]) return -1;
    if (from == to) return 0;
    int s = from, n = 0;
    for (int i = 0; i < kWheelSlots; ++i) {
        s = (s + 1) % kWheelSlots;
        if (!filled[s]) continue;
        ++n;
        if (s == to) return n;
    }
    return -1;
}
// The filled slot one press before `to`: where a jump puts the active slot, so that a single press of
// the game's own switch lands on `to` - one switch animation instead of one per slot in between.
inline int WheelSlotBefore(int to, const bool filled[kWheelSlots]) {
    for (int i = 1; i < kWheelSlots; ++i) {
        const int s = (to - i + kWheelSlots) % kWheelSlots;
        if (filled[s]) return s;
    }
    return -1;
}

// Slot k's direction on the wheel, radians clockwise from straight up: slot 1 at the top, slot 2
// down and outward (to the right of the right hand, the left of the left), slot 3 down and inward.
// Fixed per slot, never repacked around empty ones, so a slot is always the same hand movement.
inline float WheelSlotAngle(int slot, bool rightHand) {
    const float step = 2.0943951f;   // 120 degrees
    const float a = slot == 1 ? step : slot == 2 ? -step : 0.f;
    return rightHand ? a : -a;
}

// The slot the hand is over: the filled slot nearest in angle to the hand's offset from the wheel
// centre (x right, y up, metres in the wheel plane) once the hand has left the dead zone, -1 inside
// it. The current pick holds until another slot is nearer by `stickyRad`, and the dead zone only
// takes it back at three quarters of its radius, so a hand resting on a border does not flicker.
inline int WheelPick(float x, float y, const bool filled[kWheelSlots], bool rightHand, float deadZone,
                     int current, float stickyRad = 0.17f) {
    const float dz = current >= 0 ? deadZone * 0.75f : deadZone;
    if (x * x + y * y < dz * dz) return -1;
    const float a = std::atan2(x, y);
    int best = -1;
    float bestD = 1e9f, curD = 1e9f;
    for (int k = 0; k < kWheelSlots; ++k) {
        if (!filled[k]) continue;
        float d = std::fabs(a - WheelSlotAngle(k, rightHand));
        if (d > 3.14159265f) d = 6.2831853f - d;
        if (d < bestD) { bestD = d; best = k; }
        if (k == current) curD = d;
    }
    if (current >= 0 && current < kWheelSlots && filled[current] && best != current && curD - bestD < stickyRad)
        return current;
    return best;
}

// The wheel's plane: centred where the hand opened it, facing the eyes, upright. `headUp` steadies it
// when the hand is straight below or above the head (world up then lies along the line of sight).
struct WheelFrame { Vec3 center, right, up, normal; };   // normal points at the viewer
inline WheelFrame WheelFrameFacing(const Vec3& center, const Vec3& eye, const Vec3& headUp) {
    WheelFrame f;
    f.center = center;
    Vec3 n = (eye - center).Normalized();
    if (n.Length() < 0.5f) n = {0.f, 0.f, 1.f};
    Vec3 up = Vec3{0.f, 1.f, 0.f};
    if (std::fabs(n.Dot(up)) > 0.94f) up = headUp;
    Vec3 u = (up - n * up.Dot(n)).Normalized();
    if (u.Length() < 0.5f) u = (Vec3{0.f, 0.f, -1.f} - n * -n.z).Normalized();
    f.normal = n; f.up = u; f.right = u.Cross(n);
    return f;
}
inline void WheelProject(const WheelFrame& f, const Vec3& p, float& x, float& y) {
    const Vec3 d = p - f.center;
    x = d.Dot(f.right); y = d.Dot(f.up);
}
inline Quat WheelOrientation(const WheelFrame& f) { return Quat::FromBasis(f.right, f.up, f.normal); }

// Pending -> open: the grip held AND the hand still for `holdSec`. Moving faster than `stillSpeed`
// restarts the count, so a swing with the grip squeezed, or reaching for the other hand's hilt, never
// opens the wheel - only a deliberate still hold does. (Combat's own strike state is NOT a gate: its
// continuous contact counts a strike whenever the blade tip drifts past ContactOnSpeed - 0.8 m/s at
// the tip - so a hand "held still" had one live most of the time and the wheel never opened, 2026-11-01.)
inline bool WheelHoldReady(double& stillSince, double now, float handSpeed, float stillSpeed, float holdSec) {
    if (handSpeed > stillSpeed) { stillSince = now; return false; }
    return now - stillSince >= holdSec;
}

// Why the wheel may not run for a hand right now (nullptr = it may). Checked when the grip closes -
// a blocked grip passes straight through to its [Buttons] mapping - and every tick after, where a
// block cancels a pending or open wheel without a selection. Being NEAR the other weapon's hilt is not
// in here: a grip closed there is a two-hand grab and passes through, but a hand that only passes the
// hilt later just pauses the hold (and never closes an open wheel - the inward slot is that way).
struct WheelGate {
    bool enabled = true, handAllowed = true, playerActive = true, menu = false, transition = false;
    bool twoHanded = false;   // the game's arm style is two-handed: the grips belong to the weapon
    bool grabbing = false;    // this hand has taken the other weapon's hilt (two-hand grab)
    bool weapons = true;      // the hand has a weapon in some slot
};
inline const char* WheelBlocked(const WheelGate& g) {
    if (!g.enabled || !g.handAllowed) return "off for this hand";
    if (!g.playerActive || g.transition) return "not in the world";
    if (g.menu) return "a menu is open";
    if (g.twoHanded) return "two-handing";
    if (g.grabbing) return "it took the other weapon's hilt (two-hand grab)";
    if (!g.weapons) return "no weapon in this hand's slots";
    return nullptr;
}
// Blocks the wheel is expected to have (no line in the log when a long squeeze ends on one of these).
inline bool WheelBlockExpected(const char* why, const WheelGate& g) {
    return why && (!g.enabled || !g.handAllowed || !g.playerActive || g.transition || g.menu || g.twoHanded || g.grabbing);
}

// Switching the game's way. One D-pad press moves a hand to its next filled slot (the game skips
// empty ones), so the commit presses, waits for ChrAsm's active slot to answer, and presses again
// until it reads the slot that was picked. The game is the authority - nothing writes the slot.
struct WheelCommitTiming {
    double press = 0.09;      // D-pad held per press
    double gap = 0.12;        // released at least this long before the next press
    double timeout = 1.0;     // a press the active slot has not answered by then counts as unanswered
    int maxPresses = 5;       // two are ever needed; the rest is slack for a late answer overshooting
    int maxUnanswered = 2;    // the game ignoring two presses in a row ends the attempt
};
struct WheelCommit {
    int target = -1;          // slot wanted, -1 = idle
    int phase = 0;            // 0 ready, 1 D-pad down, 2 released and waiting for the game
    double t = 0;             // phase start
    int activeAtPress = -1;
    int presses = 0, unanswered = 0;
    void Start(int slot) { *this = WheelCommit{}; target = slot; }
};
enum class WheelStep { Idle, Press, Wait, Done, Failed };
inline WheelStep WheelCommitStep(WheelCommit& c, int active, double now, const WheelCommitTiming& t) {
    if (c.target < 0) return WheelStep::Idle;
    if (c.phase == 1) {
        if (now - c.t < t.press) return WheelStep::Press;
        c.phase = 2; c.t = now;
    }
    if (c.phase == 2) {
        const bool moved = active != c.activeAtPress;
        if (now - c.t < t.gap || (!moved && now - c.t < t.timeout)) return WheelStep::Wait;
        if (moved) c.unanswered = 0; else ++c.unanswered;
        c.phase = 0;
    }
    if (active == c.target) { c.target = -1; return WheelStep::Done; }
    if (c.presses >= t.maxPresses || c.unanswered >= t.maxUnanswered) { c.target = -1; return WheelStep::Failed; }
    c.phase = 1; c.t = now; c.activeAtPress = active; ++c.presses;
    return WheelStep::Press;
}

} // namespace ervr
