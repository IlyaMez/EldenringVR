#include "cam_watch.h"
#include "game_er.h"
#include "frame_probe.h"
#include "config.h"
#include "log.h"
#include "pass2_interact.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace ervr {
namespace {

// One recorded access. Kept POD and small: the handler runs on the faulting thread inside
// exception dispatch, so it must do nothing but copy registers into a slot.
struct Rec {
    uint64_t rip = 0;
    uint64_t addr = 0;
    uint64_t regs[16];     // rax rcx rdx rbx rsp rbp rsi rdi r8..r15
    uint8_t  bytes[16];    // instruction bytes at rip, decoded offline
    uint32_t tid = 0;
    uint8_t  write = 0;    // ExceptionInformation[0]: 0 read, 1 write
    uint8_t  pass = 1;     // the frame pump's pass at the access
    int32_t  group = -2;   // the task runner's tag at the access (FramePumpCurrentTaskGroup)
    uint64_t ret[6];       // return addresses into the exe found up the stack (callers, innermost first)
    uint64_t callee[6];    // what the call before each of them targeted (direct calls), else 0
};
constexpr int kMaxRec = 65536;
Rec  g_rec[kMaxRec];
std::atomic<long> g_recN{0};
std::atomic<bool> g_active{false};

uintptr_t g_lo = 0, g_hi = 0;   // watched byte range (matrix .. fov)
uintptr_t g_textLo = 0, g_textHi = 0;   // the exe's executable range, for the stack scan

// Return addresses into the exe up the faulting thread's stack, with the target of the direct call
// that pushed each one: the first is the accessing function's caller, its callee the function's entry.
// Called inside the exception handler on the faulting thread's own stack, so plain reads.
void ScanStack(uint64_t rsp, uint64_t* ret, uint64_t* callee) {
    int n = 0;
    const uint64_t top = (uint64_t)((NT_TIB*)NtCurrentTeb())->StackBase;
    for (uint64_t p = rsp; p + 8 <= top && p < rsp + 0x1000 && n < 6; p += 8) {
        const uint64_t v = *(const uint64_t*)p;
        if (v < g_textLo + 8 || v >= g_textHi) continue;
        const uint8_t* c = (const uint8_t*)v;
        uint64_t target = 0;
        if (c[-5] == 0xE8) target = v + *(const int32_t*)(c - 4);
        else if (!(c[-6] == 0xFF && c[-5] == 0x15) && !(c[-2] == 0xFF && (c[-1] & 0xF8) == 0xD0) &&
                 !(c[-3] == 0xFF && (c[-2] & 0x38) == 0x10)) continue;   // not after a call
        ret[n] = v; callee[n] = target; ++n;
    }
    for (; n < 6; ++n) ret[n] = callee[n] = 0;
}
uintptr_t g_pages[2] = {};
int       g_pageCount = 0;
DWORD     g_baseProt = PAGE_READWRITE;
void*     g_veh = nullptr;
thread_local bool t_rearm = false;

void ArmGuard() {
    for (int i = 0; i < g_pageCount; ++i) {
        DWORD o;
        VirtualProtect((void*)g_pages[i], 0x1000, g_baseProt | PAGE_GUARD, &o);
    }
}

LONG CALLBACK Veh(EXCEPTION_POINTERS* xp) {
    const DWORD code = xp->ExceptionRecord->ExceptionCode;
    if (code == STATUS_GUARD_PAGE_VIOLATION) {
        const uintptr_t a = (uintptr_t)xp->ExceptionRecord->ExceptionInformation[1];
        bool ours = false;
        for (int i = 0; i < g_pageCount; ++i)
            if ((a & ~0xFFFull) == g_pages[i]) ours = true;
        // Guard faults on any OTHER page (thread stack growth!) must go to the OS untouched.
        if (!ours) return EXCEPTION_CONTINUE_SEARCH;
        if (g_active.load(std::memory_order_relaxed)) {
            if (a >= g_lo && a < g_hi) {
                const long n = g_recN.fetch_add(1, std::memory_order_relaxed);
                if (n < kMaxRec) {
                    Rec& r = g_rec[n];
                    CONTEXT* c = xp->ContextRecord;
                    r.rip = c->Rip; r.addr = a; r.tid = GetCurrentThreadId();
                    r.write = (uint8_t)xp->ExceptionRecord->ExceptionInformation[0];
                    r.pass = (uint8_t)FramePumpCurrentPass();
                    r.group = FramePumpCurrentTaskGroup();
                    ScanStack(c->Rsp, r.ret, r.callee);
                    uint64_t* q = r.regs;
                    q[0] = c->Rax; q[1] = c->Rcx; q[2] = c->Rdx; q[3] = c->Rbx;
                    q[4] = c->Rsp; q[5] = c->Rbp; q[6] = c->Rsi; q[7] = c->Rdi;
                    q[8] = c->R8;  q[9] = c->R9;  q[10] = c->R10; q[11] = c->R11;
                    q[12] = c->R12; q[13] = c->R13; q[14] = c->R14; q[15] = c->R15;
                    // Never read across a page end - the next code page can be a guard too.
                    const size_t room = 0x1000 - (c->Rip & 0xFFF);
                    memcpy(r.bytes, (void*)c->Rip, room < 16 ? room : 16);
                }
            }
            // The guard bit auto-cleared; run the instruction, then STATUS_SINGLE_STEP re-arms.
            xp->ContextRecord->EFlags |= 0x100;
            t_rearm = true;
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == STATUS_SINGLE_STEP && t_rearm) {
        t_rearm = false;
        if (g_active.load(std::memory_order_relaxed)) ArmGuard();
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

const char* kRegName[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                            "r8","r9","r10","r11","r12","r13","r14","r15"};

void Dump() {
    const long total = g_recN.load();
    const long n = total < kMaxRec ? total : kMaxRec;
    auto* exe = (uint8_t*)GetModuleHandleW(nullptr);
    LOGI("[camwatch] ---- %ld accesses recorded (%ld kept) on %p..%p ----",
         total, n, (void*)g_lo, (void*)g_hi);

    // Aggregate by rip.
    struct Agg { uint64_t rip; long count; long writes; uint64_t offMask; int sample; long pass2; int32_t groups[4]; int ng; };
    constexpr int kMaxAgg = 96;
    Agg agg[kMaxAgg]; int na = 0;
    for (long i = 0; i < n; ++i) {
        const Rec& r = g_rec[i];
        int k = 0;
        for (; k < na; ++k) if (agg[k].rip == r.rip) break;
        if (k == na) {
            if (na == kMaxAgg) continue;
            agg[na++] = {r.rip, 0, 0, 0, (int)i, 0, {}, 0};
        }
        agg[k].count++;
        if (r.write) agg[k].writes++;
        if (r.pass == 2) agg[k].pass2++;
        bool seen = false;
        for (int g = 0; g < agg[k].ng; ++g) seen |= agg[k].groups[g] == r.group;
        if (!seen && agg[k].ng < 4) agg[k].groups[agg[k].ng++] = r.group;
        const uint64_t off = (r.addr - g_lo) / 16;   // which 16-byte row of the block
        if (off < 64) agg[k].offMask |= 1ull << off;
    }
    // Highest count first: the per-frame readers stand out from the once-only ones.
    for (int i = 0; i < na; ++i)
        for (int j = i + 1; j < na; ++j)
            if (agg[j].count > agg[i].count) { Agg t = agg[i]; agg[i] = agg[j]; agg[j] = t; }

    for (int i = 0; i < na; ++i) {
        const Rec& s = g_rec[agg[i].sample];
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)agg[i].rip, &m);
        char mod[64] = "?";
        if (m == (HMODULE)exe) snprintf(mod, sizeof(mod), "exe+0x%llx",
                                        (unsigned long long)(agg[i].rip - (uint64_t)(uintptr_t)exe));
        else if (m) {
            wchar_t path[MAX_PATH]; GetModuleFileNameW(m, path, MAX_PATH);
            const wchar_t* base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
            snprintf(mod, sizeof(mod), "%S+0x%llx", base,
                     (unsigned long long)(agg[i].rip - (uint64_t)(uintptr_t)m));
        }
        char bytes[3 * 16 + 1] = {};
        for (int b = 0; b < 16; ++b) snprintf(bytes + b * 3, 4, "%02X ", s.bytes[b]);
        char regs[16 * 24] = {}; int p = 0;
        for (int rg = 0; rg < 16; ++rg)
            p += snprintf(regs + p, sizeof(regs) - p, "%s=%llx ", kRegName[rg],
                          (unsigned long long)s.regs[rg]);
        char groups[64] = {}; int gp = 0;
        for (int g = 0; g < agg[i].ng; ++g) {
            const int32_t v = agg[i].groups[g];
            gp += snprintf(groups + gp, sizeof(groups) - gp, "%s%d", g ? "," : "", v >= 0 ? (v & 0xFFFF) : v);
        }
        LOGI("[camwatch] %-22s x%-5ld %s rows(16B):%03llx tid %lu, %ld in pass 2, runner tag(s) %s, first at +0x%llx | %s", mod,
             agg[i].count, agg[i].writes ? (agg[i].writes == agg[i].count ? "WRITE" : "MIXED") : "read ",
             (unsigned long long)agg[i].offMask, (unsigned long)s.tid, agg[i].pass2, groups,
             (unsigned long long)(s.addr - g_lo), bytes);
        LOGI("[camwatch]   regs: %s", regs);
        char chain[6 * 48] = {}; int cp = 0;
        for (int k = 0; k < 6 && s.ret[k]; ++k) {
            cp += snprintf(chain + cp, sizeof(chain) - cp, " exe+0x%llx",
                           (unsigned long long)(s.ret[k] - (uint64_t)(uintptr_t)exe));
            if (s.callee[k])
                cp += snprintf(chain + cp, sizeof(chain) - cp, "(called exe+0x%llx)",
                               (unsigned long long)(s.callee[k] - (uint64_t)(uintptr_t)exe));
        }
        if (cp) LOGI("[camwatch]   returns to:%s", chain);
    }
    LOGI("[camwatch] ---- census complete (%d unique rips) ----", na);
}

std::atomic<bool> g_started{false};

DWORD WINAPI WatchThread(LPVOID) {
    // Wait for the world; the camera object only exists in-game.
    for (int w = 0; w < 600 && !GameERPlayerActive(); ++w) Sleep(1000);
    if (!GameERPlayerActive()) { LOGW("[camwatch] player never became active"); return 0; }

    uint32_t len = 0;
    uint8_t* tgt = nullptr;
    if (Cfg().camWatchAbm) {
        // Watch the action-button manager's flag bytes: which code sets and clears them, in which pass.
        for (int w = 0; w < 600 && !(tgt = Pass2InteractAbmTarget(&len)); ++w) Sleep(1000);
        if (!tgt) { LOGW("[camwatch] action-button manager never found"); return 0; }
        Sleep((DWORD)(Cfg().camWatchDelay * 1000.f));
    } else if (Cfg().camWatchDst) {
        // Watch the task-runner's staged frame-state block: which fields does the dispatch
        // read PER PASS (x2/frame when pumped) - the dt staging the passes consume.
        for (int w = 0; w < 600 && !(tgt = FramePumpStepsDstWatchTarget(&len)); ++w) Sleep(1000);
        if (!tgt) { LOGW("[camwatch] staged block never captured (pump never ran?)"); return 0; }
    } else if (Cfg().camWatchIgt) {
        // Watch the IGT counter's WRITER: names the code that advances game time and, via
        // the register dump, where its increment comes from. Wait for the pump so the
        // census covers pumped frames (2 writes/frame vs 1 = per-pass evidence).
        for (int w = 0; w < 600 && !FramePumpDtWatchTarget(&len); ++w) Sleep(1000);
        tgt = GameERIgtWatchTarget(&len);
        if (!tgt) { LOGW("[camwatch] no GameDataMan for the IGT watch"); return 0; }
    } else if (Cfg().camWatchDt) {
        // Watch the frame-timing dt block instead of the camera. Its address only exists
        // once the pump has run a full-frame pass 2 (the hw-bp capture), so wait for it.
        for (int w = 0; w < 600 && !(tgt = FramePumpDtWatchTarget(&len)); ++w) Sleep(1000);
        if (!tgt) { LOGW("[camwatch] dt struct never captured (pump never ran?)"); return 0; }
    } else {
        Sleep((DWORD)(Cfg().camWatchDelay * 1000.f));
        tgt = GameERCamWatchTarget(&len);
        if (!tgt || !len) { LOGW("[camwatch] no camera object to watch"); return 0; }
    }
    g_lo = (uintptr_t)tgt;
    g_hi = g_lo + len;
    {
        auto* base = (uint8_t*)GetModuleHandleW(nullptr);
        auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        auto* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const uintptr_t a = (uintptr_t)base + sec->VirtualAddress, b = a + sec->Misc.VirtualSize;
            if (!g_textLo || a < g_textLo) g_textLo = a;
            if (b > g_textHi) g_textHi = b;
        }
    }
    g_pages[0] = g_lo & ~0xFFFull;
    g_pageCount = 1;
    if (((g_hi - 1) & ~0xFFFull) != g_pages[0]) { g_pages[1] = g_pages[0] + 0x1000; g_pageCount = 2; }
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((void*)g_pages[0], &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
        LOGW("[camwatch] camera page not committed?");
        return 0;
    }
    g_baseProt = mbi.Protect & ~(DWORD)PAGE_GUARD;

    g_recN.store(0);
    g_veh = AddVectoredExceptionHandler(1, Veh);
    g_active.store(true);
    ArmGuard();
    LOGI("[camwatch] armed on %p..%p (%d page(s), prot 0x%x) for %.1f s",
         (void*)g_lo, (void*)g_hi, g_pageCount, (unsigned)g_baseProt, Cfg().camWatchSeconds);

    const ULONGLONG until = GetTickCount64() + (ULONGLONG)(Cfg().camWatchSeconds * 1000.f);
    while (GetTickCount64() < until && g_recN.load() < kMaxRec) Sleep(50);

    g_active.store(false);
    Sleep(150);   // drain in-flight faults before tearing down
    for (int i = 0; i < g_pageCount; ++i) {
        DWORD o;
        VirtualProtect((void*)g_pages[i], 0x1000, g_baseProt, &o);
    }
    if (g_veh) RemoveVectoredExceptionHandler(g_veh);
    Dump();
    return 0;
}

} // namespace

void CamWatchTick() {
    if (!Cfg().camWatch && !Cfg().camWatchDt && !Cfg().camWatchIgt && !Cfg().camWatchDst && !Cfg().camWatchAbm) return;
    if (g_started.exchange(true)) return;
    if (HANDLE h = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr)) CloseHandle(h);
}
}
