#include "patches_er.h"
#include "game_memory.h"
#include "game_res.h"
#include "config.h"
#include "log.h"
#include <windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ervr {
namespace {

// SafeWrite is a plain guarded memcpy and .text is read-execute, so patches need their own
// protection dance. Restore the old protection and flush the icache like any code patcher.
bool PatchWrite(uint8_t* at, const void* bytes, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

// Patterns from uberhalit/EldenRingFpsUnlockAndMore GameData.cs (MIT, (c) uberhalit).
//
// FRAMELOCK: `mov [reg+disp], 0x3C888889` - the 1/60 the game stores as its frame-time
// target every frame: the frame LIMITER. It does not set game speed: the tasks consume the
// MEASURED frame time (the frame-dt global, see frame_probe.cpp), so the sim keeps real time
// at any achieved rate - measured 2026-10-31, in-game clock x0.98 of wall time at 52 pairs/s
// under a 1/72 tick (a fixed step would have read x0.68). Below the target the cost is
// frame rate, not slow motion.
constexpr const char* kPatFramelock = "C7 ?? ?? ?? 88 88 3C EB";
constexpr int         kOffFramelock = 3;   // the imm32 float (its low byte varies: 88/89/90)
constexpr const char* kPatFramelockFuzzy = "89 73 ?? C7 ?? ?? ?? ?? ?? ?? EB ?? 89 73";
constexpr int         kOffFramelockFuzzy = 6;

// HERTZLOCK: on every resolution change the game writes "60 Hz" + a change flag, clamping
// exclusive-fullscreen modes back to 60. uberhalit's patch zeroes both immediates.
constexpr const char* kPatHertzlock = "EB ?? C7 ?? ?? 3C 00 00 00 C7 ?? ?? 01 00 00 00";
constexpr int         kOffHertzBase = 2;    // the first mov
constexpr int         kOffHertzInt1 = 3;    // +base: the 0x3C (60) immediate
constexpr int         kOffHertzInt2 = 10;   // +base: the 1 (change flag) immediate

// Framelock imm resolve, shared by the fps unlock and the full-frame pass-2 dt swap.
uint8_t* ResolveFrameDtImm() {
    static uint8_t* s_at = nullptr;
    static bool s_tried = false;
    if (!s_tried) {
        s_tried = true;
        uint8_t* fl = AobScan(kPatFramelock);
        int off = kOffFramelock;
        if (!fl) { fl = AobScan(kPatFramelockFuzzy); off = kOffFramelockFuzzy; }
        if (fl) s_at = fl + off;
    }
    return s_at;
}

// ---- [VR] GameRes ------------------------------------------------------------------
//
// ELDEN RING reads its WINDOWED resolution straight out of its own settings file, and it
// accepts sizes no monitor could display - R.E.A.L. VR ships a square 2736x2736 preset and
// simply copies it over GraphicsConfig.xml (RealConfig.bat:485). So the resolution is a
// FILE EDIT, not a patch.
//
// The file alone is not enough, though: the game VALIDATES what it reads against its own
// resolution table and snaps to the nearest listed size (measured 2026-08-21: XML 1496x1360
// came up as a 1440x810 swapchain). So the target must also exist in that table - which is
// why the entry below is patched too, into the LAST slot (3840x2160). That slot is already
// past the point where the menu stops listing (it stops at the first entry too big for the
// screen), so the visible list is left exactly as the player expects.
//
// What is deliberately NOT done any more: faking the screen size to make the menu offer the
// entry. The game re-reads SM_CXSCREEN/SM_CYSCREEN EVERY FRAME, so lying about it skewed the
// live projection as the head moved, and the menu's size check ignored those APIs anyway.
// The entry is written over the stock 1920x1080 one, which is where the game still reaches
// it: the table is only ever walked until the first entry TOO BIG FOR THE SCREEN, so a slot
// past 3200x1800 is dead ground (measured 2026-08-21 - a target parked in the last slot was
// ignored and the request snapped down to 2048x1152). The corollary is the hard limit this
// whole feature lives under: the game will not render larger than the desktop, in the menu
// or from the file, so the target below is always fitted to the screen first.
constexpr const char* kPatResTable = "80 07 00 00 38 04 00 00 00 08 00 00 80 04 00 00";
constexpr uint32_t kSlotW = 1920, kSlotH = 1080;

// 16:9 ENFORCEMENT. The game renders the whole window but masks the result back to 16:9 with
// black bars - the scene behind them is rendered either way, which is why the ultrawide
// community's fix is a single jump patch rather than a projection rewrite. For VR that mask
// is fatal twice: it throws away the pixels we asked for, AND it leaves the frame's real
// frustum disagreeing with the frame's dimensions, so the eye crop (which is computed from
// width/height) lands on the wrong rectangle and the image shears as the head turns.
// Measured 2026-08-21 at 1656x1440: black bars top and bottom, plus the skew.
// Pattern by game version: the pre-1.12 one first, then uberhalit PR #147's, which is the
// only one that matches 1.16.2 (rva 0x19ebbee).
constexpr const char* kPatAspectOld = "8B ?? 85 ?? 74 ?? 44 8B ?? ?? 45 85 ?? 74 ?? 41 8B";
constexpr int         kOffAspectOld = 4;
constexpr const char* kPatAspectNew = "74 4F 45 8B 94 CC";
constexpr int         kOffAspectNew = 0;

std::atomic<bool> g_letterboxOff{false};   // PatchAspectClamp applied: the frame fills the window

bool PatchAspectClamp() {
    static int s_state = -1;   // -1 untried, 0 unavailable, 1 patched
    if (s_state >= 0) return s_state == 1;
    uint8_t* je = nullptr;
    if (uint8_t* a = AobScan(kPatAspectOld)) je = a + kOffAspectOld;
    else if (uint8_t* b = AobScan(kPatAspectNew)) je = b + kOffAspectNew;
    if (!je || *je != 0x74) {
        s_state = 0;
        LOGW("[patch] 16:9 letterbox patch not found; a non-16:9 GameRes will show black bars "
             "and the eye crop will not match the image (set [VR] GameRes to a 16:9 size)");
        return false;
    }
    const uint8_t jmp = 0xEB;
    s_state = PatchWrite(je, &jmp, 1) ? 1 : 0;
    g_letterboxOff.store(s_state == 1);
    if (s_state)
        LOGI("[patch] 16:9 letterbox removed at rva 0x%llx: the frame now fills the window, so "
             "its frustum matches its dimensions and the eye crop is exact",
             (unsigned long long)(je - (uint8_t*)GetModuleHandleW(nullptr)));
    else
        LOGW("[patch] 16:9 letterbox patch write failed");
    return s_state == 1;
}

// Every patched slot, at file scope so the GameResScan census below can watch them.
std::vector<uint8_t*> g_resSlots;
// The table's LAST entry (stock 3840x2160, four slots past 1920x1080; 12 entries from 800x450,
// dumped from 2.7.2.0). Which entry the settings-apply picks is ORDER-dependent: the last one,
// in table order, that fits inside the request. Every measured snap fits that rule and no
// other - 3712x2088 asked with the target in the 1920x1080 slot came up 3200x1800 (2026-10-31),
// 4224x2376 -> 3840x2160 (08-23), 1496x1360 -> 1440x810 (08-21). So on the oversize path,
// where the patched size check lets the walk reach the end, the target must ALSO sit in the
// last slot or a stock entry after the 1920 slot outbids it.
constexpr uint32_t kLastSlotOff = 4 * 8, kLastSlotW = 3840, kLastSlotH = 2160;
std::vector<uint8_t*> g_resLastSlots;
// The last slot as written at DLL attach (EarlyLastSlotWrite), so the worker's scan accepts it.
uint8_t* g_earlyLastSlot = nullptr;

// DllMain-safe (kernel32-free, POD only, SEH): find the table in the game's initialized
// .data and write the target into its LAST slot. Attach time because the worker loses the
// race: the boot-time settings apply walks the table ~0.5 s in, the worker's patch landed
// ~0.1 s after that (2026-10-31: apply's monitor-rect queries at :53.498, worker table patch
// at :53.608 - the walk saw only stock sizes and 3712x2088 came up 3200x1800). Only the last
// slot: should the size-check NOP ever be missing, the walk stops at the first stock entry
// too big for the desktop and never reaches it - today's fallback, unchanged.
uint8_t* EarlyLastSlotWrite(uint32_t w, uint32_t h) {
    __try {
        auto* base = (uint8_t*)GetModuleHandleW(nullptr);
        auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
            if (memcmp(s->Name, ".data\0\0\0", 8) != 0) continue;
            uint8_t* p = base + s->VirtualAddress;
            const size_t len = s->SizeOfRawData < s->Misc.VirtualSize ? s->SizeOfRawData : s->Misc.VirtualSize;
            for (size_t o = 0; o + kLastSlotOff + 8 <= len; o += 4) {
                const uint32_t* q = (const uint32_t*)(p + o);
                if (q[0] != kSlotW || q[1] != kSlotH || q[2] != 2048 || q[3] != 1152) continue;
                uint32_t* last = (uint32_t*)(p + o + kLastSlotOff);
                if (last[0] != kLastSlotW || last[1] != kLastSlotH) return nullptr;
                last[0] = w;
                last[1] = h;
                return (uint8_t*)last;
            }
            return nullptr;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

// Put `w`x`h` into the table so the game accepts it from GraphicsConfig.xml; `lastSlot` also
// writes the final entry (oversize path only - see g_resLastSlots).
bool PatchResTable(uint32_t w, uint32_t h, bool lastSlot) {
    static bool s_scanned = false;
    if (!s_scanned) {
        s_scanned = true;
        for (uint8_t* slot : AobScanData(kPatResTable)) {
            uint32_t sw = 0, sh = 0;
            // Bit-exact check: if the slot does not read as the stock 1920x1080 the layout is
            // not what we think it is, and writing blind would corrupt a real entry.
            if (SafeRead(slot, &sw, 4) && SafeRead(slot + 4, &sh, 4) && sw == kSlotW && sh == kSlotH) {
                g_resSlots.push_back(slot);
                uint8_t* last = slot + kLastSlotOff;
                if (last == g_earlyLastSlot ||
                    (SafeRead(last, &sw, 4) && SafeRead(last + 4, &sh, 4) && sw == kLastSlotW && sh == kLastSlotH))
                    g_resLastSlots.push_back(last);
            }
        }
        if (g_resSlots.empty())
            LOGW("[patch] resolution table not found; the game will snap GameRes to its nearest "
                 "built-in size");
        else
            LOGD("[patch] resolution table: %zu copy(ies) of the 1920x1080 slot, %zu of the last slot",
                 g_resSlots.size(), g_resLastSlots.size());
    }
    const uint32_t wh[2] = {w, h};
    int ok = 0;
    for (uint8_t* slot : g_resSlots) if (PatchWrite(slot, wh, sizeof(wh))) ++ok;
    if (lastSlot) {
        if (g_resLastSlots.empty())
            LOGW("[patch] the resolution table's last slot is not the stock 3840x2160; an oversize "
                 "GameRes may snap to the largest stock entry that fits inside it");
        for (uint8_t* slot : g_resLastSlots) PatchWrite(slot, wh, sizeof(wh));
    }
    return ok > 0;
}

// ---- desktop-size lie, ARMED ONLY UNTIL THE OVERSIZED SWAPCHAIN LANDS ----------------
//
// The late settings-apply consults the monitor rect / current display mode as well as the
// (NOPed) table walker - REMOVING these lies outright killed the resize (measured
// 2026-08-22 19:38: boot stayed 2560x1440 for the whole run), so they are load-bearing
// for acceptance after all. But the game ALSO polls the same display metrics during play,
// and a standing HEIGHT lie (1440 reported as 3280) is this project's known "distorts/
// skews when moving my head" mistake - the user re-reported exactly that, vertically, the
// day the oversize path re-armed it (the width lie never engages on this monitor, which
// is why the warp was vertical-only). So the lies live only for the window that needs
// them: armed when the oversize target is resolved, DISARMED by PatchesNoteBackbufferSize
// the moment the oversized swapchain actually appears. Only the max-track pair stays
// adjusted for the session (window management, not display metrics).
std::atomic<uint32_t> g_fakeDeskW{0}, g_fakeDeskH{0};
std::atomic<bool>     g_displayLiesOff{false};   // set once the oversize swapchain landed

// The game's own code, or DXGI answering on its behalf. Arxan-protected code can run from
// outside the image, so an unknown caller is treated as the game rather than missed.
bool CallerIsGameOrDxgi(void* ret) {
    static uintptr_t s_lo = 0, s_hi = 0;
    if (!s_lo) {
        auto* base = (uint8_t*)GetModuleHandleW(nullptr);
        auto* dos = (IMAGE_DOS_HEADER*)base;
        auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        s_lo = (uintptr_t)base;
        s_hi = s_lo + nt->OptionalHeader.SizeOfImage;
    }
    const uintptr_t r = (uintptr_t)ret;
    if (r >= s_lo && r < s_hi) return true;
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)ret, &mod) || !mod)
        return false;
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(mod, path, MAX_PATH)) return false;
    const wchar_t* b = wcsrchr(path, L'\\');
    return _wcsicmp(b ? b + 1 : path, L"dxgi.dll") == 0;
}

