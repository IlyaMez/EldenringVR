#include "pass2_interact.h"
#include "config.h"
#include "frame_probe.h"
#include "game_er.h"
#include "game_memory.h"
#include "log.h"
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

// The decisions made so far: the update below counts the ones that ran. A prompt's "pressed" answer belongs to
// the decision current when it is asked.
std::atomic<uint32_t> g_decision{0};
bool g_updateHooked = false;   // without the update hook there are no decisions to key answers on

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

// A prompt answers "pressed" at most once per decision. With the decision made once per frame (pass 2 skips the
// update), a prompt asked in both passes would otherwise answer the same press twice - an item handled twice,
// its card left on screen. Kept per prompt (caller + action button id): one press is one decision's winner, so a
// second "pressed" from the same prompt for the same decision is always the repeat. The frame bound (this frame
// or the next) lets a prompt asked before the update - the next frame's pass 1, about the last decision - still
// count as a repeat, and keeps a decision that stops updating (a state with no world step) from going quiet.
struct Answer { uintptr_t caller; int32_t id; uint32_t decision; long frame; };
SRWLOCK g_answersLock = SRWLOCK_INIT;
Answer g_answers[8]{};
int g_answersNext = 0;
std::atomic<long> g_repeats{0}, g_repeatLines{0};

bool RepeatAnswer(uintptr_t caller, int32_t id) {
    const uint32_t decision = g_decision.load(std::memory_order_acquire);
    const long frame = g_frameNo.load(std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_answersLock);
    bool repeat = false;
    for (const Answer& a : g_answers)
        if (a.caller == caller && a.id == id && a.decision == decision && frame - a.frame <= 1) { repeat = true; break; }
    if (!repeat) {
        g_answers[g_answersNext] = {caller, id, decision, frame};
        g_answersNext = (g_answersNext + 1) % 8;
    }
    ReleaseSRWLockExclusive(&g_answersLock);
    return repeat;
}

// ExecuteActionButton (fromsoftware-rs present_action_button): what every prompt calls each frame - it registers
// the prompt with the manager and answers whether the interact press was for it. Arg 3 is a float (movss,
// 2.7.2.0), 9 args, the answer in al. Every call reaches the game, in either pass; only a repeated "pressed" for
// one decision is answered "not pressed".
using FnExecuteActionButton = uint8_t(__fastcall*)(void*, int32_t, float, uint64_t, uint64_t, uint64_t, uint64_t,
                                                   uint64_t, uint64_t);
FnExecuteActionButton g_origExecute = nullptr;
std::atomic<long> g_calls[2], g_accepted[2], g_acceptLines{0};

