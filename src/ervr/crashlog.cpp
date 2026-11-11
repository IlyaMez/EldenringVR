#include "game_version.h"
#include "crashlog.h"
#include "frame_probe.h"
#include "log.h"
#include "rtti.h"
#include "game_er.h"
#include <windows.h>
#include <atomic>
#include <cstdio>

namespace ervr {
namespace {

PVOID g_veh = nullptr;
uint8_t* g_base = nullptr;

// Only codes that actually kill a process. The game raises plenty of first-chance C++ and
// SEH exceptions it handles itself; logging those would bury the one that matters.
bool Fatal(DWORD c) {
    switch (c) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        // int3 / __debugbreak: a runtime's assert. Players on Meta's runtime 1.208 ended with 0x80000003 inside
        // xrCreateSession and no line said whose (2026-11-07..11); a handled one is logged too, capped like the rest.
        case EXCEPTION_BREAKPOINT:
            return true;
        default:
            return false;
    }
}

const char* Name(DWORD c) {
    switch (c) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_OVERFLOW:          return "INT_OVERFLOW";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_BREAKPOINT:            return "BREAKPOINT (a runtime's assert; fatal unless something handles it)";
        default:                              return "?";
    }
}

// exe+0xRVA for anything inside the game module, module+0xRVA otherwise - the same shape the
// rest of the log uses, so a faulting address can be pasted straight into a disassembler.
int Sym(char* out, int cap, void* addr) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)addr, &m);
    if (!m) return snprintf(out, cap, " [%p]", addr);
    if ((uint8_t*)m == g_base) return snprintf(out, cap, " exe+0x%llx",
                                               (unsigned long long)((uint8_t*)addr - g_base));
    wchar_t pathW[MAX_PATH] = {};
    GetModuleFileNameW(m, pathW, MAX_PATH);
    const wchar_t* leafW = wcsrchr(pathW, L'\\');
    leafW = leafW ? leafW + 1 : pathW;
    char leaf[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, leafW, -1, leaf, sizeof(leaf) - 1, nullptr, nullptr);
    return snprintf(out, cap, " %s+0x%llx", leaf,
                    (unsigned long long)((uint8_t*)addr - (uint8_t*)m));
}

std::atomic<long> g_logged{0};
// Self-test plumbing. A vectored handler runs BEFORE any __except further up the stack, so
// a synthetic exception raised inside a __try proves the whole path - handler reached,
// address decoded, stack walked - and is then swallowed, leaving the game untouched.
std::atomic<long> g_selfTestSeen{0};
volatile bool     g_selfTesting = false;