BOOL(WINAPI* g_origGetMonitorInfoW)(HMONITOR, LPMONITORINFO) = nullptr;
BOOL(WINAPI* g_origGetMonitorInfoA)(HMONITOR, LPMONITORINFO) = nullptr;

void InflateMonitorInfo(LPMONITORINFO mi, void* ret) {
    if (g_displayLiesOff.load(std::memory_order_relaxed)) return;
    const uint32_t fw = g_fakeDeskW.load(std::memory_order_relaxed);
    const uint32_t fh = g_fakeDeskH.load(std::memory_order_relaxed);
    if (!fw || !mi || !CallerIsGameOrDxgi(ret)) return;
    auto grow = [&](RECT& r) {
        if (r.right - r.left < (LONG)fw) r.right = r.left + (LONG)fw;
        if (r.bottom - r.top < (LONG)fh) r.bottom = r.top + (LONG)fh;
    };
    grow(mi->rcMonitor);
    grow(mi->rcWork);
    static std::atomic<int> s_n{0};
    if (s_n.fetch_add(1, std::memory_order_relaxed) < 4)
        LOGI("[patch] monitor rect reported as %ux%u to the resolution check", fw, fh);
}

BOOL WINAPI HookGetMonitorInfoW(HMONITOR m, LPMONITORINFO mi) {
    BOOL ok = g_origGetMonitorInfoW(m, mi);
    if (ok) InflateMonitorInfo(mi, _ReturnAddress());
    return ok;
}
BOOL WINAPI HookGetMonitorInfoA(HMONITOR m, LPMONITORINFO mi) {
    BOOL ok = g_origGetMonitorInfoA(m, mi);
    if (ok) InflateMonitorInfo(mi, _ReturnAddress());
    return ok;
}

