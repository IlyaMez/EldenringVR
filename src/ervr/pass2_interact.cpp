#include "pass2_interact.h"
#include "action_button_update.h"
#include "config.h"
#include "frame_probe.h"
#include "game_er.h"
#include "game_memory.h"
#include "log.h"
#include "midhook.h"
#include "rtti.h"
#include <windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace ervr {
namespace {

// A watched game object: found by the lookup thread, vtable-checked before every read, so a manager a load
// tears down or rebuilds reads as absent, never as garbage.
struct Watched {
    const char* singleton;   // FD4 singleton name (FindSingletonSlot)
    const char* rtti;        // class of the live object
    const char* tag;         // log name
    size_t off, len;         // the bytes snapshotted
    std::atomic<uint8_t*> obj{nullptr};
    std::atomic<void**> vt{nullptr};
};
// CSActionButtonManImp: vftable, 0x88 bytes of state, its mutex at +0x90 (fromsoftware-rs action_button.rs).
// MapItemManImpl +0xA0: the item-get popup queue, ten 16-byte records with the count at +0x148 - the push at
// exe 2.7.2.0+0x5653f0 that TGA's ItemPopup calls, capacity-checked by both of its callers.
// CSMenuManImp +0x90: ui_states, one created/visible byte per UI element (fromsoftware-rs menu_man.rs; the
// pause-menu cursor is element 0x1C, game_memory.cpp).
Watched g_abm{"CSActionButtonMan", "CS::CSActionButtonManImp", "action-button state", 0x08, 0x88};
Watched g_items{"MapItemMan", "CS::MapItemManImpl", "item popup queue", 0xA0, 0xB0};
Watched g_menu{"CSMenuMan", "CS::CSMenuManImp", "ui", 0x90, 0x46};
Watched* const g_all[] = {&g_abm, &g_items, &g_menu};
constexpr int kObjs = 3;
constexpr size_t kMaxLen = 0xB0;
constexpr size_t kQueueCount = 0xA8;   // the queue's count, inside its snapshot

struct Snap { bool valid[kObjs]; uint8_t bytes[kObjs][kMaxLen]; };

std::atomic<long> g_frameNo{0};

// Frame-thread state.
Snap g_last{};
bool g_haveLast = false;
int  g_lastBoundary = 0;   // 0 frame begin, 1 before pass 2, 2 after pass 2
uint16_t g_flips[kObjs][kMaxLen];   // per byte: how often it changed (counters and timers go quiet)
long g_lines[3];                    // log lines spent per label
long g_changes[kObjs][3];           // per object and label, for the summary
ULONGLONG g_lastSummary = 0;

enum Label { kPass1, kPass2, kOutside };
const char* const kLabel[] = {"pass 1", "pass 2", "between frames"};
constexpr long kLineCap[] = {300, 300, 60};

bool g_watch = false;

// The action-button updates run so far (both passes count). The repeat guard below measures "the same press"
// in updates, which advance in every state that can answer a press - a frame counter freezes when the pump
// stops (flat frames), and "same frame" alone would then swallow every later press.
std::atomic<uint32_t> g_decision{0};
bool g_updateHooked = false;   // without the update hook there is no update count to key answers on

// Who asks: each prompt caller (return address), its calls per pass and how many came from off the frame thread.
// Measured 2026-11-04 (in-headset, 2.7.2.0): a grace's and an NPC's prompts were asked ONLY while pass 2 ran,
// once per stereo pair - the old pass-2 "not pressed" rule left them nothing to answer. Logged to show where
// each kind of prompt is asked from.
struct Caller {
    std::atomic<uintptr_t> ra{0};
    std::atomic<long> n[2]{}, offThread{0};
};
Caller g_callers[12];
std::atomic<long> g_callerLines{0};

void NoteCaller(uintptr_t ra, int p, int32_t id) {
    const unsigned long frameThread = FramePumpFrameThread();
    const unsigned long tid = GetCurrentThreadId();
    for (Caller& c : g_callers) {
        uintptr_t cur = c.ra.load(std::memory_order_acquire);
        if (!cur && c.ra.compare_exchange_strong(cur, ra, std::memory_order_acq_rel)) {
            if (g_callerLines.fetch_add(1, std::memory_order_relaxed) < 24) {
                auto* base = (uint8_t*)GetModuleHandleW(nullptr);
                LOGI("[p2watch] prompt asked from exe+0x%llX (first: action button %d, pass %d, %s thread %lu, task "
                     "tag %d, frame %ld)",
                     (unsigned long long)((uint8_t*)ra - base), id, p + 1,
                     tid == frameThread ? "the frame" : "another", tid, FramePumpCurrentTaskGroup(),
                     g_frameNo.load(std::memory_order_relaxed));
            }
            cur = ra;
        }
        if (cur != ra) continue;
        c.n[p].fetch_add(1, std::memory_order_relaxed);
        if (tid != frameThread) c.offThread.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

// The return addresses above the hook's own: each a direct call (E8) into the function that holds the address
// below it. Names who asked a prompt through the game's wrappers (exe+0xA67491 static, exe+0xA64B5C locked: the
// event script's region prompts come up the locked one from EcTestActionButtonRegion, exe+0x578256 on 2.7.2.0).
uintptr_t g_imageLo = 0, g_imageHi = 0;

int CallerChain(uintptr_t ra0, uintptr_t* out, int max) {
    if (!g_imageHi) return 0;
    auto* sp = (uintptr_t*)_AddressOfReturnAddress();
    const auto stackBase = (uintptr_t)((NT_TIB*)NtCurrentTeb())->StackBase;
    uintptr_t cur = ra0;
    int n = 0;
    for (uintptr_t* p = sp + 1; n < max && p < sp + 192 && (uintptr_t)(p + 1) <= stackBase; ++p) {
        const uintptr_t v = *p;
        if (v < g_imageLo + 5 || v >= g_imageHi) continue;
        uint8_t call[5];
        if (!SafeRead((void*)(v - 5), call, 5) || call[0] != 0xE8) continue;
        int32_t d;
        memcpy(&d, call + 1, 4);
        const uintptr_t target = v + (intptr_t)d;
        if (target > cur || cur - target > 0x600) continue;
        out[n++] = v;
        cur = v;
    }
    return n;
}

// Each (prompt caller, action button) pair's first call, with the callers above the wrapper: the log names every
// kind of prompt the session met, the pass it was asked in and who asked (a fog wall: 10000 from the locked
// wrapper, called from the event script's region check).
std::atomic<uint64_t> g_pairs[64];
std::atomic<long> g_pairLines{0};

void NotePair(uintptr_t ra, int p, int32_t id) {
    const uint64_t key = ((uint64_t)(uint32_t)(ra - g_imageLo) << 32) | (uint32_t)id;
    for (auto& s : g_pairs) {
        uint64_t cur = s.load(std::memory_order_acquire);
        if (cur == key) return;
        if (cur) continue;
        if (!s.compare_exchange_strong(cur, key, std::memory_order_acq_rel)) {
            if (cur == key) return;
            continue;
        }
        if (g_pairLines.fetch_add(1, std::memory_order_relaxed) >= 48) return;
        uintptr_t chain[3] = {};
        const int n = CallerChain(ra, chain, 3);
        char above[96] = "";
        for (int i = 0, at = 0; i < n && at < (int)sizeof(above); ++i)
            at += snprintf(above + at, sizeof(above) - at, " <- exe+0x%llX",
                           (unsigned long long)(chain[i] - g_imageLo));
        LOGI("[p2watch] action button %d asked in pass %d from exe+0x%llX%s (frame %ld)", id, p + 1,
             (unsigned long long)(ra - g_imageLo), above, g_frameNo.load(std::memory_order_relaxed));
        return;
    }
}

// One press, one answer per prompt. The single-pass game never has a prompt answer "pressed" twice for one press;
// with the stereo second pass it can (0.6.5: an item handled twice, its card left on screen with nothing to close
// it). So a prompt (caller + action button id) that already answered "pressed" within the last kRepeatUpdates
// updates is told "not pressed" - two updates is one stereo pair (both passes update) or two flat frames, far
// shorter than any second press a hand can make (the press lands on release). A dropped repeat renews the
// window, so a run of repeats from one press is dropped whole.
constexpr uint32_t kRepeatUpdates = 2;
struct Answer { uintptr_t caller; int32_t id; uint32_t decision; };
SRWLOCK g_answersLock = SRWLOCK_INIT;
Answer g_answers[8]{};
int g_answersNext = 0;
std::atomic<long> g_repeats{0}, g_repeatLines{0};

bool RepeatAnswer(uintptr_t caller, int32_t id) {
    const uint32_t decision = g_decision.load(std::memory_order_acquire);
    AcquireSRWLockExclusive(&g_answersLock);
    bool repeat = false;
    for (Answer& a : g_answers)
        if (a.caller == caller && a.id == id && decision - a.decision <= kRepeatUpdates) {
            a.decision = decision;
            repeat = true;
            break;
        }
    if (!repeat) {
        g_answers[g_answersNext] = {caller, id, decision};
        g_answersNext = (g_answersNext + 1) % 8;
    }
    ReleaseSRWLockExclusive(&g_answersLock);
    return repeat;
}

// Event-script prompts, kept alive across the second pass (2026-11-06, a boss fog wall that showed "Traverse the
// mist" and ignored the button). The manager frees, at each update, every prompt that was not asked since the
// previous update (+0xa65f70 drops it to its spent list, +0xa65de0 frees it there), and a press is wiped at the end
// of an update that leaves no prompt chosen (+0xa65a90). With the update in both passes a prompt must therefore be
// asked in both: the event scripts (22-24) ask theirs - EcTestActionButtonRegion, through the locked wrapper whose
// call returns at exe+0xA64B5C on 2.7.2.0 - in pass 1 only, so each one was freed by the pass-2 update, came back
// unchosen, and the press was wiped before it could answer. Measured the same way in players' 0.7.1 logs: every
// prompt that took a press was asked in pass 2 too. So each event-script prompt asked in pass 1 is asked again,
// with the same arguments, right before and right after the second pass - between the two updates either way round
// - with the manager's press bytes (+0x81, +0x82) cleared for the call: kept registered and chosen, never answered.
// The script's own ask in pass 1 stays the only one that can take the press. [VR] Pass2KeepScriptPrompts=false off.
struct HeldPrompt {
    void* self = nullptr;
    int32_t id = 0;
    float f = 0.f;
    uint64_t a4 = 0, a5 = 0, a6 = 0, a7 = 0, a8 = 0;
    uint8_t args[0x30]{};   // what a9 points at: the prompt's 0x30-byte struct, read by the game as three 16-byte loads
    long frame = -1;        // g_frameNo of the script's ask: re-asked only in that same frame
};
uintptr_t g_lockedRa = 0;   // the locked wrapper's return from ExecuteActionButton (the event scripts' path)
SRWLOCK g_heldLock = SRWLOCK_INIT;
HeldPrompt g_held[8];
std::atomic<long> g_heldAsked{0}, g_heldLines{0};

void HoldPrompt(void* self, int32_t id, float f, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8,
                uint64_t a9) {
    HeldPrompt h;
    h.self = self; h.id = id; h.f = f; h.a4 = a4; h.a5 = a5; h.a6 = a6; h.a7 = a7; h.a8 = a8;
    if (!a9 || !SafeRead((const void*)a9, h.args, sizeof(h.args))) return;
    h.frame = g_frameNo.load(std::memory_order_relaxed);
    bool fresh = true;
    AcquireSRWLockExclusive(&g_heldLock);
    int slot = -1, oldest = 0;
    for (int i = 0; i < 8; ++i) {
        const HeldPrompt& e = g_held[i];
        if (e.frame >= 0 && e.id == id && !memcmp(e.args, h.args, sizeof(h.args))) { slot = i; break; }
        if (e.frame < g_held[oldest].frame) oldest = i;
    }
    if (slot >= 0) fresh = g_held[slot].frame < h.frame - 2;
    g_held[slot >= 0 ? slot : oldest] = h;
    ReleaseSRWLockExclusive(&g_heldLock);
    if (fresh && g_heldLines.fetch_add(1, std::memory_order_relaxed) < 24)
        LOGI("[p2watch] action button %d is an event script's, asked in pass 1 only: asked again around the second "
             "pass (never answered there) so the action-button update in pass 2 keeps it (frame %ld)", id, h.frame);
}

// Prompt decisions belong to pass 1 (2026-11-07). The manager's update runs once per frame, in pass 1 (Pass2EventsOnce),
// and in the game without ERVR the first prompt asked after it takes the press - so does pass 1 here: a prompt asked in
// pass 2 first is answered "not pressed" there (the manager's taken mark +0x80 put back), and its press waits at +0x81
// for the same prompt's ask in the next pass 1, before the next update. Players' 0.7.4 logs: every press taken in pass
// 2 by a new grace ("Touch grace", "Rest at grace") was lost; every press taken in pass 1 worked. A prompt that is never
// asked in pass 1 (none seen in the last few frames) keeps its pass-2 answer, so nothing goes dead.
// [VR] Pass2PromptsInPass1=false: 0.7.4's rule (whichever pass asks first takes it).
struct PassOneAsk { uintptr_t caller; int32_t id; long frame; };
SRWLOCK g_passOneLock = SRWLOCK_INIT;
PassOneAsk g_passOne[16]{};
int g_passOneNext = 0;
std::atomic<long> g_deferred{0}, g_deferLines{0}, g_pass2OnlyLines{0};

void NotePassOneAsk(uintptr_t caller, int32_t id) {
    const long frame = g_frameNo.load(std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_passOneLock);
    PassOneAsk* slot = nullptr;
    for (PassOneAsk& a : g_passOne)
        if (a.caller == caller && a.id == id) { slot = &a; break; }
    if (!slot) { slot = &g_passOne[g_passOneNext]; g_passOneNext = (g_passOneNext + 1) % 16; }
    *slot = {caller, id, frame};
    ReleaseSRWLockExclusive(&g_passOneLock);
}

bool AskedInPassOneLately(uintptr_t caller, int32_t id) {
    const long frame = g_frameNo.load(std::memory_order_relaxed);
    bool lately = false;
    AcquireSRWLockShared(&g_passOneLock);
    for (const PassOneAsk& a : g_passOne)
        if (a.caller == caller && a.id == id && a.frame > 0 && frame - a.frame <= 3) { lately = true; break; }
    ReleaseSRWLockShared(&g_passOneLock);
    return lately;
}

// ExecuteActionButton (fromsoftware-rs present_action_button): what every prompt calls each frame - it registers
// the prompt with the manager and answers whether the interact press was for it. Arg 3 is a float (movss,
// 2.7.2.0), 9 args, the answer in al. Every call reaches the game, in either pass; only a prompt's repeated
// "pressed" for one press is answered "not pressed".
using FnExecuteActionButton = uint8_t(__fastcall*)(void*, int32_t, float, uint64_t, uint64_t, uint64_t, uint64_t,
                                                   uint64_t, uint64_t);
FnExecuteActionButton g_origExecute = nullptr;
std::atomic<long> g_calls[2], g_accepted[2], g_acceptLines{0};

uint8_t __fastcall Hook_ExecuteActionButton(void* self, int32_t id, float f, uint64_t a4, uint64_t a5, uint64_t a6,
                                            uint64_t a7, uint64_t a8, uint64_t a9) {
    const int p = FramePumpCurrentPass() == 2 ? 1 : 0;
    const uintptr_t ra = (uintptr_t)_ReturnAddress();
    g_calls[p].fetch_add(1, std::memory_order_relaxed);
    if (g_watch) {
        NoteCaller(ra, p, id);
        NotePair(ra, p, id);
    }
    if (p == 0 && ra == g_lockedRa && g_lockedRa && Cfg().vrPass2KeepScriptPrompts && Cfg().vrPass2ActionButtonUpdate &&
        !Cfg().vrPass2EventsOnce)   // only an update in pass 2 can forget them
        HoldPrompt(self, id, f, a4, a5, a6, a7, a8, a9);
    if (p == 0) NotePassOneAsk(ra, id);
    uint8_t taken = 0;
    const bool decideInPassOne = p == 1 && Cfg().vrPass2PromptsInPass1 && Cfg().vrPass2EventsOnce && self;
    if (decideInPassOne) SafeRead((uint8_t*)self + 0x80, &taken, 1);
    const uint8_t r = g_origExecute(self, id, f, a4, a5, a6, a7, a8, a9);
    if (!r) return 0;
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    if (decideInPassOne) {
        if (AskedInPassOneLately(ra, id)) {
            SafeWrite((uint8_t*)self + 0x80, &taken, 1);   // not taken: the press stays for pass 1's ask
            g_deferred.fetch_add(1, std::memory_order_relaxed);
            if (g_deferLines.fetch_add(1, std::memory_order_relaxed) < 40)
                LOGI("[p2watch] action button %d's press left for pass 1 (asked in pass 2 first, frame %ld, from "
                     "exe+0x%llX)", id, g_frameNo.load(std::memory_order_relaxed),
                     (unsigned long long)((uint8_t*)ra - base));
            return 0;
        }
        if (g_pass2OnlyLines.fetch_add(1, std::memory_order_relaxed) < 12)
            LOGI("[p2watch] action button %d is asked in pass 2 only (from exe+0x%llX): it takes the press there",
                 id, (unsigned long long)((uint8_t*)ra - base));
    }
    if (g_updateHooked && Cfg().vrPass2RepeatPressGuard && RepeatAnswer(ra, id)) {
        g_repeats.fetch_add(1, std::memory_order_relaxed);
        if (g_repeatLines.fetch_add(1, std::memory_order_relaxed) < 100)
            LOGI("[p2watch] action button %d answered the same press again in pass %d (frame %ld, from exe+0x%llX): "
                 "not pressed this time - the game already handled it",
                 id, p + 1, g_frameNo.load(std::memory_order_relaxed), (unsigned long long)((uint8_t*)ra - base));
        return 0;
    }
    if (g_watch) {
        g_accepted[p].fetch_add(1, std::memory_order_relaxed);
        if (g_acceptLines.fetch_add(1, std::memory_order_relaxed) < 300)
            LOGI("[p2watch] action button %d accepted the interact press in pass %d (frame %ld, from exe+0x%llX)",
                 id, p + 1, g_frameNo.load(std::memory_order_relaxed), (unsigned long long)((uint8_t*)ra - base));
    }
    return r;
}

// The action-button system's per-frame update (2.7.2.0+0xa67a20, called once per frame by the world step at
// +0x58f6a0): it resets the 15-slot prompt candidate table, then tail-calls the manager's update (+0xa64d10)
// that decides which prompt the interact press goes to. It and the event manager (25), whose door, lever and chest
// actions it serves, must step together: 0.7.0 ran the update in pass 1 only, every door, lever and chest took the
// press (logged "accepted ... in pass 1") and then did nothing; 0.6.0 had the mirror image (update in both passes,
// the event manager in pass 1 only) and the same dead doors. 0.6.5-0.7.3 ran both in both passes: doors opened, but
// a lift's lever was pulled and the lift never came - the lever's one-frame signal cleared by the event manager's
// second run before the event script, once per frame, saw it. So by default both run once per frame, in pass 1, the
// single-pass game's order: [VR] Pass2EventsOnce (default true since 0.7.4; verified at a lift 2026-11-06) skips
// this update in pass 2 and frame_probe.cpp drops group 25 from it. With Pass2EventsOnce=false, [VR]
// Pass2ActionButtonUpdate=false skips the update alone in pass 2 (0.7.0's behaviour, for comparison).
//
// The entry's first half is not the manager's: it resets CSActionButtonRegionSystem's "nearest prompt of each
// kind" table (action_button_update.h), and a prompt that asks the table is answered "yours" only if it was the
// nearest at the PREVIOUS reset. The event script (22-24, pass 1 only) asks it for every EcTestActionButtonRegion
// prompt - boss fog walls ("Traverse the mist") and the scripts' other area prompts. Reset in both passes, the
// previous reset is always pass 2's, after which the script never runs: the fog wall registers, is compared with
// pass 2's empty slot, and never shows its prompt (traced 2026-11-06 from a co-op session where a fog wall would
// not respond). So when the update runs in pass 2 (Pass2EventsOnce=false) the manager's update runs there alone and
// the table resets once per frame, in pass 1, as in the game without ERVR. [VR] Pass2RegionReset=true puts the
// pass-2 reset back, to compare.
using FnActionButtonUpdate = void(__fastcall*)(void* table, void* arg);
FnActionButtonUpdate g_origUpdate = nullptr;
using FnManagerUpdate = void(__fastcall*)(void* manager, void* arg);
FnManagerUpdate g_managerUpdate = nullptr;   // +0xa64d10, the entry's tail jump
void** g_managerSlot = nullptr;              // the CSActionButtonManImp global the entry loads
std::atomic<long> g_updatesSkipped{0}, g_updatesPass2{0}, g_regionResets[2]{}, g_regionResetsSkipped{0};

// The region table's nearest-so-far, banked at each pass boundary and put back right before pass 1's reset
// (action_button_update.h: BankRegionSlots / CarryRegionBank). Without it a prompt asked in both passes - graces,
// gatherable plants, ground items, dropped runes - registered in one pass and failed the strictly-nearer range check
// in the other, so it was answered in one pass only and the other pass's update forgot it (0.7.3: no grace prompt,
// item prompts flickering, a door ignoring the button). Only while pass 2 does not reset the table itself.
SRWLOCK g_bankLock = SRWLOCK_INIT;
RegionBest g_regionBank[kRegionSlots];
long g_regionBankFrame = -1;                     // frame of the last bank; a carry takes only the last 2 frames'
std::atomic<uint8_t*> g_regionTable{nullptr};    // the entry's table, from this frame's pass 1
std::atomic<long> g_regionTableFrame{-1};
std::atomic<long> g_regionBanked{0}, g_regionCarried{0};

bool RegionBankingOn() {
    const Config& c = Cfg();
    if (!c.vrPass2RegionBank) return false;
    const bool pass2Resets =
        c.vrPass2ActionButtonUpdate && !c.vrPass2EventsOnce && (c.vrPass2RegionReset || !g_managerUpdate);
    return !pass2Resets;
}

// Frame thread, between passes (no task runs). Only with the table pass 1 of this same frame handed the update:
// out of the world (title, loading) the entry does not run and nothing is written.
void BankRegionTable() {
    if (!RegionBankingOn()) return;
    uint8_t* table = g_regionTable.load(std::memory_order_acquire);
    const long frame = g_frameNo.load(std::memory_order_relaxed);
    if (!table || g_regionTableFrame.load(std::memory_order_acquire) != frame) return;
    uint8_t probe[kRegionTableBytes];
    if (!SafeRead(table, probe, sizeof(probe))) return;
    AcquireSRWLockExclusive(&g_bankLock);
    const int n = BankRegionSlots(table, g_regionBank);
    g_regionBankFrame = frame;
    ReleaseSRWLockExclusive(&g_bankLock);
    if (n) g_regionBanked.fetch_add(n, std::memory_order_relaxed);
}

// Pass 1's update entry, right before its reset.
void CarryRegionTable(uint8_t* table) {
    const long frame = g_frameNo.load(std::memory_order_relaxed);
    g_regionTable.store(table, std::memory_order_release);
    g_regionTableFrame.store(frame, std::memory_order_release);
    AcquireSRWLockExclusive(&g_bankLock);
    int n = 0;
    if (RegionBankingOn() && g_regionBankFrame >= frame - 2) n = CarryRegionBank(table, g_regionBank);
    else for (RegionBest& b : g_regionBank) b = RegionBest{};
    g_regionBankFrame = -1;
    ReleaseSRWLockExclusive(&g_bankLock);
    if (n) g_regionCarried.fetch_add(n, std::memory_order_relaxed);
}

void __fastcall Hook_ActionButtonUpdate(void* table, void* arg) {
    if (FramePumpCurrentPass() == 2) {
        if (!Cfg().vrPass2ActionButtonUpdate || Cfg().vrPass2EventsOnce) {
            g_updatesSkipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_updatesPass2.fetch_add(1, std::memory_order_relaxed);
        g_decision.fetch_add(1, std::memory_order_acq_rel);
        void* manager = g_managerSlot ? *g_managerSlot : nullptr;
        if (!Cfg().vrPass2RegionReset && g_managerUpdate && manager) {
            g_regionResetsSkipped.fetch_add(1, std::memory_order_relaxed);
            g_managerUpdate(manager, arg);
            return;
        }
        g_regionResets[1].fetch_add(1, std::memory_order_relaxed);
        g_origUpdate(table, arg);
        return;
    }
    g_decision.fetch_add(1, std::memory_order_acq_rel);
    g_regionResets[0].fetch_add(1, std::memory_order_relaxed);
    if (table) CarryRegionTable((uint8_t*)table);
    g_origUpdate(table, arg);
}

// MoveMapStep's per-frame world update (2.7.2.0 +0xaf94c0, from MoveMapStep's step, group 20): it steps the world's
// managers one after another, among them FieldArea's update (below) and with it the talk scripts. Measured 2026-11-07
// (flat ForcePass2, census above): it has to run in pass 2 - left out, the second eye lost its sky and ambient light
// (27% of the brightness, the August "dark eye", whose producer is no task group but FieldArea's update in here) and
// the frame rate fell. Counted per pass for the census; [VR] Pass2WorldUpdate=false (diagnostic only) leaves it out.
// Arguments: the step and its FD4TaskData (dt at +8).
using FnWorldUpdate = void(__fastcall*)(void*, void*, void*, void*);
FnWorldUpdate g_origWorldUpdate = nullptr;
std::atomic<long> g_worldUpdates[2]{}, g_worldUpdatesSkipped{0};

void __fastcall Hook_WorldUpdate(void* self, void* taskData, void* a3, void* a4) {
    if (FramePumpCurrentPass() == 2) {
        if (!Cfg().vrPass2WorldUpdate) {
            g_worldUpdatesSkipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_worldUpdates[1].fetch_add(1, std::memory_order_relaxed);
    } else {
        g_worldUpdates[0].fetch_add(1, std::memory_order_relaxed);
    }
    g_origWorldUpdate(self, taskData, a3, a4);
}

// FieldArea's per-frame update (2.7.2.0 +0x624710; rcx = the field area, xmm1 = the frame's dt, r8b/r9b and a stacked
// byte = flags; seven MoveMapStep call sites, none reads a result): the world container's step, under which the world's
// gameplay steps - CSWorldTalkManImpl's update (+0xeb14d0, called from +0x6252eb inside it: every grace's, NPC's and
// merchant's talk script) among it. [VR] Pass2FieldUpdate=false leaves it out of pass 2 while the rest of the world
// update (the renderer's, the fade's, the sound's) still runs there.
using FnFieldStep = void(__fastcall*)(void*, float, uint8_t, uint8_t, uint8_t);
FnFieldStep g_origFieldStep = nullptr;
std::atomic<long> g_fieldSteps[2]{}, g_fieldStepsSkipped{0};

void __fastcall Hook_FieldStep(void* field, float dt, uint8_t a3, uint8_t a4, uint8_t a5) {
    if (FramePumpCurrentPass() == 2) {
        if (!Cfg().vrPass2FieldUpdate) {
            g_fieldStepsSkipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_fieldSteps[1].fetch_add(1, std::memory_order_relaxed);
    } else {
        g_fieldSteps[0].fetch_add(1, std::memory_order_relaxed);
    }
    g_origFieldStep(field, dt, a3, a4, a5);
}

// The talk scripts' manager update (2.7.2.0 +0xeb14d0, CSWorldTalkManImpl; its one caller is FieldArea's update at
// +0x6252eb, which reads no result): every grace's, NPC's and merchant's talk script steps under it (each grid area's
// CSWorldGridAreaTalk slot 6 -> each talk -> the EzState machine). FieldArea's update itself has to stay in pass 2 -
// the environment lighting is made inside it (2026-11-07: without it the second eye lost sky and ambient light and
// the frame rate fell from 72 to 46) - so the talk scripts are taken out of pass 2 at their own entry instead.
// [VR] Pass2TalkScripts=true puts them back in both passes (0.7.4).
using FnTalkManUpdate = void(__fastcall*)(void*, void*);
FnTalkManUpdate g_origTalkManUpdate = nullptr;
std::atomic<long> g_talkUpdates[2]{}, g_talkUpdatesSkipped{0};

void __fastcall Hook_TalkManUpdate(void* talkMan, void* ctx) {
    if (FramePumpCurrentPass() == 2) {
        if (!Cfg().vrPass2TalkScripts) {
            g_talkUpdatesSkipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_talkUpdates[1].fetch_add(1, std::memory_order_relaxed);
    } else {
        g_talkUpdates[0].fetch_add(1, std::memory_order_relaxed);
    }
    g_origTalkManUpdate(talkMan, ctx);
}

// The managers the world update steps: every `mov rcx, [rip+X]` in its body loads one (FD4 singletons), named once the
// world is up (LookupLoop) - "[p2watch] the world update steps: ...". Diagnostic: which of them a second pass needs.
constexpr size_t kWorldUpdateBytes = 0x1213;
uintptr_t g_worldManagers[24];
int g_worldManagerCount = 0;

void WorldUpdateInit(uint8_t* wu) {
    static uint8_t body[kWorldUpdateBytes];
    if (!SafeRead(wu, body, sizeof(body))) return;
    // FieldArea's update: its prologue and first block, unique in the exe (2.7.2.0 +0x624710).
    if (uint8_t* fa = AobScan("40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E9 48 81 EC B8 00 00 00 48 C7 45 CF FE FF "
                              "FF FF 0F 29 B4 24 A0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 EF 44 88 4D 99 45 0F B6 "
                              "E8 44 88 45 98 48 8B F1 F3 0F 11 4D 8F")) {
        if (MH_CreateHook(fa, (void*)&Hook_FieldStep, (void**)&g_origFieldStep) == MH_OK && MH_EnableHook(fa) == MH_OK)
            LOGI("[stereo] FieldArea's update (the world's gameplay: talk scripts, map objects; exe+0x%llX) runs %s",
                 (unsigned long long)((uintptr_t)fa - g_imageLo),
                 Cfg().vrPass2FieldUpdate ? "in both passes" : "in pass 1 only ([VR] Pass2FieldUpdate=false)");
        else
            g_origFieldStep = nullptr;
    }
    if (!g_origFieldStep) LOGW("[stereo] FieldArea's update not found: it runs in both passes");
    // The talk scripts' manager update: prologue and first block, unique in the exe (2.7.2.0 +0xeb14d0).
    if (uint8_t* tm = AobScan("40 57 48 83 EC 50 48 C7 44 24 28 FE FF FF FF 48 89 5C 24 60 48 89 6C 24 68 48 89 74 24 78 0F "
                              "29 7C 24 40 48 8B EA 48 8B D9 33 FF 8B C7 89 44 24 20 48 8B 0D")) {
        if (MH_CreateHook(tm, (void*)&Hook_TalkManUpdate, (void**)&g_origTalkManUpdate) == MH_OK &&
            MH_EnableHook(tm) == MH_OK)
            LOGI("[stereo] talk scripts (graces, NPCs, merchants; exe+0x%llX) step %s",
                 (unsigned long long)((uintptr_t)tm - g_imageLo),
                 Cfg().vrPass2TalkScripts ? "in both passes ([VR] Pass2TalkScripts=true)"
                                          : "once per frame, in pass 1, as in the game without ERVR");
        else
            g_origTalkManUpdate = nullptr;
    }
    if (!g_origTalkManUpdate)
        LOGW("[stereo] the talk scripts' update was not found: graces and NPCs step in both passes (a new grace may "
             "ignore the button)");
    for (size_t i = 0; i + 7 <= sizeof(body); ++i) {
        if (body[i] != 0x48 || body[i + 1] != 0x8B || body[i + 2] != 0x0D) continue;
        int32_t d;
        memcpy(&d, body + i + 3, 4);
        const uintptr_t slot = (uintptr_t)wu + i + 7 + (intptr_t)d;
        if (slot < g_imageLo || slot + 8 > g_imageHi) continue;
        bool seen = false;
        for (int k = 0; k < g_worldManagerCount; ++k) seen |= g_worldManagers[k] == slot;
        if (!seen && g_worldManagerCount < (int)(sizeof(g_worldManagers) / sizeof(g_worldManagers[0])))
            g_worldManagers[g_worldManagerCount++] = slot;
    }
}

void LogWorldManagers() {
    std::string line;
    for (int k = 0; k < g_worldManagerCount; ++k) {
        // Not every slot holds an object (2026-11-07: one held a value, and reading a "vtable" through it crashed the
        // game): the vtable and the locator before it must both be the exe's, the locator's signature 1, before
        // RttiNameOfVTable reads anything through them.
        uintptr_t obj = 0, vt = 0, col = 0;
        uint32_t sig = 0;
        if (!SafeRead((void*)g_worldManagers[k], &obj, sizeof(obj)) || obj < 0x10000 ||
            !SafeRead((void*)obj, &vt, sizeof(vt)) || vt < g_imageLo + 8 || vt >= g_imageHi ||
            !SafeRead((void*)(vt - 8), &col, sizeof(col)) || col < g_imageLo || col + 24 > g_imageHi ||
            !SafeRead((void*)col, &sig, sizeof(sig)) || sig != 1)
            continue;
        std::string name = RttiNameOfVTable((void**)vt);
        char part[160];
        snprintf(part, sizeof(part), "%s exe+0x%llX %s", line.empty() ? "" : ",",
                 (unsigned long long)(g_worldManagers[k] - g_imageLo), name.empty() ? "?" : name.c_str());
        line += part;
    }
    if (!line.empty()) LOGI("[p2watch] the world update steps:%s", line.c_str());
}

// Frame thread, between passes (no task runs): the event-script prompts asked in this frame's pass 1, asked again
// with the manager's press bytes cleared for the call, so the prompt stays registered (and chosen) for the update on
// the other side of this boundary without the call ever taking the press. Only while the update runs in pass 2.
void KeepHeldPromptsAlive() {
    const Config& c = Cfg();
    if (!c.vrPass2KeepScriptPrompts || !c.vrPass2ActionButtonUpdate || c.vrPass2EventsOnce) return;
    if (!g_origExecute || !g_managerSlot) return;
    auto* manager = (uint8_t*)*g_managerSlot;
    if (!manager) return;
    const long frame = g_frameNo.load(std::memory_order_relaxed);
    HeldPrompt now[8];
    int n = 0;
    AcquireSRWLockShared(&g_heldLock);
    for (const HeldPrompt& e : g_held)
        if (e.frame == frame && e.self == manager) now[n++] = e;
    ReleaseSRWLockShared(&g_heldLock);
    for (int i = 0; i < n; ++i) {
        HeldPrompt& h = now[i];
        uint8_t press[2];
        if (!SafeRead(manager + 0x81, press, 2)) return;
        manager[0x81] = 0;
        manager[0x82] = 0;
        g_origExecute(h.self, h.id, h.f, h.a4, h.a5, h.a6, h.a7, h.a8, (uint64_t)h.args);
        manager[0x81] = press[0];
        manager[0x82] = press[1];
        g_heldAsked.fetch_add(1, std::memory_order_relaxed);
    }
}

// Game systems that step in the second eye's pass, counted per pass for the 5 s line. The second pass is meant to
// draw the same moment again; a system that steps there too can take a press, a one-frame signal or a state change
// a second time - how doors, item cards, lifts, fog walls and graces broke one after another. Each point is a
// class's virtual function, its vtable slot pointed at a register-preserving stub (midhook.h) that counts the call
// and jumps on: nothing about the call changes.
struct CensusPoint {
    const char* cls;
    int slot;
    const char* what;
    std::atomic<long> n[2];
};
CensusPoint g_census[] = {
    // Talk scripts: graces, NPCs, merchants. Slot 1 answers each condition a talk script's current state asks
    // (2.7.2.0 +0xea3b90). A grace's "Touch grace" and "Rest at grace" prompts (action buttons 6000/6100/6101) are
    // asked from inside it (exe+0xEA59B2), and players' 0.7.4 logs show those presses taken in pass 2 and lost.
    {"CS::CSEzStateTalkEnv", 1, "talk-script queries", {}},
    // A grid area's talk scripts, stepped one by one (+0xeb2970, CSWorldTalkManImpl's update calls it per area, from
    // MoveMapStep's world update). The object-action, AI and navmesh areas share the layout but their slot 6 never
    // ran in play (measured 2026-11-07), so they are not counted.
    {"CS::CSWorldGridAreaTalk", 6, "area talk updates", {}},
};
constexpr int kCensus = (int)(sizeof(g_census) / sizeof(g_census[0]));

// The return addresses above a census point's call, from its stack: each a code address in the exe right after a
// call instruction (E8 rel32, FF 15 [rip], or FF /2 through a register: the virtual calls a step's update makes).
// Names who drives the system in each pass - which game step or task to keep out of the second one.
int StackCallers(uintptr_t rsp, uintptr_t* out, int max) {
    const auto stackBase = (uintptr_t)((NT_TIB*)NtCurrentTeb())->StackBase;
    int n = 0;
    for (uintptr_t p = rsp; n < max && p + 8 <= stackBase && p < rsp + 0x1800; p += 8) {
        uintptr_t v = 0;
        if (!SafeRead((void*)p, &v, sizeof(v)) || v < g_imageLo + 8 || v >= g_imageHi) continue;
        uint8_t b[8];
        if (!SafeRead((void*)(v - 8), b, sizeof(b))) continue;
        const bool call = b[3] == 0xE8 ||                                     // call rel32
                          (b[2] == 0xFF && b[3] == 0x15) ||                   // call [rip+disp32]
                          (b[6] == 0xFF && (b[7] & 0xF8) == 0xD0) ||          // call reg
                          (b[6] == 0xFF && (b[7] & 0xF8) == 0x10 && (b[7] & 7) != 4 && (b[7] & 7) != 5) ||  // [reg]
                          (b[5] == 0xFF && (b[6] & 0x38) == 0x10 && (b[6] >> 6) == 1) ||   // call [reg+disp8]
                          (b[2] == 0xFF && (b[3] & 0x38) == 0x10 && (b[3] >> 6) == 2);     // call [reg+disp32]
        if (call) out[n++] = v;
    }
    return n;
}

std::atomic<bool> g_censusStackLogged[kCensus][2];

void CensusCount(MidHookRegs* regs, void* user) {
    auto* c = static_cast<CensusPoint*>(user);
    const int p = FramePumpCurrentPass() == 2 ? 1 : 0;
    c->n[p].fetch_add(1, std::memory_order_relaxed);
    // Once per point, pass and session, in the world: who called it (pass 1's once pass 2 has run, so it is the
    // stepping in play, not a load's first evaluation).
    std::atomic<bool>& logged = g_censusStackLogged[c - g_census][p];
    if (logged.load(std::memory_order_relaxed) || !GameERPlayerActive() ||
        (p == 0 && !c->n[1].load(std::memory_order_relaxed)))
        return;
    if (logged.exchange(true)) return;
    uintptr_t chain[24] = {};
    const int n = StackCallers(regs->rsp, chain, 24);
    char line[768] = "";
    for (int i = 0, at = 0; i < n && at < (int)sizeof(line) - 24; ++i)
        at += snprintf(line + at, sizeof(line) - at, "%sexe+0x%llX", i ? " <- " : "",
                       (unsigned long long)(chain[i] - g_imageLo));
    LOGI("[p2watch] %s in pass %d come from %s (thread %lu, %s)", c->what, p + 1, line, GetCurrentThreadId(),
         GetCurrentThreadId() == FramePumpFrameThread() ? "the frame thread" : "another thread");
}

void InstallCensus() {
    for (CensusPoint& c : g_census) {
        void** vt = FindVTable(c.cls);
        void* fn = vt ? vt[c.slot] : nullptr;
        if (!fn || (uintptr_t)fn < g_imageLo || (uintptr_t)fn >= g_imageHi) {
            LOGW("[p2watch] %s are not counted: %s[%d] not found", c.what, c.cls, c.slot);
            continue;
        }
        void* stub = MidHookMakeStub(fn, &CensusCount, &c);
        DWORD old = 0;
        if (!stub || !VirtualProtect(&vt[c.slot], sizeof(void*), PAGE_READWRITE, &old)) {
            LOGW("[p2watch] %s are not counted: the stub for %s[%d] was not placed", c.what, c.cls, c.slot);
            continue;
        }
        vt[c.slot] = stub;
        VirtualProtect(&vt[c.slot], sizeof(void*), old, &old);
        LOGI("[p2watch] counting %s per pass: %s[%d] = exe+0x%llX", c.what, c.cls, c.slot,
             (unsigned long long)((uintptr_t)fn - g_imageLo));
    }
}

bool TakeOne(Watched& w, uint8_t* out) {
    uint8_t* o = w.obj.load(std::memory_order_acquire);
    void** vt = w.vt.load(std::memory_order_acquire);
    void** cur = nullptr;
    if (!o || !SafeRead(o, &cur, sizeof(cur)) || cur != vt) return false;
    return SafeRead(o + w.off, out, w.len);
}

Snap Take() {
    Snap s{};
    for (int i = 0; i < kObjs; ++i) s.valid[i] = TakeOne(*g_all[i], s.bytes[i]);
    return s;
}

uint32_t Dword(const uint8_t* b, size_t at) { uint32_t v; memcpy(&v, b + at, 4); return v; }
uint64_t Qword(const uint8_t* b, size_t at) { uint64_t v; memcpy(&v, b + at, 8); return v; }

bool SpendLine(Label l) { return g_lines[l] < kLineCap[l] && ++g_lines[l]; }

// The action-button manager, a dword at a time: the bytes that changed, until each has flipped a few times.
void DiffAbm(const uint8_t* a, const uint8_t* b, Label l) {
    std::string line;
    bool any = false;
    for (size_t at = 0; at + 4 <= g_abm.len; at += 4) {
        const uint32_t x = Dword(a, at), y = Dword(b, at);
        if (x == y) continue;
        any = true;
        if (g_flips[0][at] < 0xFFFF) ++g_flips[0][at];
        if (g_flips[0][at] > 6) continue;
        char part[48];
        snprintf(part, sizeof(part), " +0x%02zX %08X->%08X", at + g_abm.off, x, y);
        line += part;
    }
    if (!any) return;
    ++g_changes[0][l];
    if (!line.empty() && SpendLine(l))
        LOGI("[p2watch] %s changed the %s:%s (frame %ld)", kLabel[l], g_abm.tag, line.c_str(),
             g_frameNo.load(std::memory_order_relaxed));
}

// The popup queue: its count and every record in it, whenever any byte moved.
void DiffQueue(const uint8_t* a, const uint8_t* b, Label l) {
    if (!memcmp(a, b, g_items.len)) return;
    ++g_changes[1][l];
    if (!SpendLine(l)) return;
    const uint64_t n0 = Qword(a, kQueueCount), n1 = Qword(b, kQueueCount);
    std::string recs;
    for (uint64_t i = 0; i < n1 && i < 10; ++i) {
        char part[64];
        snprintf(part, sizeof(part), " [item %08X x%u %08X %08X]", Dword(b, i * 16), Dword(b, i * 16 + 4),
                 Dword(b, i * 16 + 8), Dword(b, i * 16 + 12));
        recs += part;
    }
    LOGI("[p2watch] %s changed the %s: count %llu -> %llu%s (frame %ld)", kLabel[l], g_items.tag,
         (unsigned long long)n0, (unsigned long long)n1, recs.c_str(), g_frameNo.load(std::memory_order_relaxed));
}

// UI elements: bit 0 created, bit 1 visible (more bits on some); each change, until an element animates.
void DiffUi(const uint8_t* a, const uint8_t* b, Label l) {
    std::string line;
    bool any = false;
    for (size_t i = 0; i < g_menu.len; ++i) {
        if (a[i] == b[i]) continue;
        any = true;
        if (g_flips[2][i] < 0xFFFF) ++g_flips[2][i];
        if (g_flips[2][i] > 12) continue;
        char part[32];
        snprintf(part, sizeof(part), " [0x%02zX] %02X->%02X", i, a[i], b[i]);
        line += part;
    }
    if (!any) return;
    ++g_changes[2][l];
    if (!line.empty() && SpendLine(l))
        LOGI("[p2watch] %s changed ui elements:%s (frame %ld)", kLabel[l], line.c_str(),
             g_frameNo.load(std::memory_order_relaxed));
}

void Boundary(int boundary) {
    const Snap now = Take();
    if (g_haveLast) {
        // Before pass 2 the diff is pass 1's, after it pass 2's; at a frame's begin it is the time between frames,
        // or the whole of pass 1 when no second pass ran.
        const Label l = boundary == 1 ? kPass1 : boundary == 2 ? kPass2 : g_lastBoundary == 2 ? kOutside : kPass1;
        if (now.valid[0] && g_last.valid[0]) DiffAbm(g_last.bytes[0], now.bytes[0], l);
        if (now.valid[1] && g_last.valid[1]) DiffQueue(g_last.bytes[1], now.bytes[1], l);
        if (now.valid[2] && g_last.valid[2]) DiffUi(g_last.bytes[2], now.bytes[2], l);
    }
    g_last = now;
    g_haveLast = true;
    g_lastBoundary = boundary;
}

void Summary() {
    const ULONGLONG t = GetTickCount64();
    if (t - g_lastSummary < 5000) return;
    g_lastSummary = t;
    const long c1 = g_calls[0].exchange(0), c2 = g_calls[1].exchange(0);
    const long a1 = g_accepted[0].exchange(0), a2 = g_accepted[1].exchange(0);
    const long skipped = g_updatesSkipped.exchange(0);
    const long ranPass2 = g_updatesPass2.exchange(0);
    const long repeats = g_repeats.exchange(0);
    const long resets1 = g_regionResets[0].exchange(0), resets2 = g_regionResets[1].exchange(0);
    const long resetsSkipped = g_regionResetsSkipped.exchange(0);
    const long heldAsked = g_heldAsked.exchange(0);
    const long banked = g_regionBanked.exchange(0), carried = g_regionCarried.exchange(0);
    long ch[kObjs][3];
    memcpy(ch, g_changes, sizeof(ch));
    memset(g_changes, 0, sizeof(g_changes));
    long changes = 0;
    for (auto& o : ch) for (long v : o) changes += v;
    // Per prompt caller: calls in pass 1 / pass 2, and how many of them came from off the frame thread.
    std::string callers;
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    for (Caller& c : g_callers) {
        const uintptr_t ra = c.ra.load(std::memory_order_acquire);
        if (!ra) break;
        const long n1 = c.n[0].exchange(0), n2 = c.n[1].exchange(0), off = c.offThread.exchange(0);
        if (!n1 && !n2) continue;
        char part[96];
        const unsigned long long rva = (unsigned long long)((uint8_t*)ra - base);
        if (off) snprintf(part, sizeof(part), " exe+0x%llX %ld/%ld (%ld off the frame thread)", rva, n1, n2, off);
        else     snprintf(part, sizeof(part), " exe+0x%llX %ld/%ld", rva, n1, n2);
        callers += part;
    }
    // Gameplay counted per pass (census points above): "talk-script queries 812/806" means talk scripts step in both.
    std::string census;
    long censusAny = 0;
    if (const long deferred = g_deferred.exchange(0)) {
        char part[64];
        snprintf(part, sizeof(part), " prompt presses left for pass 1 %ld,", deferred);
        census += part;
        censusAny += deferred;
    }
    {
        const long w1 = g_worldUpdates[0].exchange(0), w2 = g_worldUpdates[1].exchange(0);
        const long ws = g_worldUpdatesSkipped.exchange(0);
        if (w1 || w2 || ws) {
            char part[96];
            snprintf(part, sizeof(part), " world updates %ld/%ld (%ld left out of pass 2)", w1, w2, ws);
            census += part;
            censusAny += w1 + w2 + ws;
        }
        const long t1 = g_talkUpdates[0].exchange(0), t2 = g_talkUpdates[1].exchange(0);
        const long tsk = g_talkUpdatesSkipped.exchange(0);
        if (t1 || t2 || tsk) {
            char part[96];
            snprintf(part, sizeof(part), ", talk-script updates %ld/%ld (%ld left out of pass 2)", t1, t2, tsk);
            census += part;
            censusAny += t1 + t2 + tsk;
        }
        const long f1 = g_fieldSteps[0].exchange(0), f2 = g_fieldSteps[1].exchange(0);
        const long fs = g_fieldStepsSkipped.exchange(0);
        if (f1 || f2 || fs) {
            char part[96];
            snprintf(part, sizeof(part), ", field-area updates %ld/%ld (%ld left out of pass 2)", f1, f2, fs);
            census += part;
            censusAny += f1 + f2 + fs;
        }
    }
    for (CensusPoint& c : g_census) {
        const long n1 = c.n[0].exchange(0, std::memory_order_relaxed), n2 = c.n[1].exchange(0, std::memory_order_relaxed);
        if (!n1 && !n2) continue;
        censusAny += n1 + n2;
        char part[96];
        snprintf(part, sizeof(part), "%s %s %ld/%ld", census.empty() ? "" : ",", c.what, n1, n2);
        census += part;
    }
    if (!a1 && !a2 && !changes && !c1 && !c2 && !repeats && !ranPass2 && !censusAny) return;
    LOGI("[p2watch] 5 s: interact presses accepted pass 1 %ld / pass 2 %ld (of %ld / %ld prompt calls), repeated "
         "answers dropped %ld, action-button updates in pass 2 run %ld / skipped %ld, prompt region table resets "
         "pass 1 %ld / pass 2 %ld (%ld left out), nearest prompts banked between passes %ld / carried into the reset "
         "%ld, event-script prompts asked again around pass 2 %ld | changes in "
         "pass 1 / pass 2 / between frames: action-button state %ld/%ld/%ld, item popup queue %ld/%ld/%ld, ui "
         "%ld/%ld/%ld%s%s%s%s",
         a1, a2, c1, c2, repeats, ranPass2, skipped, resets1, resets2, resetsSkipped, banked, carried, heldAsked,
         ch[0][0], ch[0][1], ch[0][2],
         ch[1][0], ch[1][1], ch[1][2], ch[2][0], ch[2][1], ch[2][2],
         callers.empty() ? "" : " | prompts asked from (pass 1/pass 2):", callers.c_str(),
         census.empty() ? "" : " | game systems stepping (pass 1/pass 2):", census.c_str());
}

// The managers exist from boot; look them up once the world is up (the scan reads the whole module), and
// again only if one stops answering to its vtable.
void LookupLoop() {
    ULONGLONG nextTry[kObjs] = {};
    bool managersLogged = false;
    for (;;) {
        Sleep(2000);
        if (!GameERPlayerActive()) continue;
        if (!managersLogged) {
            managersLogged = true;
            LogWorldManagers();
        }
        for (int i = 0; i < kObjs; ++i) {
            Watched& w = *g_all[i];
            uint8_t* o = w.obj.load(std::memory_order_acquire);
            void** vt = w.vt.load(std::memory_order_acquire);
            void** cur = nullptr;
            if (o && SafeRead(o, &cur, sizeof(cur)) && cur == vt) continue;
            const ULONGLONG t = GetTickCount64();
            if (t < nextTry[i]) continue;
            nextTry[i] = t + 30000;
            void** nvt = nullptr;
            uint8_t* n = FindSingletonInstance(w.singleton, w.rtti, false, &nvt);
            w.vt.store(nvt, std::memory_order_release);
            w.obj.store(n, std::memory_order_release);
            if (n) LOGI("[p2watch] watching the %s: %s %p", w.tag, w.rtti, (void*)n);
            else LOGW("[p2watch] %s not found (%s) - its changes are not logged", w.tag, w.rtti);
        }
    }
}
} // namespace

void Pass2InteractInit() {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    {
        const auto* dos = (const IMAGE_DOS_HEADER*)base;
        const auto* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        g_imageLo = (uintptr_t)base;
        g_imageHi = (uintptr_t)base + nt->OptionalHeader.SizeOfImage;
    }
    // 2.7.2.0 +0xa64680. The first 0x41 bytes are unique in the exe and spell out the argument use (r9b byte,
    // xmm2 float, edx int, rcx this), so a changed signature misses instead of mis-hooking.
    uint8_t* exec = AobScan("48 8B C4 55 41 54 41 55 41 56 41 57 48 8D 68 C9 48 81 EC F0 00 00 00 48 C7 45 B7 FE FF "
                            "FF FF 48 89 58 08 48 89 70 10 48 89 78 18 0F 29 70 C8 0F 29 78 B8 45 0F B6 F1 0F 28 FA 8B "
                            "F2 4C 8B E9");
    // 2.7.2.0 +0xa67a20: the candidate-table reset loop (stride 0x48, 15 slots, FLT_MAX distances), unique.
    uint8_t* upd = AobScan("40 53 48 83 EC 20 48 83 C1 18 48 8B DA 4C 8D 81 38 04 00 00 49 3B C8 74 25 48 83 C1 08 33 "
                           "D2 90 48 8B 01 48 89 41 08 C7 41 F8 FF FF 7F 7F");
    // The locked wrapper's call into ExecuteActionButton (2.7.2.0 +0xa64b57, the event scripts' path from
    // EcTestActionButtonRegion): its return address marks the prompts asked again around pass 2 (HoldPrompt).
    if (uint8_t* site = AobScan("44 0F B6 CB 0F 28 D6 8B D7 48 8B CE E8 ?? ?? ?? ?? 0F B6 D8 48 8B CD E8")) {
        int32_t d = 0;
        memcpy(&d, site + 13, 4);
        if (exec && site + 17 + d == exec) g_lockedRa = (uintptr_t)(site + 17);
    }
    // The entry's tail: the manager's global and its own update, for running that update without the region
    // table's reset in pass 2. Read before hooking (MinHook rewrites only the first bytes).
    AbmUpdateTail tail;
    uint8_t entry[0xC0];
    if (upd && SafeRead(upd, entry, sizeof(entry)) && DecodeAbmUpdateTail(entry, sizeof(entry), (uintptr_t)upd, &tail) &&
        tail.managerSlot > g_imageLo && tail.managerSlot + 8 <= g_imageHi && tail.managerUpdate > g_imageLo &&
        tail.managerUpdate < g_imageHi) {
        g_managerSlot = (void**)tail.managerSlot;
        g_managerUpdate = (FnManagerUpdate)tail.managerUpdate;
    }
    // 2.7.2.0 +0xaf94c0, MoveMapStep's world update: its prologue and first block, unique in the exe.
    if (uint8_t* wu = AobScan("48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC D0 00 00 00 48 C7 45 A7 FE "
                              "FF FF FF 48 89 58 18 0F 29 70 B8 0F 29 78 A8 44 0F 29 40 98 44 0F 29 48 88 44 0F 29 90 78 "
                              "FF FF FF 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 CF 4C 8B FA 48 8B D9 33 FF 89 7D 87 0F 57 FF")) {
        WorldUpdateInit(wu);   // reads the body before the hook rewrites its first bytes
        if (MH_CreateHook(wu, (void*)&Hook_WorldUpdate, (void**)&g_origWorldUpdate) == MH_OK && MH_EnableHook(wu) == MH_OK)
            LOGI("[stereo] MoveMapStep's world update (talk scripts and the other world managers, exe+0x%llX) runs %s",
                 (unsigned long long)(wu - base),
                 Cfg().vrPass2WorldUpdate ? "in both passes" : "in pass 1 only ([VR] Pass2WorldUpdate=false)");
        else
            LOGW("[stereo] MoveMapStep's world update found but not hooked: it runs in both passes");
    } else {
        LOGW("[stereo] MoveMapStep's world update not found: it runs in both passes");
    }
    const bool execOk = exec && MH_CreateHook(exec, (void*)&Hook_ExecuteActionButton, (void**)&g_origExecute) == MH_OK &&
                        MH_EnableHook(exec) == MH_OK;
    const bool updOk = upd && MH_CreateHook(upd, (void*)&Hook_ActionButtonUpdate, (void**)&g_origUpdate) == MH_OK &&
                       MH_EnableHook(upd) == MH_OK;
    g_updateHooked = updOk;
    if (execOk && updOk) {
        const Config& c = Cfg();
        const bool pass2Update = c.vrPass2ActionButtonUpdate && !c.vrPass2EventsOnce;
        LOGI("[stereo] action buttons: their update runs %s (exe+0x%llX); a prompt that answers the same press "
             "again within %u updates is told not pressed%s (exe+0x%llX)",
             c.vrPass2EventsOnce ? "once per frame, in pass 1, with the event manager, as in the game without ERVR"
             : c.vrPass2ActionButtonUpdate ? "in both passes, with the event manager's doors, levers and chests "
                                             "([VR] Pass2EventsOnce=false: a lift may ignore its pulled lever)"
                                           : "in pass 1 only ([VR] Pass2ActionButtonUpdate=false: doors, levers "
                                             "and chests ignore the button)",
             (unsigned long long)(upd - base), kRepeatUpdates,
             c.vrPass2RepeatPressGuard ? "" : " - OFF, [VR] Pass2RepeatPressGuard=false",
             (unsigned long long)(exec - base));
        if (pass2Update && g_managerUpdate)
            LOGI("[stereo] action buttons: the nearest-prompt table (fog walls, the event scripts' area prompts) "
                 "resets once per frame, in pass 1, with the scripts that ask it%s; pass 2 runs the manager's update "
                 "alone (exe+0x%llX, manager at exe+0x%llX)",
                 c.vrPass2RegionReset ? " - NOT NOW: [VR] Pass2RegionReset=true resets it in pass 2 too, and fog "
                                        "walls offer no prompt" : "",
                 (unsigned long long)(tail.managerUpdate - g_imageLo),
                 (unsigned long long)(tail.managerSlot - g_imageLo));
        else if (pass2Update)
            LOGW("[stereo] action buttons: the update's tail did not decode (manager update and global not found): "
                 "the nearest-prompt table resets in both passes, and boss fog walls and the event scripts' other "
                 "area prompts will not appear");
        if (RegionBankingOn())
            LOGI("[stereo] action buttons: each kind's nearest prompt is banked at both pass boundaries and carried "
                 "into the reset, so a prompt asked in both passes (graces, plants, items, runes) is answered in both");
        else if (!c.vrPass2RegionBank)
            LOGI("[stereo] action buttons: nearest prompts are not carried across the passes ([VR] "
                 "Pass2RegionBank=false): graces and items may show no prompt");
        if (pass2Update && g_lockedRa)
            LOGI("[stereo] action buttons: the event scripts' prompts (asked in pass 1 only, through exe+0x%llX) are "
                 "asked again around the second pass so its update keeps them and the button reaches them%s",
                 (unsigned long long)(g_lockedRa - g_imageLo),
                 c.vrPass2KeepScriptPrompts ? "" : " - OFF, [VR] Pass2KeepScriptPrompts=false");
        else if (pass2Update)
            LOGW("[stereo] action buttons: the event scripts' prompt call was not found: boss fog walls and their other "
                 "area prompts show but may ignore the button");
    } else
        LOGW("[stereo] action-button hooks incomplete (prompt call %s, update %s): the second eye's pass can answer the "
             "interact button again (an item card left on screen)", execOk ? "ok" : "not hooked",
             updOk ? "ok" : "not hooked");
    if (Cfg().vrPass2Watch) {
        InstallCensus();
        std::thread(LookupLoop).detach();
        g_watch = true;
    }
}

// [Experimental] TestScriptPrompt: a prompt asked once per frame outside both passes, before the pass-1 update -
// where the event scripts' asks sit relative to the updates - held like theirs when Pass2KeepScriptPrompts is on.
// What it takes is logged, so an unattended run can show a script-style prompt taking a press.
void TestScriptPromptAsk() {
    const int id = Cfg().testScriptPrompt;
    if (id <= 0 || !g_origExecute || !g_managerSlot || !GameERPlayerActive()) return;
    void* manager = *g_managerSlot;
    if (!manager) return;
    struct alignas(16) Args { uint8_t b[0x30]; } args{};
    const uint64_t none = ~0ull;
    memcpy(args.b + 4, &none, 8);   // EcTestActionButtonRegion's: byte 0 clear, -1 at +4, the rest zero
    if (Cfg().vrPass2KeepScriptPrompts) HoldPrompt(manager, id, 0.f, 0, 0, 0, 0, 0, (uint64_t)&args);
    static std::atomic<long> s_asks{0}, s_taken{0};
    const long asks = s_asks.fetch_add(1, std::memory_order_relaxed) + 1;
    if (asks == 1) LOGI("[p2watch] test prompt %d asked at each frame's start (kept alive around pass 2: %s)", id,
                        Cfg().vrPass2KeepScriptPrompts ? "yes" : "no, Pass2KeepScriptPrompts=false");
    if (g_origExecute(manager, id, 0.f, 0, 0, 0, 0, 0, (uint64_t)&args))
        LOGI("[p2watch] test prompt %d took the press (frame %ld, ask %ld, %ld taken so far)", id,
             g_frameNo.load(std::memory_order_relaxed), asks, s_taken.fetch_add(1, std::memory_order_relaxed) + 1);
}

void Pass2InteractFrameBegin(long frame) {
    g_frameNo.store(frame, std::memory_order_relaxed);   // the repeat check's frame bound needs it watched or not
    TestScriptPromptAsk();
    if (!g_watch) return;
    Boundary(0);
    Summary();
}

void Pass2InteractBeforePass2() {
    if (g_watch) Boundary(1);
    BankRegionTable();
    KeepHeldPromptsAlive();   // after the diff: what the re-ask changes is not pass 1's
}
void Pass2InteractAfterPass2() {
    if (g_watch) Boundary(2);
    BankRegionTable();
    KeepHeldPromptsAlive();
}

uint8_t* Pass2InteractAbmTarget(uint32_t* outLen) {
    uint8_t* o = g_abm.obj.load(std::memory_order_acquire);
    if (!o) return nullptr;
    if (outLen) *outLen = 8;
    return o + 0x28;
}
} // namespace ervr
