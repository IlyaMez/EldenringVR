#pragma once
// The belt's geometry and decisions, kept free of game and GPU state so the regression test
// (tools/test_belt.cpp) can drive them. belt.cpp is the only production user.
#include "vrmath.h"
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace ervr {

// The pouch slots the game gives a shortcut (Event Action held + a D-pad direction): top left, top right,
// middle left, middle right. Slots 5 and 6 have none - the game uses them from the pause menu only.
constexpr int kBeltSlots = 4;

// A pouch slot holds an item: a Goods id (category 4 in the top nibble). An empty slot reads 0xFFFFFFFF.
inline bool BeltItemValid(uint32_t itemId) { return itemId != 0xFFFFFFFFu && (itemId >> 28) == 4u && (itemId & 0x0FFFFFFFu) != 0; }
inline uint32_t BeltGoodsId(uint32_t itemId) { return itemId & 0x0FFFFFFFu; }

// ---------------------------------------------------------------- the game's pouch shortcut
// D-pad direction per slot. The game's own controls list (Event Action + D-pad, all three controller columns):
// top left = up, top right = right, middle left = left, middle right = down.
enum BeltDir : uint8_t { BeltUp = 0, BeltRight, BeltLeft, BeltDown };
inline const char* BeltDirName(uint8_t d) { return d == BeltUp ? "up" : d == BeltRight ? "right" : d == BeltLeft ? "left" : "down"; }

// Splits "a, b c" into at most `max` tokens on commas and spaces.
inline int BeltTokens(const std::string& s, std::string out[], int max) {
    int n = 0;
    std::string cur;
    for (size_t i = 0; i <= s.size(); ++i) {
        const char ch = i < s.size() ? s[i] : ',';
        if (ch == ',' || ch == ' ' || ch == '\t' || ch == ';') {
            if (!cur.empty()) { if (n < max) out[n] = cur; ++n; cur.clear(); }
        } else cur += (char)std::tolower((unsigned char)ch);
    }
    return n;
}
// "up,right,left,down" (slot 1..4). False, and `out` untouched, unless it names four different directions.
inline bool BeltParseDirections(const std::string& s, uint8_t out[kBeltSlots]) {
    std::string tok[kBeltSlots + 1];
    if (BeltTokens(s, tok, kBeltSlots + 1) != kBeltSlots) return false;
    uint8_t d[kBeltSlots];
    unsigned seen = 0;
    for (int k = 0; k < kBeltSlots; ++k) {
        if (tok[k] == "up") d[k] = BeltUp; else if (tok[k] == "right") d[k] = BeltRight;
        else if (tok[k] == "left") d[k] = BeltLeft; else if (tok[k] == "down") d[k] = BeltDown;
        else return false;
        if (seen & (1u << d[k])) return false;
        seen |= 1u << d[k];
    }
    for (int k = 0; k < kBeltSlots; ++k) out[k] = d[k];
    return true;
}
// "-30,30,-80,80": degrees around the waist per slot. False, and `out` untouched, unless four numbers.
inline bool BeltParseAngles(const std::string& s, float out[kBeltSlots]) {
    std::string tok[kBeltSlots + 1];
    if (BeltTokens(s, tok, kBeltSlots + 1) != kBeltSlots) return false;
    float a[kBeltSlots];
    for (int k = 0; k < kBeltSlots; ++k) {
        char* end = nullptr;
        a[k] = std::strtof(tok[k].c_str(), &end);
        if (!end || *end || std::fabs(a[k]) > 180.f) return false;
    }
    for (int k = 0; k < kBeltSlots; ++k) out[k] = a[k];
    return true;
}

// The pouch shortcut pressed as a person presses it: Event Action held first, then the direction with it, the
// direction let go before Event Action. A short lead is read by the game as a plain D-pad press - a spell, item or
// weapon switch - because it counts the hold in its own frames (the two-hand chord's lesson, 2026-08-25).
struct BeltChordTiming { double lead = 0.40, press = 0.20, tail = 0.15; };
struct BeltChord {
    int slot = -1;
    int phase = 0;      // 0 idle, 1 Event Action alone, 2 with the direction, 3 Event Action alone again
    double t = 0.0;     // phase start
    bool Active() const { return phase != 0; }
    void Start(int s, double now) { slot = s; phase = 1; t = now; }
};
enum : uint8_t { BeltChordMod = 1, BeltChordDir = 2 };
// What to hold this tick (BeltChordMod and/or BeltChordDir); 0 once it is over.
inline uint8_t BeltChordStep(BeltChord& c, double now, const BeltChordTiming& t) {
    switch (c.phase) {
    case 1:
        if (now - c.t < t.lead) return BeltChordMod;
        c.phase = 2; c.t = now;
        return BeltChordMod | BeltChordDir;
    case 2:
        if (now - c.t < t.press) return BeltChordMod | BeltChordDir;
        c.phase = 3; c.t = now;
        return BeltChordMod;
    case 3:
        if (now - c.t < t.tail) return BeltChordMod;
        break;
    }
    c.phase = 0;
    return 0;
}

// ---------------------------------------------------------------- where the belt hangs
// The eyes sit ahead of and above the neck's pivot, so looking down at the belt carries them forward and down -
// the neck point stays where it is, and the belt with it.
constexpr float kEyeAboveNeck = 0.10f, kEyeAheadOfNeck = 0.09f;
inline Vec3 BeltNeck(const Pose& head) { return head.pos - head.rot.Rotate({0.f, kEyeAboveNeck, -kEyeAheadOfNeck}); }
// Level forward and right for a yaw in the VR convention (Quat::Yaw: 0 = -Z, positive = turned left).
inline Vec3 BeltYawForward(float yaw) { return {-std::sin(yaw), 0.f, -std::cos(yaw)}; }
inline Vec3 BeltYawRight(float yaw) { return {std::cos(yaw), 0.f, -std::sin(yaw)}; }
// A slot `angle` radians around the waist from straight ahead (positive = to the right), `radius` out from the
// spine, `drop` below the eyes of an upright head.
inline Vec3 BeltSlotPos(const Vec3& neck, float yaw, float angle, float radius, float drop) {
    const Vec3 out = BeltYawForward(yaw) * std::cos(angle) + BeltYawRight(yaw) * std::sin(angle);
    return Vec3{neck.x, neck.y + kEyeAboveNeck - drop, neck.z} + out * radius;
}

// The belt turns with the body, not the head. It stays put while the head looks around within `deadZone` of it, is
// dragged along beyond that, and settles under the head once the head has kept one heading (within 12 degrees) for
// two seconds - long enough that a glance at something beside you leaves it, short enough that a body that turned
// gets its belt back. Frozen entirely while the player reaches for it or looks down at it, so an item never slides
// away from the hand going for it.
struct BeltYaw {
    bool valid = false;
    float yaw = 0.f;
    float steadyYaw = 0.f;
    double steadySince = 0.0;
    static constexpr float kSteadyBand = 0.21f;    // rad
    static constexpr double kSettleAfter = 2.0;    // s
    static constexpr float kSettleTau = 0.6f;      // s
    float Update(float headYaw, double now, double dt, float deadZone, bool freeze) {
        if (!valid) { valid = true; yaw = steadyYaw = headYaw; steadySince = now; return yaw; }
        if (std::fabs(WrapPi(headYaw - steadyYaw)) > kSteadyBand) { steadyYaw = headYaw; steadySince = now; }
        if (freeze) return yaw;
        const float d = WrapPi(headYaw - yaw);
        if (std::fabs(d) > deadZone) yaw = WrapPi(yaw + (d > 0.f ? d - deadZone : d + deadZone));
        else if (now - steadySince >= kSettleAfter) yaw = WrapPi(yaw + d * (1.f - std::exp(-(float)dt / kSettleTau)));
        return yaw;
    }
};

// The filled slot nearest the hand within `radius`, skipping slots the other hand holds; -1 if none.
inline int BeltNearest(const Vec3& hand, const Vec3 pos[kBeltSlots], const bool filled[kBeltSlots], const bool taken[kBeltSlots], float radius) {
    int best = -1;
    float bestD = radius;
    for (int k = 0; k < kBeltSlots; ++k) {
        if (!filled[k] || taken[k]) continue;
        const float d = (pos[k] - hand).Length();
        if (d <= bestD) { bestD = d; best = k; }
    }
    return best;
}

// ---------------------------------------------------------------- what the game made of a use
// The state a use is judged by, read on the game thread.
struct BeltGameState {
    uint32_t pouch[kBeltSlots] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    int quantity[kBeltSlots] = {-1, -1, -1, -1};   // -1 = not found in the inventory
    uint32_t queuedItem = 0xFFFFFFFFu;             // the item the player's next UseGoods event applies
    int quickSlot = -1, weaponL = -1, weaponR = -1, spellSlot = -1;   // the selections a plain D-pad press moves
};
enum class BeltOutcome { Pending, Used, OtherSlot, Switched, Unconfirmed };
struct BeltVerdict { BeltOutcome outcome = BeltOutcome::Pending; int otherSlot = -1; const char* switched = nullptr; };
// Slot k's item went: its count dropped, the slot emptied or changed (the last one, a flask turning empty), or the
// game queued it for its UseGoods event.
inline bool BeltSlotWent(const BeltGameState& before, const BeltGameState& now, int k) {
    const uint32_t item = before.pouch[k];
    if (!BeltItemValid(item)) return false;
    if (now.pouch[k] != item) return true;
    if (before.quantity[k] > 0 && now.quantity[k] >= 0 && now.quantity[k] < before.quantity[k]) return true;
    return now.queuedItem != before.queuedItem && now.queuedItem == item;
}
// `checkSwitch`: a plain D-pad switch shows within a few frames of the press, so only that long - the weapon wheel
// may switch on purpose later. `over`: the watch ends; whatever was not seen is Unconfirmed.
inline BeltVerdict BeltJudge(const BeltGameState& before, const BeltGameState& now, int slot, bool checkSwitch, bool over) {
    BeltVerdict v;
    if (slot < 0 || slot >= kBeltSlots) { v.outcome = BeltOutcome::Unconfirmed; return v; }
    if (BeltSlotWent(before, now, slot)) { v.outcome = BeltOutcome::Used; return v; }
    for (int k = 0; k < kBeltSlots; ++k)
        if (k != slot && BeltSlotWent(before, now, k)) { v.outcome = BeltOutcome::OtherSlot; v.otherSlot = k; return v; }
    if (checkSwitch) {
        const char* what = now.quickSlot != before.quickSlot ? "quick item"
                         : now.spellSlot != before.spellSlot ? "spell"
                         : now.weaponR != before.weaponR ? "right-hand weapon"
                         : now.weaponL != before.weaponL ? "left-hand weapon" : nullptr;
        if (what) { v.outcome = BeltOutcome::Switched; v.switched = what; return v; }
    }
    if (over) v.outcome = BeltOutcome::Unconfirmed;
    return v;
}
// The game used slot `other` for the direction sent for `sent`: those two directions trade places.
inline void BeltSwapDirections(uint8_t dir[kBeltSlots], int sent, int other) {
    if (sent < 0 || other < 0 || sent >= kBeltSlots || other >= kBeltSlots) return;
    const uint8_t t = dir[sent]; dir[sent] = dir[other]; dir[other] = t;
}

} // namespace ervr