// The other place a desktop size comes from. ENUM_CURRENT_SETTINGS is the "how big is the
// screen right now" query; the numbered modes are the display's supported mode list.
BOOL(WINAPI* g_origEnumDisplaySettingsW)(LPCWSTR, DWORD, DEVMODEW*) = nullptr;
BOOL(WINAPI* g_origEnumDisplaySettingsExW)(LPCWSTR, DWORD, DEVMODEW*, DWORD) = nullptr;

void HandleEnumDisplaySettings(DWORD mode, DEVMODEW* dm, void* ret) {
    if (g_displayLiesOff.load(std::memory_order_relaxed)) return;
    const uint32_t fw = g_fakeDeskW.load(std::memory_order_relaxed);
    const uint32_t fh = g_fakeDeskH.load(std::memory_order_relaxed);
    if (!fw || !dm) return;
    if (!CallerIsGameOrDxgi(ret) || mode != ENUM_CURRENT_SETTINGS) return;
    if (dm->dmPelsWidth < fw) dm->dmPelsWidth = fw;
    if (dm->dmPelsHeight < fh) dm->dmPelsHeight = fh;
}

BOOL WINAPI HookEnumDisplaySettingsW(LPCWSTR dev, DWORD mode, DEVMODEW* dm) {
    BOOL ok = g_origEnumDisplaySettingsW(dev, mode, dm);
    if (ok) HandleEnumDisplaySettings(mode, dm, _ReturnAddress());
    return ok;
}
BOOL WINAPI HookEnumDisplaySettingsExW(LPCWSTR dev, DWORD mode, DEVMODEW* dm, DWORD flags) {
    BOOL ok = g_origEnumDisplaySettingsExW(dev, mode, dm, flags);
    if (ok) HandleEnumDisplaySettings(mode, dm, _ReturnAddress());
    return ok;
}

// ONLY the max-window-track metrics. Windows clamps a window to SM_CXMAXTRACK/SM_CYMAXTRACK
// (the DefWindowProc WM_GETMINMAXINFO default), which is what stops a window larger than the
// desktop from ever existing. SM_CXSCREEN/SM_CYSCREEN are deliberately NOT touched here: the
// game re-reads those every frame and builds its projection from them, so inflating them is
// what skewed the image as the head moved.
int(WINAPI* g_origGetSystemMetrics)(int) = nullptr;
int WINAPI HookGetSystemMetrics(int idx) {
    const int v = g_origGetSystemMetrics(idx);
    if (idx != SM_CXMAXTRACK && idx != SM_CYMAXTRACK) return v;
    const uint32_t fw = g_fakeDeskW.load(std::memory_order_relaxed);
    const uint32_t fh = g_fakeDeskH.load(std::memory_order_relaxed);
    if (!fw || !CallerIsGameOrDxgi(_ReturnAddress())) return v;
    const int want = (idx == SM_CXMAXTRACK) ? (int)fw : (int)fh;
    return v < want ? want : v;
}

void InstallMonitorHooks() {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32) return;
    struct { const char* name; void* hook; void** orig; } fns[] = {
        {"GetSystemMetrics", (void*)&HookGetSystemMetrics, (void**)&g_origGetSystemMetrics},
        {"GetMonitorInfoW", (void*)&HookGetMonitorInfoW, (void**)&g_origGetMonitorInfoW},
        {"GetMonitorInfoA", (void*)&HookGetMonitorInfoA, (void**)&g_origGetMonitorInfoA},
        {"EnumDisplaySettingsW", (void*)&HookEnumDisplaySettingsW, (void**)&g_origEnumDisplaySettingsW},
        {"EnumDisplaySettingsExW", (void*)&HookEnumDisplaySettingsExW, (void**)&g_origEnumDisplaySettingsExW},
    };
    for (auto& f : fns) {
        void* t = (void*)GetProcAddress(u32, f.name);
        if (!t) continue;
        if (MH_CreateHook(t, f.hook, f.orig) != MH_OK || MH_EnableHook(t) != MH_OK)
            LOGW("[patch] hook %s failed; GameRes stays capped at the desktop size", f.name);
    }
}

