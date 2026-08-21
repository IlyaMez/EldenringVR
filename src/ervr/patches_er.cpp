#include "patches_er.h"
#include "game_memory.h"
#include "config.h"
#include "log.h"
#include <windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstdio>
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
// FRAMELOCK: `mov [reg+disp], 0x3C888889` - the 1/60 the game stores as its per-frame delta
// time every frame. This is the SAME constant our registered probe tasks observed coming
// back as FD4TaskData::dt (fixed 0.01667 even when the real frame rate sagged to 40), so
// patching it moves the limiter target AND the simulation step together: the game runs at
// correct speed exactly when it actually reaches the target rate, and in slow motion when
// the GPU cannot keep up. That is inherent to the game's fixed-step design, not a bug here.
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

// RESOLUTION TABLE: the game's selectable resolutions live in a data section as consecutive
// <u32 w><u32 h> pairs; uberhalit's custom-resolution technique overwrites the 1920x1080
// entry with the desired size (the next entry, 2048x1152, anchors the pattern), and flips
// one je->jmp so non-16:9 sizes are not clamped/letterboxed back to 16:9. This is the VR
// sharpness lever: each eye is a CROP of the game frame, so a headset-sized near-square
// game resolution replaces the "enable DSR and pick 4K/5K" user chore.
constexpr const char* kPatResTable = "80 07 00 00 38 04 00 00 00 08 00 00 80 04 00 00";
// The je of the 16:9 clamp. First pattern is pre-1.12; second is game patch 1.12+ (from
// uberhalit PR #147) - 1.16.2 matches only the second.
constexpr const char* kPatResAspectOld = "8B ?? 85 ?? 74 ?? 44 8B ?? ?? 45 85 ?? 74 ?? 41 8B";
constexpr int         kOffResAspectOld = 4;
constexpr const char* kPatResAspectNew = "74 4F 45 8B 94 CC";
constexpr int         kOffResAspectNew = 0;

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

std::wstring CachePath() {
    const std::wstring& ini = Cfg().iniPath;
    size_t p = ini.find_last_of(L"\\/");
    return (p == std::wstring::npos ? std::wstring(L".") : ini.substr(0, p)) + L"\\ERVR.cache.ini";
}

// The target the table entry should hold right now; false = feature off / stereo off.
bool ResolveResTarget(uint32_t& w, uint32_t& h) {
    const Config& c = Cfg();
    if (c.vrStereoMode == "off" || c.vrGameRes == "off" || c.vrGameRes.empty()) return false;
    if (c.vrGameRes == "auto") {
        const std::wstring cache = CachePath();
        uint32_t ew = (uint32_t)GetPrivateProfileIntW(L"Cache", L"HmdEyeW", 0, cache.c_str());
        uint32_t eh = (uint32_t)GetPrivateProfileIntW(L"Cache", L"HmdEyeH", 0, cache.c_str());
        if (!ew || !eh) {
            // First run, headset not seen yet: a sane near-square default (Quest 3 panel).
            // The real value lands in the cache the moment the runtime reports it.
            ew = 2064; eh = 2208;
        }
        float s = c.vrGameResScale;
        s = s < 0.25f ? 0.25f : (s > 2.f ? 2.f : s);
        w = (uint32_t)(ew * s);
        h = (uint32_t)(eh * s);
    } else if (sscanf_s(c.vrGameRes.c_str(), "%ux%u", &w, &h) != 2 &&
               sscanf_s(c.vrGameRes.c_str(), "%uX%u", &w, &h) != 2) {
        LOGW("[patch] [VR] GameRes=\"%s\" is not off/auto/WxH; resolution patch skipped",
             c.vrGameRes.c_str());
        return false;
    }
    // Multiple-of-8 keeps every downstream size (swapchain, crops, video encoders) happy.
    w = (w + 4u) & ~7u;
    h = (h + 4u) & ~7u;
    const auto clampU = [](uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : (v > hi ? hi : v); };
    w = clampU(w, 1024u, 5120u);
    h = clampU(h, 768u, 5120u);
    return true;
}

std::vector<uint8_t*> g_resEntries;         // table entries we overwrite (each: u32 w, u32 h)
uint8_t* g_resAspectJe = nullptr;           // the je byte of the 16:9 clamp
bool     g_resScanned = false;
uint32_t g_resAppliedW = 0, g_resAppliedH = 0;
uint32_t g_hmdEyeW = 0, g_hmdEyeH = 0;      // last size handed to PatchesNoteHmdEyeSize

