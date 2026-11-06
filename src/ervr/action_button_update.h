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
} // namespace ervr