// ---- [VR] GameResScan: who reads (or restores) the patched resolution-table slot? ----
//
// The open question on the oversize path: every desktop-size API was lied to without effect
// (see config.h at vrGameResOversize), so the "apply settings" pass that rejects an entry
// larger than the desktop must compare against something cached. This census answers it the
// way posescan/camwatch answered theirs: PAGE_GUARD on the patched slots, a vectored handler
// records every touch (rip, registers, instruction bytes - enough to identify the compare
// and its other operand offline), single-step re-arms, aggregate dump at the end. Armed once
// at startup, right after the slots are written, because the reject was measured to happen
// within the first seconds of boot (the swapchain that comes up IS its outcome). A WRITE
// record would be its own answer: Arxan's integrity repair restoring the stock entry, in
// which case there is no size check to beat at all, only a checksum.
namespace reswatch {

struct Rec {
    uint64_t rip = 0;
    uint64_t addr = 0;
    uint64_t regs[16];     // rax rcx rdx rbx rsp rbp rsi rdi r8..r15
    uint8_t  bytes[16];    // instruction bytes at rip, decoded offline
    uint32_t tid = 0;
    uint8_t  write = 0;    // ExceptionInformation[0]: 0 read, 1 write
};
constexpr int kMaxRec = 4096;
Rec  g_rec[kMaxRec];
std::atomic<long> g_recN{0};
std::atomic<long> g_faults{0};    // ALL guard faults on our pages, recorded or not
std::atomic<bool> g_active{false};

constexpr int kMaxPages = 8;
uintptr_t g_pages[kMaxPages] = {};
DWORD     g_pageProt[kMaxPages] = {};
int       g_pageCount = 0;
void*     g_veh = nullptr;
uint32_t  g_tgtW = 0, g_tgtH = 0, g_scrW = 0, g_scrH = 0;   // for the dump annotations
thread_local bool t_rearm = false;

void ArmGuard() {
    for (int i = 0; i < g_pageCount; ++i) {
        DWORD o;
        VirtualProtect((void*)g_pages[i], 0x1000, g_pageProt[i] | PAGE_GUARD, &o);
    }
}
void DisarmGuard() {
    for (int i = 0; i < g_pageCount; ++i) {
        DWORD o;
        VirtualProtect((void*)g_pages[i], 0x1000, g_pageProt[i], &o);
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
            // These pages hold other data too; a page that turns out to be hot would drag the
            // whole game through exception dispatch, so a runaway census disarms itself.
            if (g_faults.fetch_add(1, std::memory_order_relaxed) > 3000000) {
                g_active.store(false);
                return EXCEPTION_CONTINUE_EXECUTION;   // guard already cleared; stop re-arming
            }
            bool slot = false;
            for (uint8_t* s : g_resSlots)
                if (a >= (uintptr_t)s && a < (uintptr_t)s + 8) slot = true;
            if (slot) {
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

void ModName(uint64_t rip, char* out, size_t n) {
    auto* exe = (uint8_t*)GetModuleHandleW(nullptr);
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)rip, &m);
    if (m == (HMODULE)exe) { snprintf(out, n, "exe+0x%llx", (unsigned long long)(rip - (uint64_t)(uintptr_t)exe)); return; }
    if (m) {
        wchar_t path[MAX_PATH]; GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
        snprintf(out, n, "%S+0x%llx", base, (unsigned long long)(rip - (uint64_t)(uintptr_t)m));
        return;
    }
    snprintf(out, n, "0x%llx", (unsigned long long)rip);   // outside any module: Arxan region
}

void Dump() {
    const long total = g_recN.load();
    const long n = total < kMaxRec ? total : kMaxRec;
    LOGI("[reswatch] ---- %ld slot touches recorded (%ld kept, %ld page faults total) on %zu "
         "slot(s), target %ux%u, screen %ux%u ----",
         total, n, g_faults.load(), g_resSlots.size(), g_tgtW, g_tgtH, g_scrW, g_scrH);

    // The first few in order: boot-time validation is a one-shot sequence, and the order
    // (our slot, then the entry after it) is what tells the walker from the menu lister.
    for (long i = 0; i < n && i < 12; ++i) {
        const Rec& r = g_rec[i];
        char mod[64]; ModName(r.rip, mod, sizeof(mod));
        LOGI("[reswatch] #%02ld %-22s %s slot+%d tid %lu", i, mod, r.write ? "WRITE" : "read ",
             (int)(r.addr - (uintptr_t)g_resSlots[0]) & 7, (unsigned long)r.tid);
    }

    // Aggregate by rip, highest count first.
    struct Agg { uint64_t rip; long count; long writes; int sample; };
    constexpr int kMaxAgg = 64;
    Agg agg[kMaxAgg]; int na = 0;
    for (long i = 0; i < n; ++i) {
        const Rec& r = g_rec[i];
        int k = 0;
        for (; k < na; ++k) if (agg[k].rip == r.rip) break;
        if (k == na) { if (na == kMaxAgg) continue; agg[na++] = {r.rip, 0, 0, (int)i}; }
        agg[k].count++;
        if (r.write) agg[k].writes++;
    }
    for (int i = 0; i < na; ++i)
        for (int j = i + 1; j < na; ++j)
            if (agg[j].count > agg[i].count) { Agg t = agg[i]; agg[i] = agg[j]; agg[j] = t; }

    for (int i = 0; i < na; ++i) {
        const Rec& s = g_rec[agg[i].sample];
        char mod[64]; ModName(agg[i].rip, mod, sizeof(mod));
        char bytes[3 * 16 + 1] = {};
        for (int b = 0; b < 16; ++b) snprintf(bytes + b * 3, 4, "%02X ", s.bytes[b]);
        // Flag registers holding the sizes in play: the compare's other operand gives away
        // where the cached desktop size lives.
        char regs[16 * 32] = {}; int p = 0;
        for (int rg = 0; rg < 16; ++rg) {
            const uint32_t lo = (uint32_t)s.regs[rg];
            const char* tag = lo == g_scrW ? "=scrW" : lo == g_scrH ? "=scrH"
                            : lo == g_tgtW ? "=tgtW" : lo == g_tgtH ? "=tgtH" : "";
            p += snprintf(regs + p, sizeof(regs) - p, "%s=%llx%s ", kRegName[rg],
                          (unsigned long long)s.regs[rg], tag);
        }
        LOGI("[reswatch] %-22s x%-4ld %s tid %lu | %s", mod, agg[i].count,
             agg[i].writes ? (agg[i].writes == agg[i].count ? "WRITE" : "MIXED") : "read ",
             (unsigned long)s.tid, bytes);
        LOGI("[reswatch]   regs: %s", regs);
    }

    // Did the patch survive the window? A slot no longer holding the target after an Arxan
    // repair is the whole story in one line.
    for (size_t i = 0; i < g_resSlots.size(); ++i) {
        uint32_t sw = 0, sh = 0;
        SafeRead(g_resSlots[i], &sw, 4); SafeRead(g_resSlots[i] + 4, &sh, 4);
        LOGI("[reswatch] slot %zu now holds %ux%u (%s)", i, sw, sh,
             (sw == g_tgtW && sh == g_tgtH) ? "patch intact" : "PATCH REVERTED");
    }
    LOGI("[reswatch] ---- census complete (%d unique rips) ----", na);
}

DWORD WINAPI WatchThread(LPVOID) {
    const float secs = Cfg().vrGameResScanSeconds;
    const ULONGLONG until = GetTickCount64() + (ULONGLONG)(secs * 1000.f);
    while (GetTickCount64() < until && g_recN.load() < kMaxRec &&
           g_active.load(std::memory_order_relaxed))
        Sleep(50);
    g_active.store(false);
    Sleep(150);   // drain in-flight faults before tearing down
    DisarmGuard();
    if (g_veh) RemoveVectoredExceptionHandler(g_veh);
    Dump();
    return 0;
}

void Arm(uint32_t tgtW, uint32_t tgtH) {
    static bool s_armed = false;
    if (s_armed || g_resSlots.empty()) return;
    s_armed = true;
    g_tgtW = tgtW; g_tgtH = tgtH;
    g_scrW = (uint32_t)GetSystemMetrics(SM_CXSCREEN);
    g_scrH = (uint32_t)GetSystemMetrics(SM_CYSCREEN);
    for (uint8_t* s : g_resSlots) {
        for (int half = 0; half < 2; ++half) {
            const uintptr_t page = ((uintptr_t)s + half * 7) & ~0xFFFull;
            int i = 0;
            for (; i < g_pageCount; ++i) if (g_pages[i] == page) break;
            if (i < g_pageCount || g_pageCount == kMaxPages) continue;
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery((void*)page, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) continue;
            g_pages[g_pageCount] = page;
            g_pageProt[g_pageCount] = mbi.Protect & ~(DWORD)PAGE_GUARD;
            ++g_pageCount;
        }
    }
    if (!g_pageCount) { LOGW("[reswatch] no committed page to watch?"); return; }
    g_recN.store(0);
    g_veh = AddVectoredExceptionHandler(1, Veh);
    g_active.store(true);
    ArmGuard();
    LOGI("[reswatch] armed on %d page(s) over %zu slot(s) for %.0f s (slot 0 at %p)",
         g_pageCount, g_resSlots.size(), Cfg().vrGameResScanSeconds, g_resSlots[0]);
    if (HANDLE h = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr)) CloseHandle(h);
}

} // namespace reswatch

