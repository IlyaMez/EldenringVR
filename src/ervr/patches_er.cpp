#include "patches_er.h"
#include "game_memory.h"
#include "config.h"
#include "log.h"
#include <windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <algorithm>
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
    if (s_state)
        LOGI("[patch] 16:9 letterbox removed at rva 0x%llx: the frame now fills the window, so "
             "its frustum matches its dimensions and the eye crop is exact",
             (unsigned long long)(je - (uint8_t*)GetModuleHandleW(nullptr)));
    else
        LOGW("[patch] 16:9 letterbox patch write failed");
    return s_state == 1;
}

// Put `w`x`h` into the table so the game accepts it from GraphicsConfig.xml.
bool PatchResTable(uint32_t w, uint32_t h) {
    static std::vector<uint8_t*> s_slots;
    static bool s_scanned = false;
    if (!s_scanned) {
        s_scanned = true;
        for (uint8_t* slot : AobScanData(kPatResTable)) {
            uint32_t sw = 0, sh = 0;
            // Bit-exact check: if the slot does not read as the stock 1920x1080 the layout is
            // not what we think it is, and writing blind would corrupt a real entry.
            if (SafeRead(slot, &sw, 4) && SafeRead(slot + 4, &sh, 4) && sw == kSlotW && sh == kSlotH)
                s_slots.push_back(slot);
        }
        if (s_slots.empty())
            LOGW("[patch] resolution table not found; the game will snap GameRes to its nearest "
                 "built-in size");
    }
    const uint32_t wh[2] = {w, h};
    int ok = 0;
    for (uint8_t* slot : s_slots) if (PatchWrite(slot, wh, sizeof(wh))) ++ok;
    return ok > 0;
}

// ---- desktop-size lie, deliberately narrow -------------------------------------------
//
// The size clamp does NOT read GetSystemMetrics: inflating that changed nothing, and since
// the game re-reads SM_CXSCREEN/SM_CYSCREEN every frame, lying there skewed the live
// projection instead. What it does read is the monitor RECTANGLE, which reaches the game
// through DXGI - dxgi.dll implements IDXGIOutput::GetDesc on top of GetMonitorInfoW, and
// dxgi.dll+0x11257 / +0x4ad1c were the only callers in the census. So the lie lives here,
// affects one rarely-called API, and is dropped the moment the target fits the desktop.
std::atomic<uint32_t> g_fakeDeskW{0}, g_fakeDeskH{0};

BOOL(WINAPI* g_origGetMonitorInfoW)(HMONITOR, LPMONITORINFO) = nullptr;
BOOL(WINAPI* g_origGetMonitorInfoA)(HMONITOR, LPMONITORINFO) = nullptr;

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

void InflateMonitorInfo(LPMONITORINFO mi, void* ret) {
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
    const uint32_t fw = g_fakeDeskW.load(std::memory_order_relaxed);
    const uint32_t fh = g_fakeDeskH.load(std::memory_order_relaxed);
    if (!fw || !dm) return;
    const bool ours = CallerIsGameOrDxgi(ret);
    static std::atomic<int> s_n{0};
    if (s_n.fetch_add(1, std::memory_order_relaxed) < 10)
        LOGI("[patch] EnumDisplaySettings(mode %ld) -> %lux%lu%s", (long)(int)mode,
             dm->dmPelsWidth, dm->dmPelsHeight, ours ? "" : "   [not game/dxgi]");
    if (!ours || mode != ENUM_CURRENT_SETTINGS) return;
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

std::wstring CachePath() {
    const std::wstring& ini = Cfg().iniPath;
    size_t p = ini.find_last_of(L"\\/");
    return (p == std::wstring::npos ? std::wstring(L".") : ini.substr(0, p)) + L"\\ERVR.cache.ini";
}

// The target the settings file should hold; false = feature off / stereo off.
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
        float s = c.vrGameResScale;
        s = s < 0.25f ? 0.25f : (s > 2.f ? 2.f : s);
        w = (uint32_t)(ew * s);
        h = (uint32_t)(eh * s);
    } else if (sscanf_s(c.vrGameRes.c_str(), "%ux%u", &w, &h) != 2 &&
               sscanf_s(c.vrGameRes.c_str(), "%uX%u", &w, &h) != 2) {
        LOGW("[patch] [VR] GameRes=\"%s\" is not off/auto/WxH; resolution untouched",
             c.vrGameRes.c_str());
        return false;
    }
    // Fit to the desktop: the game refuses anything larger, from the menu AND from the file.
    // HEIGHT-first, because per-eye pixels track the frame's height almost alone - the crop
    // keeps roughly 1.05*height horizontally however wide the frame is, since a wider frame
    // just widens the rendered FOV proportionally. So height is maximised against the
    // desktop and the width simply follows at 16:9.
    //
    // 16:9 exactly, even though a squarer frame reaches the same sharpness for ~35% fewer
    // pixels: the game masks anything else back to 16:9 with black bars, which wastes those
    // pixels AND leaves the frame's true frustum disagreeing with its dimensions - and since
    // the eye crop is computed from the dimensions, the image then shears as the head turns
    // (both measured in-headset, 2026-08-21). GameResAspectFix can unmask it, but that is an
    // experiment you opt into by setting a non-16:9 GameRes explicitly.
    const uint32_t screenW = (uint32_t)std::max(640, GetSystemMetrics(SM_CXSCREEN));
    const uint32_t screenH = (uint32_t)std::max(480, GetSystemMetrics(SM_CYSCREEN));
    if ((w > screenW || h > screenH) && c.vrGameResOversize) {
        // Tell the size check the desktop is big enough (see the monitor-rect hook above)
        // instead of shrinking the target to fit it.
        g_fakeDeskW.store(w + 16);
        g_fakeDeskH.store(h + 16);
        InstallMonitorHooks();
    } else if (w > screenW || h > screenH) {
        const uint32_t fh = std::min({h, screenH, (uint32_t)((uint64_t)screenW * 9 / 16)});
        const uint32_t fw = (uint32_t)((uint64_t)fh * 16 / 9);
        LOGI("[patch] GameRes %ux%u exceeds the %ux%u desktop (the game will not render larger "
             "than it); using %ux%u - the tallest 16:9 frame that fits, and height is what "
             "per-eye sharpness depends on. A larger desktop (NVIDIA DSR) raises this ceiling.",
             w, h, screenW, screenH, fw, fh);
        w = fw; h = fh;
    }
    // Multiple-of-8 keeps every downstream size (swapchain, crops, video encoders) happy.
    w = (w + 4u) & ~7u;
    h = (h + 4u) & ~7u;
    const auto clampU = [](uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : (v > hi ? hi : v); };
    w = clampU(w, 640u, 8192u);
    h = clampU(h, 480u, 8192u);
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

} // namespace

float* PatchesFrameDtImm() { return (float*)ResolveFrameDtImm(); }

bool PatchesWriteFrameDt(float dt) {
    uint8_t* at = ResolveFrameDtImm();
    if (!at) return false;
    return PatchWrite(at, &dt, 4);
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

void PatchesApplyGameResolution() {
    uint32_t w = 0, h = 0;
    if (!ResolveResTarget(w, h)) return;
    // The table entry has to exist every launch (it is a memory patch), even when the file
    // already says the right thing - so this runs before the early-out below.
    const bool listed = PatchResTable(w, h);
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
        LOGD("[patch] hertz-lock pattern not found; exclusive fullscreen may stay 60 Hz");
    }
}
}