// ---- window-metrics hooks -----------------------------------------------------------
// The windowed-mode resolution list is the SAME table we patch, filtered to entries that
// fit the desktop the game measures - and the game is DPI-virtualized, so a 3440x1440
// desktop at 150% scaling reads as ~2293x960, which is why the stock windowed list capped
// at 1680x945 (even 1920x1080 was hidden). A VR target is bigger than any real desktop, so
// the filter would always hide it: report a screen large enough for the target to the GAME
// only. SM_C*MAXTRACK is inflated too, so DefWindowProc's WM_GETMINMAXINFO default cannot
// clamp the window either. The window ends up partially offscreen - irrelevant, the
// headset shows the real image and the monitor is just a mirror.
//
// SCOPED TO GAME CODE by return address: the first (process-wide) version fed the fake
// 2128x2336 screen to EVERYTHING in the process - VDXR's compositor, Virtual Desktop's
// client, ReShade, capture hooks - and the VR startup died in a GPU video-engine hang
// (LiveKernelEvent 141, then XR_ERROR_RUNTIME_FAILURE floods, 2026-08-21). Only calls
// whose immediate caller sits inside eldenring.exe see the inflated screen.
std::atomic<uint32_t> g_fakeScreenW{0}, g_fakeScreenH{0};   // 0 = hooks pass through

bool CallerIsGame(void* retAddr) {
    static uintptr_t s_lo = 0, s_hi = 0;
    if (!s_lo) {
        auto* base = (uint8_t*)GetModuleHandleW(nullptr);
        auto* dos = (IMAGE_DOS_HEADER*)base;
        auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        s_lo = (uintptr_t)base;
        s_hi = s_lo + nt->OptionalHeader.SizeOfImage;
    }
    return (uintptr_t)retAddr >= s_lo && (uintptr_t)retAddr < s_hi;
}

int(WINAPI* g_origGetSystemMetrics)(int) = nullptr;
BOOL(WINAPI* g_origGetMonitorInfoW)(HMONITOR, LPMONITORINFO) = nullptr;
BOOL(WINAPI* g_origGetMonitorInfoA)(HMONITOR, LPMONITORINFO) = nullptr;
BOOL(WINAPI* g_origSystemParametersInfoW)(UINT, UINT, PVOID, UINT) = nullptr;

void InflateRectTo(RECT& r, uint32_t w, uint32_t h) {
    if (r.right - r.left < (LONG)w) r.right = r.left + (LONG)w;
    if (r.bottom - r.top < (LONG)h) r.bottom = r.top + (LONG)h;
}

int WINAPI HookGetSystemMetrics(int idx) {
    int v = g_origGetSystemMetrics(idx);
    const uint32_t fw = g_fakeScreenW.load(std::memory_order_relaxed);
    const uint32_t fh = g_fakeScreenH.load(std::memory_order_relaxed);
    if (!fw || !CallerIsGame(_ReturnAddress())) return v;
    switch (idx) {
    case SM_CXSCREEN: case SM_CXFULLSCREEN: case SM_CXVIRTUALSCREEN:
    case SM_CXMAXIMIZED: case SM_CXMAXTRACK:
        if (v < (int)fw) v = (int)fw;
        break;
    case SM_CYSCREEN: case SM_CYFULLSCREEN: case SM_CYVIRTUALSCREEN:
    case SM_CYMAXIMIZED: case SM_CYMAXTRACK:
        if (v < (int)fh) v = (int)fh;
        break;
    default: break;
    }
    return v;
}
BOOL WINAPI HookGetMonitorInfoW(HMONITOR m, LPMONITORINFO mi) {
    BOOL ok = g_origGetMonitorInfoW(m, mi);
    const uint32_t fw = g_fakeScreenW.load(std::memory_order_relaxed);
    if (ok && fw && mi && CallerIsGame(_ReturnAddress())) {
        InflateRectTo(mi->rcMonitor, fw, g_fakeScreenH);
        InflateRectTo(mi->rcWork, fw, g_fakeScreenH);
    }
    return ok;
}
BOOL WINAPI HookGetMonitorInfoA(HMONITOR m, LPMONITORINFO mi) {
    BOOL ok = g_origGetMonitorInfoA(m, mi);
    const uint32_t fw = g_fakeScreenW.load(std::memory_order_relaxed);
    if (ok && fw && mi && CallerIsGame(_ReturnAddress())) {
        InflateRectTo(mi->rcMonitor, fw, g_fakeScreenH);
        InflateRectTo(mi->rcWork, fw, g_fakeScreenH);
    }
    return ok;
}
BOOL WINAPI HookSystemParametersInfoW(UINT action, UINT p, PVOID data, UINT wini) {
    BOOL ok = g_origSystemParametersInfoW(action, p, data, wini);
    const uint32_t fw = g_fakeScreenW.load(std::memory_order_relaxed);
    if (ok && fw && action == SPI_GETWORKAREA && data && CallerIsGame(_ReturnAddress()))
        InflateRectTo(*(RECT*)data, fw, g_fakeScreenH);
    return ok;
}