// SIZE CHECK. The boot-time settings apply walks the resolution table and skips any entry
// larger than the desktop - THE cap this whole feature lived under. Found by the GameResScan
// census 2026-08-22 (1.16.2, exe+0xe97577):
//   44 39 37             cmp [rdi], r14d      ; entry.width  vs desktop.width
//   0F 8F xx xx xx xx    jg  skip             ; wider than the desktop -> skip entry
//   44 39 7F 04          cmp [rdi+4], r15d    ; entry.height vs desktop.height
//   0F 8F xx xx xx xx    jg  skip             ; taller -> skip entry
// r14d/r15d held the TRUE 3440x1440 while every hooked size API was actively lying (the
// monitor-rect lie delivered 3088x3280 in the same window), so the walker's desktop size
// comes from a source the hooks never covered. Rather than keep hunting it, both jg's are
// NOPed: every entry is then "small enough", the XML request matches ours, and the only
// remaining obstacle is the Windows max-track window clamp the monitor hooks already lift.
// Side effect: the in-game windowed resolution list shows every table entry. Harmless.
constexpr const char* kPatSizeCheck = "44 39 37 0F 8F ?? ?? ?? ?? 44 39 7F 04 0F 8F";

bool PatchSizeCheck() {
    static int s_state = -1;   // -1 untried, 0 unavailable, 1 patched
    if (s_state >= 0) return s_state == 1;
    uint8_t* at = AobScan(kPatSizeCheck);
    if (!at) {
        s_state = 0;
        LOGW("[patch] resolution size check not found (game patch moved it?); GameRes stays "
             "capped at the desktop size");
        return false;
    }
    const uint8_t nop6[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    s_state = (PatchWrite(at + 3, nop6, 6) && PatchWrite(at + 13, nop6, 6)) ? 1 : 0;
    if (s_state)
        LOGI("[patch] resolution size check removed at rva 0x%llx: the game now accepts "
             "render sizes larger than the desktop",
             (unsigned long long)(at - (uint8_t*)GetModuleHandleW(nullptr)));
    else
        LOGW("[patch] resolution size check write failed");
    return s_state == 1;
}

std::wstring CachePath() {
    const std::wstring& ini = Cfg().iniPath;
    size_t p = ini.find_last_of(L"\\/");
    return (p == std::wstring::npos ? std::wstring(L".") : ini.substr(0, p)) + L"\\ERVR.cache.ini";
}

// The target the settings file should hold; false = feature off / stereo off. The auto size
// and the final snap (game_res.h) are shared with the DllMain-time config write, which must
// land on the identical numbers or the settings-apply rejects the request.
bool ResolveResTarget(uint32_t& w, uint32_t& h) {
    const Config& c = Cfg();
    if (c.vrStereoMode == "off" || c.vrGameRes == "off" || c.vrGameRes.empty()) return false;
    if (c.vrGameRes == "auto") {
        const std::wstring cache = CachePath();
        uint32_t ew = (uint32_t)GetPrivateProfileIntW(L"Cache", L"HmdEyeW", 0, cache.c_str());
        uint32_t eh = (uint32_t)GetPrivateProfileIntW(L"Cache", L"HmdEyeH", 0, cache.c_str());
        if (!ew || !eh) {
            // First run, headset never seen: a sane near-square default (Quest 3 panel).
            // The runtime's real recommendation lands in the cache on the first VR session.
            ew = 2064; eh = 2208;
        }
        w = ew;
        h = eh;   // GameResScale is applied to the FITTED height below, not here
    } else if (sscanf_s(c.vrGameRes.c_str(), "%ux%u", &w, &h) != 2 &&
               sscanf_s(c.vrGameRes.c_str(), "%uX%u", &w, &h) != 2) {
        LOGW("[patch] [VR] GameRes=\"%s\" is not off/auto/WxH; resolution untouched",
             c.vrGameRes.c_str());
        return false;
    }
    // The desktop used to be a hard ceiling: the settings-apply pass skips any table entry
    // larger than the screen. With GameResOversize the check itself is patched out (see
    // kPatSizeCheck - found by the GameResScan census after every API lie failed), so the
    // target survives at full size; the desktop-size lies stay armed anyway because Windows
    // separately clamps the WINDOW to the max-track metrics. When the check pattern does not
    // resolve (a game patch moved it) everything falls back to the old fit-to-desktop.
    const uint32_t screenW = (uint32_t)std::max(640, GetSystemMetrics(SM_CXSCREEN));
    const uint32_t screenH = (uint32_t)std::max(480, GetSystemMetrics(SM_CYSCREEN));
    const float s = std::min(2.f, std::max(0.25f, c.vrGameResScale));
    const bool oversize = c.vrGameResOversize && PatchSizeCheck();
    if (oversize) {
        const uint32_t askW = w, askH = h;
        if (c.vrGameRes == "auto") {
            // No ceiling: a 16:9 frame at the eye recommendation's pixel budget (see
            // Auto169Target), GameResScale scaling that height.
            Auto169Target(askW, askH, s, w, h);
            SnapGameRes(w, h);
            LOGI("[patch] GameRes auto: 16:9 frame %ux%u at the headset's %ux%u pixel budget"
                 " (x%.2f) - fitted-frame geometry, oversize density", w, h, askW, askH, s);
        } else {
            SnapGameRes(w, h);
            if (w != askW || h != askH)
                LOGI("[patch] GameRes %ux%u -> %ux%u: %s", askW, askH, w, h,
                     (uint64_t)w * 9 == (uint64_t)h * 16
                         ? "exact 16:9 - a frame slightly off 16:9 renders 16:9 inside black "
                           "bars, where the HUD layer cannot find the UI"
                         : "multiples of 8");
        }
        // After the snap: the lies have to cover the size actually requested.
        if (w > screenW || h > screenH) {
            g_fakeDeskW.store(w + 16);
            g_fakeDeskH.store(h + 16);
            g_displayLiesOff.store(false);   // fresh apply cycle: lie until the resize lands
            InstallMonitorHooks();
            LOGI("[patch] GameRes %ux%u exceeds the %ux%u desktop: size check patched and "
                 "window clamp lifted, no DSR needed. The swapchain line below is the proof.",
                 w, h, screenW, screenH);
        }
        return true;
    } else if (c.vrGameRes == "auto") {
        // Fit to the desktop: HEIGHT-first, because per-eye pixels track the frame's height
        // almost alone - the crop keeps roughly 1.05*height horizontally however wide the
        // frame is, since a wider frame just widens the rendered FOV proportionally. So
        // height is maximised against the desktop and the width follows at 16:9.
        //
        // ALWAYS, not only when the target overflows: the headset's own eye size is nearly
        // square, so an auto target left alone on a large desktop (one running DSR, say)
        // would be non-16:9 and bring the 16:9 mask and its shear straight back (measured
        // in-headset 2026-08-21; the letterbox patch covers the oversize path instead).
        // GameResScale lands HERE, on the fitted height, so it reads as "fraction of the
        // sharpest frame this desktop allows". Applied to the headset's raw recommendation
        // instead it would do nothing at all until it dropped below the desktop cap.
        const uint32_t maxH = std::min({screenH, (uint32_t)((uint64_t)screenW * 9 / 16)});
        uint32_t fh = std::min({(uint32_t)(std::min(h, maxH) * s), maxH});
        uint32_t fw = (uint32_t)((uint64_t)fh * 16 / 9);
        SnapGameRes(fw, fh, maxH);   // exact 16:9, still inside the desktop
        if (fw != w || fh != h)
            LOGI("[patch] GameRes target %ux%u -> %ux%u: the tallest 16:9 frame that fits the "
                 "%ux%u desktop%s.",
                 w, h, fw, fh, screenW, screenH,
                 c.vrGameResOversize ? " (size-check patch unavailable on this game version)"
                                     : " (GameResOversize=false)");
        w = fw; h = fh;
        return true;
    } else if (w > screenW || h > screenH) {
        LOGW("[patch] GameRes %ux%u is larger than the %ux%u desktop and the size-check patch "
             "is unavailable; the game will clamp it to a built-in size. Lower it.",
             w, h, screenW, screenH);
    }
    SnapGameRes(w, h);
    return true;
}

// GraphicsConfig.xml is UTF-16LE with a BOM. Read it whole, swap three tag bodies, write it
// back - never regenerate it, or every unrelated graphics setting the player chose is lost.
bool ReadFileW(const std::wstring& path, std::wstring& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    std::vector<uint8_t> bytes;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 2 && size.QuadPart < (1 << 20);
    if (ok) {
        bytes.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = ReadFile(f, bytes.data(), (DWORD)bytes.size(), &got, nullptr) && got == bytes.size();
    }
    CloseHandle(f);
    if (!ok) return false;
    size_t off = (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) ? 2 : 0;   // BOM
    out.assign((const wchar_t*)(bytes.data() + off), (bytes.size() - off) / sizeof(wchar_t));
    return true;
}

bool WriteFileW(const std::wstring& path, const std::wstring& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const uint8_t bom[2] = {0xFF, 0xFE};
    DWORD wrote = 0;
    bool ok = WriteFile(f, bom, 2, &wrote, nullptr) && wrote == 2;
    if (ok) {
        const DWORD n = (DWORD)(text.size() * sizeof(wchar_t));
        ok = WriteFile(f, text.data(), n, &wrote, nullptr) && wrote == n;
    }
    CloseHandle(f);
    return ok;
}

bool SetTag(std::wstring& s, const wchar_t* tag, const std::wstring& value) {
    const std::wstring open = std::wstring(L"<") + tag + L">";
    const std::wstring close = std::wstring(L"</") + tag + L">";
    const size_t a = s.find(open);
    if (a == std::wstring::npos) return false;
    const size_t b = s.find(close, a + open.size());
    if (b == std::wstring::npos) return false;
    s.replace(a + open.size(), b - a - open.size(), value);
    return true;
}

// Every GraphicsConfig.xml under %APPDATA%\EldenRing (the file sits either directly there or
// in a per-Steam-account subfolder, depending on the install).
std::vector<std::wstring> GraphicsConfigPaths() {
    std::vector<std::wstring> out;
    wchar_t app[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"APPDATA", app, MAX_PATH)) return out;
    const std::wstring root = std::wstring(app) + L"\\EldenRing";
    const std::wstring direct = root + L"\\GraphicsConfig.xml";
    if (GetFileAttributesW(direct.c_str()) != INVALID_FILE_ATTRIBUTES) out.push_back(direct);
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        const std::wstring sub = root + L"\\" + fd.cFileName + L"\\GraphicsConfig.xml";
        if (GetFileAttributesW(sub.c_str()) != INVALID_FILE_ATTRIBUTES) out.push_back(sub);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

uint32_t g_resAppliedW = 0, g_resAppliedH = 0;
uint32_t g_hmdEyeW = 0, g_hmdEyeH = 0;      // last size handed to PatchesNoteHmdEyeSize
uint32_t g_earlyWroteW = 0, g_earlyWroteH = 0;   // what the DllMain-time write put in the XML

} // namespace

