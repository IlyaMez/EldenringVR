// action_button_update.h: the update entry's bytes give the manager's global and its update (exe 2.7.2.0).
#include "action_button_update.h"
#include <cstdio>
#include <cstdlib>

using namespace ervr;
static void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}

// eldenring.exe 2.7.2.0, RVA 0xa67a20, 165 bytes: the region-table reset loop, the manager load, its null panic,
// the tail jump to 0xa64d10, then the next function's start.
static const uint8_t k272[] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0xC1, 0x18, 0x48, 0x8B, 0xDA, 0x4C, 0x8D, 0x81, 0x38, 0x04, 0x00,
    0x00, 0x49, 0x3B, 0xC8, 0x74, 0x25, 0x48, 0x83, 0xC1, 0x08, 0x33, 0xD2, 0x90, 0x48, 0x8B, 0x01, 0x48, 0x89, 0x41,
    0x08, 0xC7, 0x41, 0xF8, 0xFF, 0xFF, 0x7F, 0x7F, 0x48, 0x89, 0x11, 0x48, 0x8D, 0x49, 0x48, 0x48, 0x8D, 0x41, 0xF8,
    0x49, 0x3B, 0xC0, 0x75, 0xE2, 0x48, 0x8B, 0x0D, 0x33, 0xDA, 0x30, 0x03, 0x48, 0x85, 0xC9, 0x75, 0x2E, 0x48, 0x8D,
    0x0D, 0xB0, 0x25, 0x30, 0x03, 0xE8, 0xCA, 0xCE, 0x45, 0x01, 0x4C, 0x8B, 0xC8, 0x4C, 0x8D, 0x05, 0x38, 0x06, 0x83,
    0x02, 0xBA, 0xB4, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x0D, 0x14, 0x50, 0xF6, 0x01, 0xE8, 0x6F, 0x52, 0x45, 0x01, 0x48,
    0x8B, 0x0D, 0x00, 0xDA, 0x30, 0x03, 0x48, 0x8B, 0xD3, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xE9, 0x6B, 0xD2, 0xFF, 0xFF,
    0x90, 0x48, 0x8D, 0x0D, 0x39, 0xAD, 0x4B, 0x01, 0xE9, 0x5C, 0xF4, 0x40, 0x55, 0x53, 0x56, 0x41, 0x55, 0x41, 0x56,
    0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xF8, 0x48, 0x81, 0xEC, 0x08, 0x01, 0x00};

// The game's region table, as the reset (+0xa67a20's loop) and the test (+0xa67620) use it.
struct Table {
    alignas(8) uint8_t b[kRegionTableBytes] = {};
    float Dist(int k) const { float d; memcpy(&d, b + kRegionFirstSlot + k * kRegionSlotStride, 4); return d; }
    uint64_t Cur(int k) const { uint64_t v; memcpy(&v, b + kRegionFirstSlot + k * kRegionSlotStride + 8, 8); return v; }
    uint64_t Prev(int k) const { uint64_t v; memcpy(&v, b + kRegionFirstSlot + k * kRegionSlotStride + 0x10, 8); return v; }
    void Reset() {   // last = this, this = none, distance = FLT_MAX
        for (size_t k = 0; k < kRegionSlots; ++k) {
            uint8_t* s = b + kRegionFirstSlot + k * kRegionSlotStride;
            memcpy(s + 0x10, s + 8, 8);
            memset(s + 8, 0, 8);
            memcpy(s, &kRegionFar, 4);
        }
    }
    // The range check passes only a prompt strictly nearer than the slot's distance (otherwise: "not yours"); a
    // passing one registers, and is "yours" if it is last cycle's nearest.
    bool Test(int k, uint64_t element, float d) {
        uint8_t* s = b + kRegionFirstSlot + k * kRegionSlotStride;
        if (!(d < Dist(k))) return false;
        memcpy(s, &d, 4);
        memcpy(s + 8, &element, 8);
        return element == Prev(k);
    }
    Table() { Reset(); }
};

