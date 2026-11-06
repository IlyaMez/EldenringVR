#pragma once
// The action-button system's per-frame update entry (2.7.2.0 +0xa67a20, called once per frame by the world step at
// +0x58f704), read as bytes. It does two separate jobs:
//   1. resets CSActionButtonRegionSystem's table (its rcx): 15 slots of {distance, this cycle's nearest, the last
//      cycle's nearest}, one per kind of prompt (ActionButtonParam byte 1): last = this, this = none, distance =
//      FLT_MAX. A prompt that asks the table (+0xa67800, +0xa67620) registers itself if it is the nearest so far
//      and is answered "yours" only if it was the nearest LAST cycle - one cycle of hysteresis.
//   2. loads CSActionButtonManImp from its global and tail-jumps to the manager's own update (+0xa64d10), which
//      decides the interact press. It never reads the region table.
// ERVR runs the manager's update alone in the second eye's pass (pass2_interact.cpp), so the table resets once
// per frame like the event script that asks it. This finds the two addresses that takes: the manager's global
// and its update. Pure, so tools/test_action_button_update.cpp checks it on the 2.7.2.0 bytes.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ervr {
struct AbmUpdateTail {
    uintptr_t managerSlot = 0;     // the global holding CSActionButtonManImp*
    uintptr_t managerUpdate = 0;   // void update(CSActionButtonManImp*, frame-time arg)
};

// fn: the update's first n bytes, mapped at address `at`. The table loop ends `jne loop` and the manager load
// follows it (mov rcx, [rip+d]); the tail restores rdx and jumps (mov rdx, rbx; add rsp, 20h; pop rbx; jmp rel32).
inline bool DecodeAbmUpdateTail(const uint8_t* fn, size_t n, uintptr_t at, AbmUpdateTail* out) {
    static const uint8_t kLoopEnd[] = {0x75, 0xE2, 0x48, 0x8B, 0x0D};
    static const uint8_t kTail[] = {0x48, 0x8B, 0xD3, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xE9};
    size_t i = 0;
    while (i + sizeof(kLoopEnd) + 4 <= n && memcmp(fn + i, kLoopEnd, sizeof(kLoopEnd)) != 0) ++i;
    if (i + sizeof(kLoopEnd) + 4 > n) return false;
    int32_t d;
    memcpy(&d, fn + i + sizeof(kLoopEnd), 4);
    AbmUpdateTail t;
    t.managerSlot = at + i + 2 + 7 + (intptr_t)d;   // the 7-byte mov starts after the 2-byte jne
    size_t j = i + sizeof(kLoopEnd) + 4;
    while (j + sizeof(kTail) + 4 <= n && memcmp(fn + j, kTail, sizeof(kTail)) != 0) ++j;
    if (j + sizeof(kTail) + 4 > n) return false;
    memcpy(&d, fn + j + sizeof(kTail), 4);
    t.managerUpdate = at + j + sizeof(kTail) + 4 + (intptr_t)d;
    *out = t;
    return true;
}

// The region table's slots (one per kind of prompt): float distance at +0, this cycle's nearest at +8, last cycle's
// at +0x10. The test (+0xa67620 / +0xa67800) hands the slot's distance to its range check, which passes a prompt
// only if it is STRICTLY nearer (+0xa680f7: comiss best, d; jbe fail); a passing prompt is registered, and every
// prompt is answered "yours" only if it is last cycle's nearest.
//
// With the reset once per frame (pass 1), a prompt asked in both eyes' passes registers in the first and fails the
// range check in the second at the same distance: it is answered in one pass only, and the manager's update in the
// other pass forgets it every frame - graces and gatherable plants offered no prompt, ground items and dropped runes
// flickered, a door showed its prompt and ignored the button (0.7.3, 2026-11-06). So at each pass boundary ERVR
// banks every slot's nearest so far and reopens its distance (BankRegionSlots), and right before the real reset puts
// the nearest of the whole frame back (CarryRegionBank): each pass's prompts register as in a single-pass game, and
// the hysteresis still spans one frame, which the event scripts' prompts (pass 1 only) need.
constexpr size_t kRegionFirstSlot = 0x18, kRegionSlotStride = 0x48, kRegionSlots = 15;
constexpr size_t kRegionTableBytes = kRegionFirstSlot + kRegionSlots * kRegionSlotStride;
constexpr float kRegionFar = 3.40282347e+38f;   // FLT_MAX, what the reset writes

struct RegionBest {
    float dist = kRegionFar;
    uint64_t cur = 0;
};

// Between passes. Returns how many slots held a prompt registered since the previous call.
inline int BankRegionSlots(uint8_t* table, RegionBest* bank) {
    int n = 0;
    for (size_t i = 0; i < kRegionSlots; ++i) {
        uint8_t* s = table + kRegionFirstSlot + i * kRegionSlotStride;
        float dist;
        uint64_t cur;
        memcpy(&dist, s, 4);
        memcpy(&cur, s + 8, 8);
        if (!cur || !(dist < kRegionFar)) continue;   // nothing registered since the distance was reopened
        ++n;
        if (dist < bank[i].dist) bank[i] = RegionBest{dist, cur};
        memcpy(s, &kRegionFar, 4);                     // reopened; the nearest stays registered until a nearer one
    }
    return n;
}

// Right before the real reset (pass 1's update entry): the frame's nearest, banked or registered since, becomes the
// slot's nearest, which the reset then makes last cycle's. Empties the bank. Returns how many slots it changed.
inline int CarryRegionBank(uint8_t* table, RegionBest* bank) {
    int n = 0;
    for (size_t i = 0; i < kRegionSlots; ++i) {
        uint8_t* s = table + kRegionFirstSlot + i * kRegionSlotStride;
        float dist;
        uint64_t cur;
        memcpy(&dist, s, 4);
        memcpy(&cur, s + 8, 8);
        const RegionBest b = bank[i];
        bank[i] = RegionBest{};
        if (!b.cur || (cur && dist <= b.dist)) continue;   // what is registered is as near: the game's own pick
        memcpy(s, &b.dist, 4);
        memcpy(s + 8, &b.cur, 8);
        ++n;
    }
    return n;
}
} // namespace ervr