float* PatchesFrameDtImm() { return (float*)ResolveFrameDtImm(); }

bool PatchesWriteFrameDt(float dt) {
    uint8_t* at = ResolveFrameDtImm();
    if (!at) return false;
    return PatchWrite(at, &dt, 4);
}

// The limiter must sit ABOVE the headset's refresh. Below it the loop can never meet the
// refresh and the runtime drops the game to half rate: FpsTarget=72 under VD's 90 Hz was the
// "sluggish" 2026-10-31 session (45 Hz display period, ~52 pairs/s, xrWaitFrame holding the
// frame thread 6.6 ms a pair); the same loop uncapped ran 88 pairs/s at a 90 Hz period. 5%
// over the refresh, not equal to it: then xrWaitFrame alone paces the loop - two pacers on
// nominally the same rate drift against each other and drop frames. Raise-only: a limiter
// already above the refresh (or a refresh that later drops) needs nothing. The sim consumes
// the MEASURED frame time, so moving the limiter changes the cap only, never game speed.
void PatchesFollowHeadsetRefresh(float hz) {
    static float s_seen = 0.f;
    if (!Cfg().vrLimiterFollowsHeadset || hz < 30.f || std::fabs(hz - s_seen) < 0.5f) return;
    s_seen = hz;
    const float* imm = PatchesFrameDtImm();
    if (!imm) return;
    float cur = 0.f;
    memcpy(&cur, imm, 4);
    const float curHz = cur > 1e-5f ? 1.f / cur : 0.f;
    const float want = hz * 1.05f;
    if (curHz >= want - 0.5f) {
        LOGI("[patch] frame limiter %.0f fps already clears the %.0f Hz headset", curHz, hz);
        return;
    }
    if (PatchesWriteFrameDt(1.f / want))
        LOGI("[patch] frame limiter %.0f -> %.0f fps to clear the %.0f Hz headset (a limiter "
             "below the refresh makes the runtime halve the game's rate)", curHz, want, hz);
    else
        LOGW("[patch] frame limiter write failed; with the limiter at %.0f fps under a %.0f Hz "
             "headset, raise [Patches] FpsTarget above %.0f", curHz, hz, hz);
}