// One stereo frame as ERVR runs it. Prompts asked before the update entry (`early`) or after it (`late`) in each
// pass, both orders seen in players' logs. Returns how often each asker was answered "yours" per pass.
struct Ask { int kind; uint64_t element; float d; bool pass1, pass2, early; int yes[2]; };
static void Frame(Table& t, RegionBest* bank, Ask* asks, int n, bool banking) {
    for (int pass = 0; pass < 2; ++pass) {
        const auto askAll = [&](bool early) {
            for (int i = 0; i < n; ++i) {
                Ask& a = asks[i];
                if (a.early == early && (pass == 0 ? a.pass1 : a.pass2) && t.Test(a.kind, a.element, a.d)) ++a.yes[pass];
            }
        };
        askAll(true);
        if (pass == 0) {   // the update entry: the reset runs in pass 1 only
            if (banking) CarryRegionBank(t.b, bank);
            t.Reset();
        }
        askAll(false);
        if (banking) BankRegionSlots(t.b, bank);   // BeforePass2 after pass 1, AfterPass2 after pass 2
    }
}

static void RegionScenarios() {
    for (int early = 0; early < 2; ++early) {
        {   // 0.7.3 without banking: a grace asked in both passes is answered in one pass only.
            Table t; RegionBest bank[kRegionSlots];
            Ask a[] = {{3, 0x1000, 2.f, true, true, early != 0, {0, 0}}};
            for (int f = 0; f < 10; ++f) Frame(t, bank, a, 1, false);
            Check((a[0].yes[0] == 0) != (a[0].yes[1] == 0),
                  "without banking a both-pass prompt is answered in one pass only (the 0.7.3 bug)");
        }
        {   // Banking: answered in both passes, every frame after the first.
            Table t; RegionBest bank[kRegionSlots];
            Ask a[] = {{3, 0x1000, 2.f, true, true, early != 0, {0, 0}}};
            for (int f = 0; f < 10; ++f) Frame(t, bank, a, 1, true);
            Check(a[0].yes[0] >= 9 && a[0].yes[1] >= 9, "a grace asked in both passes is answered in both");
        }
        {   // A fog wall, asked by the event script in pass 1 only: answered every frame (one reset per frame).
            Table t; RegionBest bank[kRegionSlots];
            Ask a[] = {{5, 0x2000, 4.f, true, false, early != 0, {0, 0}}};
            for (int f = 0; f < 10; ++f) Frame(t, bank, a, 1, true);
            Check(a[0].yes[0] >= 9, "a pass-1-only fog wall is still answered every frame");
        }
        {   // The nearest of a kind wins across both passes: a farther prompt asked in pass 2 only never takes the
            // slot from the nearer pass-1 one, and is never answered.
            Table t; RegionBest bank[kRegionSlots];
            Ask a[] = {{5, 0x2000, 4.f, true, false, early != 0, {0, 0}}, {5, 0x3000, 6.f, false, true, early != 0, {0, 0}}};
            for (int f = 0; f < 10; ++f) Frame(t, bank, a, 2, true);
            Check(a[0].yes[0] >= 9 && a[1].yes[1] == 0, "the nearer prompt keeps its kind's slot");
        }
        {   // ...and the other way round: a nearer pass-2 prompt takes the slot from a farther pass-1 one.
            Table t; RegionBest bank[kRegionSlots];
            Ask a[] = {{5, 0x2000, 6.f, true, false, early != 0, {0, 0}}, {5, 0x3000, 4.f, false, true, early != 0, {0, 0}}};
            for (int f = 0; f < 10; ++f) Frame(t, bank, a, 2, true);
            // (the farther one can be answered in the very first frame, before the nearer one was ever asked)
            Check(a[0].yes[0] <= 1 && a[1].yes[1] >= 9, "a nearer prompt in the other pass wins");
        }
        {   // Walking from one item to the next: the slot follows the nearest within the game's one-cycle hysteresis
            // (plus a frame when prompts are asked before the update: the cycle then spans pass 2 and the next pass 1).
            Table t; RegionBest bank[kRegionSlots];
            Ask a[] = {{2, 0x4000, 1.f, true, true, early != 0, {0, 0}}, {2, 0x5000, 3.f, true, true, early != 0, {0, 0}}};
            for (int f = 0; f < 5; ++f) Frame(t, bank, a, 2, true);
            Check(a[0].yes[0] >= 4 && a[1].yes[0] == 0, "the nearer item is the one answered");
            a[0].d = 5.f;   // the player moved: the other item is nearer now
            a[0].yes[0] = a[0].yes[1] = a[1].yes[0] = a[1].yes[1] = 0;
            for (int f = 0; f < 5; ++f) Frame(t, bank, a, 2, true);
            Check(a[1].yes[0] >= 3 && a[1].yes[1] >= 3 && a[0].yes[0] <= 2, "the slot moves to the new nearest");
        }
    }
    {   // Bank bookkeeping: a carried slot is written back only when nearer than what is registered.
        Table t; RegionBest bank[kRegionSlots];
        Check(t.Test(1, 0x10, 2.f) == false, "first ask registers");
        Check(BankRegionSlots(t.b, bank) == 1 && t.Dist(1) == kRegionFar && t.Cur(1) == 0x10, "banked and reopened");
        t.Test(1, 0x20, 3.f);   // a farther one registers in the reopened slot
        Check(CarryRegionBank(t.b, bank) == 1 && t.Cur(1) == 0x10 && t.Dist(1) == 2.f, "the nearer banked one is put back");
        Check(CarryRegionBank(t.b, bank) == 0, "the bank is empty after a carry");
    }
}