bool InstallMetricsHooks() {
    static bool s_done = false, s_ok = false;
    if (s_done) return s_ok;
    s_done = true;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32) return false;
    struct { const char* name; void* hook; void** orig; } fns[] = {
        {"GetSystemMetrics", (void*)&HookGetSystemMetrics, (void**)&g_origGetSystemMetrics},
        {"GetMonitorInfoW", (void*)&HookGetMonitorInfoW, (void**)&g_origGetMonitorInfoW},
        {"GetMonitorInfoA", (void*)&HookGetMonitorInfoA, (void**)&g_origGetMonitorInfoA},
        {"SystemParametersInfoW", (void*)&HookSystemParametersInfoW, (void**)&g_origSystemParametersInfoW},
    };
    int n = 0;
    for (auto& f : fns) {
        void* target = (void*)GetProcAddress(u32, f.name);
        if (!target) continue;
        if (MH_CreateHook(target, f.hook, f.orig) == MH_OK && MH_EnableHook(target) == MH_OK) ++n;
        else LOGW("[patch] metrics hook %s failed", f.name);
    }
    s_ok = n > 0;
    return s_ok;
}

} // namespace

void PatchesApplyGameResolution() {
    uint32_t w = 0, h = 0;
    if (!ResolveResTarget(w, h)) {
        // Kill switch: GameRes=off (hot-reloadable) also stops the metrics inflation live.
        if (g_fakeScreenW.exchange(0)) {
            g_fakeScreenH.store(0);
            LOGI("[patch] GameRes off: window-metrics inflation disabled (table entry stays "
                 "patched until restart)");
        }
        return;
    }
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    if (!g_resScanned) {
        g_resScanned = true;
        g_resEntries = AobScanData(kPatResTable);
        if (uint8_t* a = AobScan(kPatResAspectOld)) g_resAspectJe = a + kOffResAspectOld;
        else if (uint8_t* b = AobScan(kPatResAspectNew)) g_resAspectJe = b + kOffResAspectNew;
        if (g_resEntries.empty())
            LOGW("[patch] resolution table (1920x1080 entry) not found in the data sections; "
                 "GameRes unavailable on this game build - fall back to DSR for sharpness");
        if (!g_resAspectJe)
            LOGW("[patch] 16:9 aspect-clamp pattern not found; a non-16:9 GameRes may letterbox");
    }
    if (g_resEntries.empty()) return;
    if (w == g_resAppliedW && h == g_resAppliedH) return;

    const uint32_t wh[2] = {w, h};
    int ok = 0;
    for (uint8_t* e : g_resEntries)
        if (PatchWrite(e, wh, sizeof(wh))) ++ok;
    if (!ok) { LOGW("[patch] resolution table write failed"); return; }
    if (g_resAspectJe && *g_resAspectJe == 0x74) {
        const uint8_t jmp = 0xEB;
        if (PatchWrite(g_resAspectJe, &jmp, 1))
            LOGI("[patch] 16:9 aspect clamp bypassed at rva 0x%llx",
                 (unsigned long long)(g_resAspectJe - base));
        else
            LOGW("[patch] aspect-clamp bypass write failed (16:9 letterbox may remain)");
    }
    const bool re = g_resAppliedW != 0;
    g_resAppliedW = w; g_resAppliedH = h;
    // Let the game believe the screen fits the target (margin for borders/title bar), or
    // its fit-the-desktop filter hides the patched entry from the windowed list and its
    // window sizing clamps the client area right back down.
    g_fakeScreenW.store(w + 64);
    g_fakeScreenH.store(h + 128);
    if (InstallMetricsHooks())
        LOGI("[patch] window-metrics hooks live: the game sees a %ux%u screen so %ux%u "
             "passes its windowed-resolution filter", w + 64, h + 128, w, h);
    else
        LOGW("[patch] window-metrics hooks failed - %ux%u may be missing from the windowed "
             "list (game filters it against the desktop size)", w, h);
    LOGI("[patch] resolution table: %d entr%s patched (first at rva 0x%llx): 1920x1080 -> %ux%u "
         "(%s). USER STEP (once): in-game System > Screen, set Screen Mode to WINDOWED and pick "
         "%ux%u in the resolution list - the game saves it and every later launch applies it "
         "automatically.%s",
         ok, ok == 1 ? "y" : "ies", (unsigned long long)(g_resEntries[0] - base), w, h,
         Cfg().vrGameRes == "auto" ? "auto: headset eye size x scale" : "GameRes override",
         w, h,
         re ? " (target changed: re-pick it in the settings or restart)" : "");
}

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
        LOGI("[patch] headset recommends %ux%u per eye; cached for the GameRes=auto target", w, h);
    }
    PatchesApplyGameResolution();   // no-op unless this changes an auto target
}

float* PatchesFrameDtImm() { return (float*)ResolveFrameDtImm(); }

bool PatchesWriteFrameDt(float dt) {
    uint8_t* at = ResolveFrameDtImm();
    if (!at) return false;
    return PatchWrite(at, &dt, 4);
}

void PatchesInit() {
    // Resolution first: it must land before the game applies its saved display settings,
    // or the saved custom resolution no longer matches a table entry and the game falls
    // back to a default for the whole session.
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
    LOGI("[patch] fps unlock: frame tick at rva 0x%llx: %.1f -> %d fps (dt %.5f). The game "
         "runs slow-motion whenever it cannot actually reach this rate.",
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
        LOGW("[patch] hertz-lock pattern not found; exclusive fullscreen may stay 60 Hz");
    }
}
}