void PatchesMakeDpiAware() {
    if (!Cfg().patchDpiAware) return;
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    HMODULE u32 = LoadLibraryW(L"user32.dll");   // worker thread: no loader lock held
    if (!u32) return;
    // Without this, a scaled desktop is virtualized for the game: it renders at the logical
    // size and DWM upscales the result, so the pixels VR captures are fewer than the monitor
    // has. Harmless no-op at 100% scaling. A manifest-declared awareness wins over the call.
    using PFN_SetCtx = BOOL(WINAPI*)(HANDLE);
    auto setCtx = (PFN_SetCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    const int before = GetSystemMetrics(SM_CXSCREEN);
    bool ok = false;
    if (setCtx) {
        ok = setCtx((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */) != FALSE;
        if (!ok) ok = setCtx((HANDLE)-3 /* PER_MONITOR_AWARE */) != FALSE;
    }
    if (!ok) {
        using PFN_SetAware = BOOL(WINAPI*)(void);
        if (auto setAware = (PFN_SetAware)GetProcAddress(u32, "SetProcessDPIAware"))
            ok = setAware() != FALSE;
    }
    const int after = GetSystemMetrics(SM_CXSCREEN);
    if (ok && after != before)
        LOGI("[patch] DPI awareness enabled: the game measures the screen as %dx%d (was %d wide "
             "virtualized)", after, GetSystemMetrics(SM_CYSCREEN), before);
    else if (ok)
        LOGD("[patch] DPI awareness set (screen %dx%d; no scaling in play)", after,
             GetSystemMetrics(SM_CYSCREEN));
    else
        LOGD("[patch] DPI awareness unchanged (err %lu; the exe manifest likely declares it)",
             GetLastError());
}

// GraphicsConfig.xml must hold the target BEFORE the game's boot-time read of it, and that
// read happens on the MAIN thread so early that even the first statements of the worker
// thread lose the race (measured 2026-08-22 evening: a write at +80 ms after LogInit still
// lost; the session then never resized above the desktop). The only spot guaranteed to run
// earlier is DllMain(PROCESS_ATTACH) - on the main thread, before the game's entry point.
// Constraints there: loader lock (kernel32 file APIs only - no user32, no logging) and
// Arxan sections possibly still packed (no AobScan). So this is the OPTIMISTIC write: the
// raw target with no size-check patching and no desktop fit. The worker's full pass runs
// ~100 ms later, applies the real patches, and rewrites the file if its answer differs
// (it only differs when the size-check AOB stops resolving on a future game version - that
// one session snaps to a built-in size and the next boot self-heals with the fitted value).
void PatchesEarlyGraphicsConfigWrite(void* module) {
    wchar_t mod[MAX_PATH]{};
    if (!GetModuleFileNameW((HMODULE)module, mod, MAX_PATH)) return;
    std::wstring dir(mod);
    const size_t cut = dir.find_last_of(L"\\/");
    if (cut == std::wstring::npos) return;
    dir.resize(cut);
    const std::wstring ini = dir + L"\\ERVR.ini";

    wchar_t buf[64];
    GetPrivateProfileStringW(L"General", L"Enabled", L"true", buf, 64, ini.c_str());
    if (_wcsicmp(buf, L"false") == 0 || wcscmp(buf, L"0") == 0) return;
    GetPrivateProfileStringW(L"VR", L"StereoMode", L"cinema", buf, 64, ini.c_str());
    if (_wcsicmp(buf, L"off") == 0) return;
    // Only the OVERSIZE path needs this race-beating write: an oversized request must be in
    // the file before the game's boot read or the settings-apply rejects it. Fitted sizes
    // are accepted through the normal patched-table path, and a clean exit leaves the file
    // at the fitted size anyway - writing a raw eye size here in fitted mode would just ask
    // the boot clamp to knock it back down.
    GetPrivateProfileStringW(L"VR", L"GameResOversize", L"true", buf, 64, ini.c_str());
    if (_wcsicmp(buf, L"true") != 0 && wcscmp(buf, L"1") != 0) return;
    GetPrivateProfileStringW(L"VR", L"GameRes", L"auto", buf, 64, ini.c_str());
    uint32_t w = 0, h = 0;
    if (_wcsicmp(buf, L"off") == 0 || !buf[0]) return;
    if (_wcsicmp(buf, L"auto") == 0) {
        const std::wstring cache = dir + L"\\ERVR.cache.ini";
        w = (uint32_t)GetPrivateProfileIntW(L"Cache", L"HmdEyeW", 0, cache.c_str());
        h = (uint32_t)GetPrivateProfileIntW(L"Cache", L"HmdEyeH", 0, cache.c_str());
        if (!w || !h) { w = 2064; h = 2208; }   // first run: Quest 3-sized default
        GetPrivateProfileStringW(L"VR", L"GameResScale", L"0.86", buf, 64, ini.c_str());   // = config.h default
        const double s = std::min(2.0, std::max(0.25, _wtof(buf) > 0.01 ? _wtof(buf) : 0.86));
        // Same arithmetic as ResolveResTarget's oversize branch - the two MUST agree.
        const uint32_t ew = w, eh = h;
        Auto169Target(ew, eh, s, w, h);
    } else if (swscanf_s(buf, L"%ux%u", &w, &h) != 2 && swscanf_s(buf, L"%uX%u", &w, &h) != 2) {
        return;
    }
    SnapGameRes(w, h);

    // The file names the size; the table has to list it by the time the apply walks it.
    g_earlyLastSlot = EarlyLastSlotWrite(w, h);

    for (const std::wstring& path : GraphicsConfigPaths()) {
        std::wstring xml;
        if (!ReadFileW(path, xml)) continue;
        const std::wstring backup = path.substr(0, path.size() - 4) + L"_ervr_backup.xml";
        if (GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES)
            CopyFileW(path.c_str(), backup.c_str(), TRUE);
        std::wstring before = xml;
        const bool okW = SetTag(xml, L"Resolution-WindowScreenWidth", std::to_wstring(w));
        const bool okH = SetTag(xml, L"Resolution-WindowScreenHeight", std::to_wstring(h));
        SetTag(xml, L"ScreenMode", L"WINDOW");
        if (!okW || !okH || xml == before) continue;
        if (WriteFileW(path, xml)) { g_earlyWroteW = w; g_earlyWroteH = h; }
    }
}

void PatchesApplyGameResolution() {
    uint32_t w = 0, h = 0;
    // The attach-time write is silent (no log exists yet); say here what it did, once.
    if (g_earlyWroteW) {
        LOGI("[patch] GraphicsConfig.xml repaired at DLL attach: %ux%u written before the "
             "game's boot read (a clean exit normalizes the file back to the fitted size, "
             "which is why every boot has to re-write it this early)",
             g_earlyWroteW, g_earlyWroteH);
        g_earlyWroteW = g_earlyWroteH = 0;
    }
    static bool s_lastSlotLogged = false;
    if (!s_lastSlotLogged) {
        s_lastSlotLogged = true;
        if (g_earlyLastSlot)
            LOGI("[patch] resolution table's last slot set at DLL attach (rva 0x%llx), ahead of "
                 "the game's boot-time walk",
                 (unsigned long long)(g_earlyLastSlot - (uint8_t*)GetModuleHandleW(nullptr)));
    }
    if (!ResolveResTarget(w, h)) return;
    // The table entry has to exist every launch (it is a memory patch), even when the file
    // already says the right thing - so this runs before the early-out below.
    const bool listed = PatchResTable(w, h, Cfg().vrGameResOversize && PatchSizeCheck());
    // The census must be armed HERE, before the game's boot-time settings apply walks the
    // table - by the first present it has already happened.
    if (Cfg().vrGameResScan) reswatch::Arm(w, h);
    // Only needed when the target is not 16:9 - at 16:9 there is nothing to mask off, and a
    // patch we do not need is a patch that cannot misfire.
    const float aspect = (float)w / (float)h;
    if (Cfg().vrGameResAspectFix && std::fabs(aspect - 16.f / 9.f) > 0.01f) PatchAspectClamp();
    if (w == g_resAppliedW && h == g_resAppliedH) return;

    const std::vector<std::wstring> paths = GraphicsConfigPaths();
    if (paths.empty()) {
        LOGW("[patch] GameRes: no GraphicsConfig.xml under %%APPDATA%%\\EldenRing - run the game "
             "once to create it, or set [VR] GameRes=off");
        return;
    }
    for (const std::wstring& path : paths) {
        std::wstring xml;
        if (!ReadFileW(path, xml)) { LOGW("[patch] GameRes: cannot read %S", path.c_str()); continue; }
        // One-time backup of whatever the player had before ERVR ever touched it.
        const std::wstring backup = path.substr(0, path.size() - 4) + L"_ervr_backup.xml";
        if (GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES)
            CopyFileW(path.c_str(), backup.c_str(), TRUE);

        const std::wstring ws = std::to_wstring(w), hs = std::to_wstring(h);
        std::wstring before = xml;
        const bool okW = SetTag(xml, L"Resolution-WindowScreenWidth", ws);
        const bool okH = SetTag(xml, L"Resolution-WindowScreenHeight", hs);
        // Windowed is mandatory for VR anyway: exclusive fullscreen crashes on startup with a
        // streaming headset, and borderless would resample to the desktop size.
        SetTag(xml, L"ScreenMode", L"WINDOW");
        if (!okW || !okH) {
            LOGW("[patch] GameRes: %S has no Resolution-WindowScreen tags; left alone", path.c_str());
            continue;
        }
        if (xml == before) { g_resAppliedW = w; g_resAppliedH = h; continue; }   // already correct
        if (!WriteFileW(path, xml)) {
            LOGW("[patch] GameRes: cannot write %S (is the game already running?)", path.c_str());
            continue;
        }
        LOGI("[patch] GameRes: %S set to windowed %ux%u (%s)%s. No in-game resolution step is "
             "needed; the original was saved next to it as GraphicsConfig_ervr_backup.xml.",
             path.c_str(), w, h,
             Cfg().vrGameRes == "auto" ? "auto: headset eye size x GameResScale" : "GameRes override",
             listed ? " and registered in the game's resolution table"
                    : " -- WARNING: not in the game's table, it will snap to a built-in size");
        g_resAppliedW = w; g_resAppliedH = h;
    }
}

void PatchesNoteBackbufferSize(uint32_t w, uint32_t h) {
    if (g_displayLiesOff.load(std::memory_order_relaxed)) return;
    const uint32_t fh = g_fakeDeskH.load(std::memory_order_relaxed);
    if (!fh) return;   // lies not armed
    // "Landed" = the back buffer is past the REAL desktop in either dimension, not "equals
    // the request": the game snaps a request to its nearest built-in size (4224x2376 asked,
    // 3840x2160 delivered, 2026-08-23), and waiting for the exact request left the height
    // lie standing all session - the vertical head-motion warp again. SM_CYSCREEN is never
    // lied to, so it is the truthful comparand here.
    const uint32_t realW = (uint32_t)std::max(640, GetSystemMetrics(SM_CXSCREEN));
    const uint32_t realH = (uint32_t)std::max(480, GetSystemMetrics(SM_CYSCREEN));
    if (w <= realW && h <= realH) return;   // still the boot-fitted swapchain
    g_displayLiesOff.store(true);
    LOGI("[patch] oversized swapchain landed (%ux%u): desktop-size lies DISARMED - the "
         "game's in-play display-metric polls see the true desktop again (a standing "
         "height lie is the vertical head-motion warp)", w, h);
}

bool PatchesLetterboxRemoved() { return g_letterboxOff.load(std::memory_order_relaxed); }

void PatchesNoteHmdEyeSize(uint32_t w, uint32_t h) {
    if (!w || !h) return;
    if (w == g_hmdEyeW && h == g_hmdEyeH) return;   // called at scan cadence; dedupe first
    g_hmdEyeW = w; g_hmdEyeH = h;
    const std::wstring cache = CachePath();
    const uint32_t cw = (uint32_t)GetPrivateProfileIntW(L"Cache", L"HmdEyeW", 0, cache.c_str());
    const uint32_t ch = (uint32_t)GetPrivateProfileIntW(L"Cache", L"HmdEyeH", 0, cache.c_str());
    if (cw != w || ch != h) {
        WritePrivateProfileStringW(L"Cache", L"HmdEyeW", std::to_wstring(w).c_str(), cache.c_str());
        WritePrivateProfileStringW(L"Cache", L"HmdEyeH", std::to_wstring(h).c_str(), cache.c_str());
        LOGI("[patch] headset recommends %ux%u per eye; cached as the GameRes=auto target", w, h);
    }
    // The game read GraphicsConfig.xml at startup, so a change here lands on the NEXT launch.
    PatchesApplyGameResolution();
}

void PatchesInit() {
    // Before the game reads its settings file. Writing it is what sets the render resolution.
    PatchesApplyGameResolution();

    const int fps = Cfg().patchFpsTarget;
    if (fps <= 0) return;
    // uberhalit's own clamp. Below 30 the game misbehaves; above 360 nothing sensible.
    const int target = fps < 30 ? 30 : (fps > 360 ? 360 : fps);
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);

    uint8_t* flimm = ResolveFrameDtImm();
    if (!flimm) {
        LOGW("[patch] FpsTarget=%d: framelock pattern not found (game patch changed it?); no unlock", fps);
        return;
    }
    float before = 0.f;
    memcpy(&before, flimm, 4);
    const float dt = 1.0f / (float)target;
    if (!PatchWrite(flimm, &dt, 4)) {
        LOGW("[patch] framelock found at rva 0x%llx but the write failed", (unsigned long long)(flimm - base));
        return;
    }
    LOGI("[patch] fps unlock: frame tick at rva 0x%llx: %.1f -> %d fps (dt %.5f) - the "
         "limiter target; game speed follows the measured frame time.",
         (unsigned long long)(flimm - base),
         before > 1e-6f ? 1.f / before : 0.f, target, dt);

    if (uint8_t* hz = AobScan(kPatHertzlock)) {
        uint8_t* b = hz + kOffHertzBase;
        const int32_t zero = 0;
        const bool ok1 = PatchWrite(b + kOffHertzInt1, &zero, 4);
        const bool ok2 = PatchWrite(b + kOffHertzInt2, &zero, 4);
        if (ok1 && ok2)
            LOGI("[patch] hertz lock removed at rva 0x%llx (display modes no longer forced to 60 Hz)",
                 (unsigned long long)(b - base));
        else
            LOGW("[patch] hertz lock write failed");
    } else {
        // Only matters for exclusive fullscreen; borderless/windowed never hits that path.
        LOGD("[patch] hertz-lock pattern not found; exclusive fullscreen may stay 60 Hz");
    }
}
}