uint8_t __fastcall Hook_ExecuteActionButton(void* self, int32_t id, float f, uint64_t a4, uint64_t a5, uint64_t a6,
                                            uint64_t a7, uint64_t a8, uint64_t a9) {
    const int p = FramePumpCurrentPass() == 2 ? 1 : 0;
    const uintptr_t ra = (uintptr_t)_ReturnAddress();
    g_calls[p].fetch_add(1, std::memory_order_relaxed);
    if (g_watch) NoteCaller(ra, p, id);
    const uint8_t r = g_origExecute(self, id, f, a4, a5, a6, a7, a8, a9);
    if (!r) return 0;
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    if (g_updateHooked && !Cfg().vrPass2ActionButtons && RepeatAnswer(ra, id)) {
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
// that decides which prompt the interact press goes to. Skipped in pass 2.
using FnActionButtonUpdate = void(__fastcall*)(void* table, void* arg);
FnActionButtonUpdate g_origUpdate = nullptr;
std::atomic<long> g_updatesSkipped{0};

void __fastcall Hook_ActionButtonUpdate(void* table, void* arg) {
    if (FramePumpCurrentPass() == 2 && !Cfg().vrPass2ActionButtons) {
        g_updatesSkipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_decision.fetch_add(1, std::memory_order_acq_rel);
    g_origUpdate(table, arg);
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
    const long repeats = g_repeats.exchange(0);
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
    if (!a1 && !a2 && !changes && !c1 && !c2 && !repeats) return;
    LOGI("[p2watch] 5 s: interact presses accepted pass 1 %ld / pass 2 %ld (of %ld / %ld prompt calls), repeated "
         "answers dropped %ld, action-button updates skipped in pass 2 %ld | changes in pass 1 / pass 2 / between "
         "frames: action-button state %ld/%ld/%ld, item popup queue %ld/%ld/%ld, ui %ld/%ld/%ld%s%s",
         a1, a2, c1, c2, repeats, skipped, ch[0][0], ch[0][1], ch[0][2], ch[1][0], ch[1][1], ch[1][2], ch[2][0],
         ch[2][1], ch[2][2], callers.empty() ? "" : " | prompts asked from (pass 1/pass 2):", callers.c_str());
}

// The managers exist from boot; look them up once the world is up (the scan reads the whole module), and
// again only if one stops answering to its vtable.
void LookupLoop() {
    ULONGLONG nextTry[kObjs] = {};
    for (;;) {
        Sleep(2000);
        if (!GameERPlayerActive()) continue;
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
    // 2.7.2.0 +0xa64680. The first 0x41 bytes are unique in the exe and spell out the argument use (r9b byte,
    // xmm2 float, edx int, rcx this), so a changed signature misses instead of mis-hooking.
    uint8_t* exec = AobScan("48 8B C4 55 41 54 41 55 41 56 41 57 48 8D 68 C9 48 81 EC F0 00 00 00 48 C7 45 B7 FE FF "
                            "FF FF 48 89 58 08 48 89 70 10 48 89 78 18 0F 29 70 C8 0F 29 78 B8 45 0F B6 F1 0F 28 FA 8B "
                            "F2 4C 8B E9");
    // 2.7.2.0 +0xa67a20: the candidate-table reset loop (stride 0x48, 15 slots, FLT_MAX distances), unique.
    uint8_t* upd = AobScan("40 53 48 83 EC 20 48 83 C1 18 48 8B DA 4C 8D 81 38 04 00 00 49 3B C8 74 25 48 83 C1 08 33 "
                           "D2 90 48 8B 01 48 89 41 08 C7 41 F8 FF FF 7F 7F");
    const bool execOk = exec && MH_CreateHook(exec, (void*)&Hook_ExecuteActionButton, (void**)&g_origExecute) == MH_OK &&
                        MH_EnableHook(exec) == MH_OK;
    const bool updOk = upd && MH_CreateHook(upd, (void*)&Hook_ActionButtonUpdate, (void**)&g_origUpdate) == MH_OK &&
                       MH_EnableHook(upd) == MH_OK;
    g_updateHooked = updOk;
    if (execOk && updOk)
        LOGI("[stereo] action buttons decide once per frame: the second eye's pass skips their update (exe+0x%llX); "
             "every prompt is still asked, in either pass, and one that answers the same press twice gets "
             "not-pressed the second time (exe+0x%llX)%s",
             (unsigned long long)(upd - base), (unsigned long long)(exec - base),
             Cfg().vrPass2ActionButtons ? " - OFF for now, [VR] Pass2ActionButtons=true" : "");
    else
        LOGW("[stereo] action-button hooks incomplete (prompt call %s, update %s): the second eye's pass can answer the "
             "interact button again (doors, item cards)", execOk ? "ok" : "not hooked", updOk ? "ok" : "not hooked");
    if (Cfg().vrPass2Watch) {
        std::thread(LookupLoop).detach();
        g_watch = true;
    }
}

void Pass2InteractFrameBegin(long frame) {
    g_frameNo.store(frame, std::memory_order_relaxed);   // the repeat check's frame bound needs it watched or not
    if (!g_watch) return;
    Boundary(0);
    Summary();
}

void Pass2InteractBeforePass2() { if (g_watch) Boundary(1); }
void Pass2InteractAfterPass2()  { if (g_watch) Boundary(2); }

uint8_t* Pass2InteractAbmTarget(uint32_t* outLen) {
    uint8_t* o = g_abm.obj.load(std::memory_order_acquire);
    if (!o) return nullptr;
    if (outLen) *outLen = 8;
    return o + 0x28;
}
} // namespace ervr
