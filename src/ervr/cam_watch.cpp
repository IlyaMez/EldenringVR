#include "cam_watch.h"
#include "game_er.h"
#include "config.h"
#include "log.h"
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
};
constexpr int kMaxRec = 65536;
Rec  g_rec[kMaxRec];
std::atomic<long> g_recN{0};
std::atomic<bool> g_active{false};

uintptr_t g_lo = 0, g_hi = 0;   // watched byte range (matrix .. fov)
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
    struct Agg { uint64_t rip; long count; long writes; uint64_t offMask; int sample; };
    constexpr int kMaxAgg = 96;
    Agg agg[kMaxAgg]; int na = 0;
    for (long i = 0; i < n; ++i) {
        const Rec& r = g_rec[i];
        int k = 0;
        for (; k < na; ++k) if (agg[k].rip == r.rip) break;
        if (k == na) {
            if (na == kMaxAgg) continue;
            agg[na++] = {r.rip, 0, 0, 0, (int)i};
        }
        agg[k].count++;
        if (r.write) agg[k].writes++;
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
        LOGI("[camwatch] %-22s x%-5ld %s rows(16B):%03llx tid %lu | %s", mod, agg[i].count,
             agg[i].writes ? (agg[i].writes == agg[i].count ? "WRITE" : "MIXED") : "read ",
             (unsigned long long)agg[i].offMask, (unsigned long)s.tid, bytes);
        LOGI("[camwatch]   regs: %s", regs);
    }
    LOGI("[camwatch] ---- census complete (%d unique rips) ----", na);
}

std::atomic<bool> g_started{false};

DWORD WINAPI WatchThread(LPVOID) {
    // Wait for the world; the camera object only exists in-game.
    for (int w = 0; w < 600 && !GameERPlayerActive(); ++w) Sleep(1000);
    if (!GameERPlayerActive()) { LOGW("[camwatch] player never became active"); return 0; }
    Sleep((DWORD)(Cfg().camWatchDelay * 1000.f));

    uint32_t len = 0;
    uint8_t* tgt = GameERCamWatchTarget(&len);
    if (!tgt || !len) { LOGW("[camwatch] no camera object to watch"); return 0; }
    g_lo = (uintptr_t)tgt;
    g_hi = g_lo + len;
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
    if (!Cfg().camWatch) return;
    if (g_started.exchange(true)) return;
    if (HANDLE h = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr)) CloseHandle(h);
}
}