// Reading a stack slot that is not committed must not fault us. Its own function so the
// __try is legal (no non-POD locals anywhere near it).
bool SafeRead64(const uint64_t* p, uint64_t* out) {
    __try { *out = *p; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The faulting thread's call chain by real x64 unwinding, every module named (the raw stack scan below only
// names the game's own frames, and a fault inside a VR runtime, ReShade or a driver needs the DLL's name).
// Its own function: no C++ objects near the __try.
int UnwindFrames(const CONTEXT* from, void** out, int cap) {
    int n = 0;
    __try {
        CONTEXT c = *from;
        for (; n < cap && c.Rip; ++n) {
            out[n] = (void*)c.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
            if (!fn) {   // leaf function: the return address is at rsp
                c.Rip = *(DWORD64*)c.Rsp;
                c.Rsp += 8;
            } else {
                void* handlerData = nullptr; DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &handlerData, &establisher, nullptr);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

// A user-space heap/stack pointer worth dereferencing?
bool PlausiblePtr(uint64_t v) {
    return v >= 0x10000 && v < 0x00007FFFFFFFFFFFull && (v & 7) == 0;
}

// DL_PANIC has a dedicated handler in main.cpp (it can SUPPRESS pass-2 asserts, which this
// one must not pre-empt or duplicate).
constexpr uintptr_t kRvaDlPanicNullWrite = 0x1eb9999;

// The transition null-call (2.6.2.0). Every fast-travel/death/spawn-window crash of
// 2026-08-22 was this ONE instruction sequence:
//     48 8b 01              mov  rax, [rcx]        <- faults: rcx == 0
//     48 8d 54 24 28        lea  rdx, [rsp+0x28]
//     ff 90 44 04 00 00     call qword ptr [rax+0x444]
// - a virtual call on an object that is null while the world is mid-transition (register
// dump 15:11:05: rcx=0, task id 0x90000052 in r14). The game's own code lacks the null
// check; the second engine pass widens the window until warps hit it near-always. Eating
// it = executing the `if (obj)` FromSoftware forgot: skip the 14 bytes, zero rax (the
// would-be return value), continue. Armed only after a byte-compare so a game update
// leaves it dormant instead of corrupting unknown code.
constexpr uintptr_t kRvaTransitionNullCall = 0xc7f88b;
// Site 2 of the same transition family (register dump 15:16:19: CS::WorldChrManImp + FD4Time,
// a poisoned chr pointer): movss xmm1, [rcx+0xb0]. Eating = skip the load, zero xmm1 - one
// garbage float in a dying chr's update instead of a dead process.
constexpr uintptr_t kRvaTransitionMovss = 0x3f9b2b;
constexpr uint8_t kMovssBytes[8] = {0xf3, 0x0f, 0x10, 0x89, 0xb0, 0x00, 0x00, 0x00};
// That movss is the first touch of the chr in a small per-chr update (2.7.2.0 0x3f9dd0, called
// for each node of a WorldChrMan chr list, task 0x90000052). Skipping only the load was not
// enough on 2.7.2.0: the pointer is not even canonical (14:39:34: a FieldInsHandle sat in the
// node), and the same function faulted again on [rcx+0x190] two instructions later. With this
// exact prologue in front of the site, the eat returns from the whole function instead - that
// character's update skipped, the list walk goes on to the next node.
constexpr size_t kMovssFnOffset = 0x1b;   // function start -> movss
constexpr uint8_t kMovssPrologue[kMovssFnOffset] = {
    0x48, 0x8b, 0xc4,                                   // mov rax, rsp
    0x57,                                               // push rdi
    0x48, 0x83, 0xec, 0x40,                             // sub rsp, 0x40
    0x48, 0xc7, 0x40, 0xd8, 0xfe, 0xff, 0xff, 0xff,     // mov qword [rax-0x28], -2
    0x48, 0x89, 0x58, 0x08,                             // mov [rax+8], rbx
    0x48, 0x89, 0x70, 0x10,                             // mov [rax+0x10], rsi
    0x48, 0x8b, 0xd9};                                  // mov rbx, rcx
constexpr uint8_t kNullCallBytes[14] = {0x48, 0x8b, 0x01, 0x48, 0x8d, 0x54, 0x24, 0x28,
                                        0xff, 0x90, 0x44, 0x04, 0x00, 0x00};
// 1.17 (2.7.1.0 and 2.7.2.0) calls [rax+0x448] at the same site; same 14 bytes long.
constexpr uint8_t kNullCallBytes117[14] = {0x48, 0x8b, 0x01, 0x48, 0x8d, 0x54, 0x24, 0x28,
                                           0xff, 0x90, 0x48, 0x04, 0x00, 0x00};
const uint8_t* NullCallBytes() { return IsGame117(CurrentGameBuild()) ? kNullCallBytes117 : kNullCallBytes; }
std::atomic<long> g_nullCallEats{0};

bool SafeRead64(const uint64_t* p, uint64_t* out);   // defined below

// Byte-verify a code site through the SEH-guarded read path. A raw memcmp on these pages
// returned a MISMATCH at fault time while SafeRead64 microseconds later saw the true bytes
// (16:52 diagnostic: cmp=1, rcx=0, logged rip bytes correct) - Arxan's code-page handling
// makes plain data reads of .text unreliable inside the dispatch window. The guarded
// two-qword read is the path that demonstrably works, so verification uses it.
bool CodeBytesMatch(const uint8_t* at, const uint8_t* expect, size_t n) {
    uint64_t q[2] = {};
    if (n > 16) return false;
    if (!SafeRead64((const uint64_t*)at, &q[0])) return false;
    if (n > 8 && !SafeRead64((const uint64_t*)at + 1, &q[1])) return false;
    return memcmp(q, expect, n) == 0;
}

LONG CALLBACK Veh(EXCEPTION_POINTERS* xp) {
    if (!xp || !xp->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = xp->ExceptionRecord->ExceptionCode;
    // Transition null-call fix (see kRvaTransitionNullCall above). Condition is exact:
    // this address, access violation, rcx null. Anything else falls through to reporting.
    // Byte-verified LAZILY, here: Arxan only decrypts .text regions on demand, so an
    // install-time memcmp read ciphertext and disarmed the fix (15:15 run). At the moment
    // of the fault the bytes ARE the executing code - compare then.
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        (uint8_t*)xp->ExceptionRecord->ExceptionAddress == g_base + GameRva(kRvaTransitionNullCall)) {
        // Every condition below was verifiably TRUE in the 16:43 crash dump, yet the eat
        // did not happen - so when it fails again, say exactly which check refused.
        const bool rcx0 = xp->ContextRecord->Rcx == 0;
        const int cmp = CodeBytesMatch(g_base + GameRva(kRvaTransitionNullCall), NullCallBytes(), sizeof(kNullCallBytes)) ? 0 : 1;
        if (!rcx0 || cmp != 0)
            LOGE("[crash] null-call site hit but NOT eaten: rcx=%llx cmp=%d base=%p addr=%p",
                 (unsigned long long)xp->ContextRecord->Rcx, cmp, (void*)g_base,
                 xp->ExceptionRecord->ExceptionAddress);
    }
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        (uint8_t*)xp->ExceptionRecord->ExceptionAddress == g_base + GameRva(kRvaTransitionNullCall) &&
        xp->ContextRecord->Rcx == 0 &&
        (CodeBytesMatch(g_base + GameRva(kRvaTransitionNullCall), NullCallBytes(), sizeof(kNullCallBytes)) ||
         GameERVersionOk())) {
        const long n = g_nullCallEats.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 32)
            LOGW("[crash] transition null-call EATEN (#%ld): the object is null mid-transition; "
                 "virtual call skipped (the null check the game forgot)", n);
        xp->ContextRecord->Rip += sizeof(kNullCallBytes);
        xp->ContextRecord->Rax = 0;   // the call's would-be result: null, the honest value
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        (uint8_t*)xp->ExceptionRecord->ExceptionAddress == g_base + GameRva(kRvaTransitionMovss) &&
        (CodeBytesMatch(g_base + GameRva(kRvaTransitionMovss), kMovssBytes, sizeof(kMovssBytes)) ||
         GameERVersionOk())) {
        const long n = g_nullCallEats.fetch_add(1, std::memory_order_relaxed) + 1;
        CONTEXT* c = xp->ContextRecord;
        const uint8_t* fn = g_base + GameRva(kRvaTransitionMovss) - kMovssFnOffset;
        // At the movss: rax still holds the entry rsp (return address), rsp is 0x48 below it,
        // rbx/rsi sit in the home slots, rdi was pushed. Undo exactly that and return.
        const uint64_t entry = c->Rsp + 0x48;
        uint64_t ret = 0, rdi = 0, rbx = 0, rsi = 0;
        if (c->Rax == entry && CodeBytesMatch(fn, kMovssPrologue, 16) &&
            CodeBytesMatch(fn + 16, kMovssPrologue + 16, kMovssFnOffset - 16) &&
            SafeRead64((const uint64_t*)entry, &ret) && SafeRead64((const uint64_t*)(entry - 8), &rdi) &&
            SafeRead64((const uint64_t*)(entry + 8), &rbx) && SafeRead64((const uint64_t*)(entry + 0x10), &rsi)) {
            if (n <= 32)
                LOGW("[crash] transition movss on poisoned chr pointer %llx EATEN (#%ld): that "
                     "character's update skipped", (unsigned long long)c->Rcx, n);
            c->Rip = ret; c->Rsp = entry + 8; c->Rdi = rdi; c->Rbx = rbx; c->Rsi = rsi; c->Rax = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (n <= 32)
            LOGW("[crash] transition movss on poisoned chr pointer EATEN (#%ld): load "
                 "skipped, xmm1 zeroed", n);
        c->Rip += sizeof(kMovssBytes);
        c->Xmm1.Low = 0; c->Xmm1.High = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (!Fatal(code)) return EXCEPTION_CONTINUE_SEARCH;
    if (g_selfTesting) {
        g_selfTestSeen.fetch_add(1, std::memory_order_relaxed);
        return EXCEPTION_CONTINUE_SEARCH;   // counted, not logged as a crash; __except eats it
    }
    if ((uint8_t*)xp->ExceptionRecord->ExceptionAddress == g_base + GameRva(kRvaDlPanicNullWrite))
        return EXCEPTION_CONTINUE_SEARCH;   // main.cpp's PanicVeh owns this one
    // A fault raised while we are already reporting one would recurse until the stack dies.
    static thread_local bool s_inside = false;
    if (s_inside) return EXCEPTION_CONTINUE_SEARCH;
    // Cap the whole session: a first-chance fault the game handles could otherwise repeat
    // every frame, and the interesting one is always among the first few.
    if (g_logged.fetch_add(1, std::memory_order_relaxed) >= 24) return EXCEPTION_CONTINUE_SEARCH;
    s_inside = true;

    void* at = xp->ExceptionRecord->ExceptionAddress;
    char sym[160] = {};
    Sym(sym, sizeof(sym), at);

    char detail[160] = {};
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
        xp->ExceptionRecord->NumberParameters >= 2) {
        const ULONG_PTR op = xp->ExceptionRecord->ExceptionInformation[0];
        snprintf(detail, sizeof(detail), " %s 0x%llx",
                 op == 0 ? "reading" : op == 1 ? "writing" : "executing",
                 (unsigned long long)xp->ExceptionRecord->ExceptionInformation[1]);
    }

    // WHICH PASS is the whole point: a fault in pass 2 is ours (the second engine pass
    // replaying over state pass 1 never saw), a fault in pass 1 is the game's own.
    const int pass = FramePumpCurrentPass();
    LOGE("[crash] %s (0x%08X) at%s%s | pass %d | thread %lu", Name(code), (unsigned)code, sym,
         detail, pass, GetCurrentThreadId());

    // Registers, with RTTI identification of whatever objects they still point at. The
    // faulting task's `this` (or its owner) is usually in one of them, and its class name
    // is the ANSWER this handler exists for: which engine subsystem's task read the null.
    {
        const CONTEXT* c = xp->ContextRecord;
        struct { const char* n; uint64_t v; } regs[] = {
            {"rax", c->Rax}, {"rbx", c->Rbx}, {"rcx", c->Rcx}, {"rdx", c->Rdx},
            {"rsi", c->Rsi}, {"rdi", c->Rdi}, {"rbp", c->Rbp}, {"r8", c->R8},
            {"r9", c->R9},   {"r10", c->R10}, {"r11", c->R11}, {"r12", c->R12},
            {"r13", c->R13}, {"r14", c->R14}, {"r15", c->R15},
        };
        char line[900];
        int p2 = snprintf(line, sizeof(line), "[crash] regs:");
        for (auto& r : regs) {
            const char* cls = nullptr;
            std::string clsStr;
            uint64_t vt = 0;
            if (PlausiblePtr(r.v) && SafeRead64((const uint64_t*)r.v, &vt) &&
                vt > (uint64_t)g_base && vt < (uint64_t)g_base + 0x4000000) {
                clsStr = RttiNameOfVTable((void**)vt);
                if (!clsStr.empty()) cls = clsStr.c_str();
            }
            p2 += snprintf(line + p2, sizeof(line) - p2, cls ? " %s=%llx<%s>" : " %s=%llx",
                           r.n, (unsigned long long)r.v, cls);
            if (p2 > (int)sizeof(line) - 120) break;
        }
        LOGE("%s", line);
        // The faulting instruction itself, so the null deref's shape (which register, what
        // offset) is readable without a disassembler session.
        uint64_t b0 = 0, b1 = 0;
        if (SafeRead64((const uint64_t*)at, &b0) && SafeRead64((const uint64_t*)at + 1, &b1))
            LOGE("[crash] code at rip: %016llx %016llx (little-endian)",
                 (unsigned long long)b0, (unsigned long long)b1);
    }

    // Walk the FAULTING context's stack. RtlCaptureStackBackTrace here would walk the
    // exception dispatcher's frames instead of the code that actually faulted (the same
    // reason main.cpp's PanicVeh scans by hand). A raw scan for return addresses into .text
    // is crude but names the caller chain reliably.
    auto* dos = (IMAGE_DOS_HEADER*)g_base;
    auto* nt = (IMAGE_NT_HEADERS*)(g_base + dos->e_lfanew);
    const uintptr_t lo = (uintptr_t)g_base + 0x1000;
    const uintptr_t hi = (uintptr_t)g_base + nt->OptionalHeader.SizeOfImage;
    const uint64_t* sp = (const uint64_t*)xp->ContextRecord->Rsp;
    char line[900];
    int p = snprintf(line, sizeof(line), "[crash] stack:");
    for (int i = 0; i < 256 && p < (int)sizeof(line) - 24; ++i) {
        uint64_t v = 0;
        if (!SafeRead64(sp + i, &v)) break;
        if (v >= lo && v < hi)
            p += snprintf(line + p, sizeof(line) - p, " exe+0x%llx",
                          (unsigned long long)(v - (uintptr_t)g_base));
    }
    LOGE("%s", line);
    {
        void* frames[20] = {};
        const int n = UnwindFrames(xp->ContextRecord, frames, 20);
        int q = snprintf(line, sizeof(line), "[crash] frames:");
        for (int i = 0; i < n && q < (int)sizeof(line) - 80; ++i) q += Sym(line + q, (int)sizeof(line) - q, frames[i]);
        LOGE("%s", line);
    }

    s_inside = false;
    return EXCEPTION_CONTINUE_SEARCH;   // log only; never change what the game does
}

}  // namespace

void CrashLogInstall() {
    if (g_veh) return;
    g_base = (uint8_t*)GetModuleHandleW(nullptr);
    // Registered LAST (0): every handler that wants to deal with an exception legitimately -
    // the game's own SEH, our hardware-breakpoint VEH in cam_watch - gets it first.
    g_veh = AddVectoredExceptionHandler(0, Veh);
    LOGI("[crash] fatal-exception reporter installed (log only)");
    LOGI("[crash] transition null-call fix armed (bytes verified lazily at fault time)");
}

// Raises a synthetic fault and checks the handler saw it. Costs one exception at startup and
// turns "the reporter is installed" into "the reporter demonstrably fires", which matters
// because the thing it exists to catch takes a headset session to reproduce.
void CrashLogSelfTest() {
    if (!g_veh) { LOGE("[crash] self-test SKIPPED - no handler installed"); return; }
    const long before = g_selfTestSeen.load(std::memory_order_relaxed);
    g_selfTesting = true;
    __try {
        RaiseException(EXCEPTION_INT_OVERFLOW, 0, 0, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_selfTesting = false;
    const long after = g_selfTestSeen.load(std::memory_order_relaxed);
    if (after > before) LOGI("[crash] self-test PASSED - handler sees fatal exceptions");
    else                LOGE("[crash] self-test FAILED - fatal exceptions are NOT being seen; "
                             "a warp crash will not be reported");
}

void CrashLogShutdown() {
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
}

}  // namespace ervr