int main() {
    {   // At its RVA: the manager global 0x3d75498 (CSActionButtonMan's slot) and its update 0xa64d10.
        AbmUpdateTail t;
        Check(DecodeAbmUpdateTail(k272, sizeof(k272), 0xa67a20, &t), "2.7.2.0 bytes decode");
        Check(t.managerSlot == 0x3d75498, "manager global is 0x3d75498");
        Check(t.managerUpdate == 0xa64d10, "manager update is 0xa64d10");
    }
    {   // Mapped at a real image base: both move with it.
        AbmUpdateTail t;
        const uintptr_t base = 0x7ff7d0000000ull;
        Check(DecodeAbmUpdateTail(k272, sizeof(k272), base + 0xa67a20, &t), "decodes at an image base");
        Check(t.managerSlot == base + 0x3d75498 && t.managerUpdate == base + 0xa64d10, "addresses follow the base");
    }
    {   // Cut before the tail jump's displacement: refused, not read past the end.
        AbmUpdateTail t;
        Check(!DecodeAbmUpdateTail(k272, 0x83, 0xa67a20, &t), "a short read is refused");
    }
    {   // A different function (the loop's jne target changed): refused.
        uint8_t b[sizeof(k272)];
        memcpy(b, k272, sizeof(b));
        b[0x3D] = 0xE0;   // jne -0x20 instead of -0x1E
        AbmUpdateTail t;
        Check(!DecodeAbmUpdateTail(b, sizeof(b), 0xa67a20, &t), "an unknown loop is refused");
    }
    {   // No tail jump after the manager load: refused.
        uint8_t b[sizeof(k272)];
        memcpy(b, k272, sizeof(b));
        b[0x80] = 0xE8;   // a call where the jmp was
        AbmUpdateTail t;
        Check(!DecodeAbmUpdateTail(b, sizeof(b), 0xa67a20, &t), "no tail jump is refused");
    }
    RegionScenarios();
    std::puts("action_button_update: ok");
    return 0;
}
