// ERVR - motion controls for ELDEN RING on top of R.E.A.L. VR.
//
// Lifecycle:
//   DllMain(ATTACH) -> spawn worker thread (never block the loader lock)
//   worker: init log/config/MinHook -> loop { attach VR backend, hook XInput
//           when modules appear, poll VR, build virtual pad } until process exit.
#include "log.h"
#include "config.h"
#include "vr_backend.h"
#include "virtual_pad.h"
#include "xinput_hook.h"
#include "game_memory.h"
#include "midhook.h"
#include "game_er.h"
#include "combat.h"
#include "reshade_addon.h"
#include "stereo.h"
#include "debug_keys.h"
#include "patches_er.h"
#include "frame_probe.h"
#include "cam_watch.h"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <string>

namespace {

HMODULE g_self = nullptr;
HANDLE g_thread = nullptr;
std::atomic<bool> g_quit{false};

std::wstring ModuleDir(HMODULE m) {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(m, buf, MAX_PATH);
    std::wstring s(buf);
    size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"." : s.substr(0, p);
}

ervr::LogLevel ParseLevel(const std::string& s) {
    if (s == "debug") return ervr::LogLevel::Debug;
    if (s == "warn") return ervr::LogLevel::Warn;
    if (s == "error") return ervr::LogLevel::Error;
    return ervr::LogLevel::Info;
}

double Now() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

// Every Dantelion engine assert funnels into ONE deliberate null-write - DL_PANIC at
// exe+0x1eb9999 on 2.6.2.0 ("DL_PANIC is firing core dump", imm 0x00DEADBA). The Windows
// crash record therefore always shows the same offset and never the culprit. This VEH
// matches exactly that address, scans the faulting thread's stack for return addresses into
// the exe, logs them (the log fflushes per line, so it survives the crash), and lets the
// crash proceed. Wrong game version = the address never matches = dormant.
constexpr uintptr_t kRvaDlPanicNullWrite = 0x1eb9999;

LONG CALLBACK PanicVeh(EXCEPTION_POINTERS* xp) {
    using namespace ervr;
    if (xp->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    if ((uint8_t*)xp->ExceptionRecord->ExceptionAddress != base + kRvaDlPanicNullWrite)
        return EXCEPTION_CONTINUE_SEARCH;
    // Pass-2 asserts are SPURIOUS by construction: the second pass replays the frame over
    // world state mid-transition (fast travel's fade-out fired two of these on 2026-08-21)
    // that pass 1 would never see. The panic function is log-then-die: after the 11-byte
    // null-write it just does `add rsp,0x70; pop rdi; ret` - so skipping the write turns
    // the assert into "log and continue" and the asserting task carries on. Only ever done
    // for pass 2; pass-1 panics are real and still crash (with the stack logged below).
    const bool pass2 = FramePumpCurrentPass() == 2;
    static LONG s_eaten = 0;
    if (pass2 && Cfg().vrPass2EatAsserts && InterlockedIncrement(&s_eaten) <= 64) {
        LOGE("[panic] DL_PANIC during pass 2 SUPPRESSED (#%ld) - spurious assert on "
             "transition-state; skipping the deliberate null-write",
             s_eaten);
        xp->ContextRecord->Rip += 11;   // mov dword ptr [0], 0xDEADBA is 11 bytes
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    static LONG s_seen = 0;
    if (InterlockedIncrement(&s_seen) > 3) return EXCEPTION_CONTINUE_SEARCH;
    // Walk the FAULTING context's stack (RtlCaptureStackBackTrace here would walk the
    // exception dispatcher's, not the panicking function's). A raw scan for .text return
    // addresses is crude but names the caller chain reliably enough.
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    const uintptr_t textLo = (uintptr_t)base + 0x1000;
    const uintptr_t textHi = (uintptr_t)base + nt->OptionalHeader.SizeOfImage;
    const uint64_t* sp = (const uint64_t*)xp->ContextRecord->Rsp;
    char line[768]; int p = 0;
    for (int i = 0; i < 256 && p < (int)sizeof(line) - 24; ++i) {
        uint64_t v = 0;
        __try { v = sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (v >= textLo && v < textHi)
            p += snprintf(line + p, sizeof(line) - p, " exe+0x%llx",
                          (unsigned long long)(v - (uintptr_t)base));
    }
    LOGE("[panic] DL_PANIC (deliberate engine assert) on thread %lu during pass %d; "
         "return addresses on the faulting stack:%s",
         GetCurrentThreadId(), FramePumpCurrentPass(), line);
    return EXCEPTION_CONTINUE_SEARCH;
}

FILETIME IniWriteTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d);
    return d.ftLastWriteTime;
}

DWORD WINAPI Worker(LPVOID) {
    using namespace ervr;
    std::wstring dir = ModuleDir(g_self);
    std::wstring ini = dir + L"\\ERVR.ini";
    Cfg().Load(ini);
    LogInit(dir + L"\\ERVR.log", ParseLevel(Cfg().logLevel));
    LOGI("ERVR %s starting (pid %lu) in %S", ERVR_VERSION, GetCurrentProcessId(), dir.c_str());
    if (!Cfg().enabled) { LOGI("Disabled via ERVR.ini; exiting worker"); return 0; }
    // Before the game creates its window and measures the desktop: DPI virtualization is
    // what caps the windowed resolution list (and therefore the VR image) at 1680x945.
    PatchesMakeDpiAware();
    AddVectoredExceptionHandler(1, PanicVeh);   // turn DL_PANIC crashes into logged stacks

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        LOGE("MinHook init failed"); return 0;
    }
    MidHookSelfTest();
    BackendsInit(Cfg().vrRuntime.c_str());
    VirtualPadInit();
    StereoInit();
    if (Cfg().fpEnabled) GameERInit();
    PatchesInit();   // [Patches] FpsTarget etc.; no-op when unset

    FILETIME iniTime = IniWriteTime(ini);
    bool xinputHooked = false, addonInstalled = false;
    double lastModuleScan = 0, lastIniCheck = 0;
    const double period = 1.0 / (Cfg().pollHz > 0 ? Cfg().pollHz : 250);
    int tick = 0;

    while (!g_quit.load()) {
        double t = Now();
        // XInput modules may load lazily; scan every 500 ms until hooked (and keep scanning for late modules).
        if (t - lastModuleScan > 0.5) {
            lastModuleScan = t;
            bool h = XInputHooksInstall();
            if (h && !xinputHooked) { xinputHooked = true; LOGI("XInput hooks live"); }
            // ReShade loads as the game's dxgi.dll; it may appear after us, so keep trying.
            if (!addonInstalled && ReShadeAddonInstall(g_self)) {
                addonInstalled = true;
                LOGI("ReShade add-on live (D3D12 capture + XR submission active)");
            }
            FrameProbeTick();   // M2 spike; no-op unless [Experimental] TaskProbe=true
            CamWatchTick();     // camera-capture hunt; no-op unless [Experimental] CamWatch=true
            DebugKeysTick();    // numpad A/B hotkeys for the stereo experiments
            // Feed the headset's recommended per-eye size to the resolution patch (cached
            // for the next boot; dedup + no-op inside when nothing changed).
            if (IVRBackend* vb = ActiveBackend()) {
                uint32_t ew = 0, eh = 0;
                if (vb->IsReady() && vb->GetRecommendedEyeSize(ew, eh))
                    PatchesNoteHmdEyeSize(ew, eh);
            }
        }
        // Hot-reload the ini (any key except VRRuntime/LogLevel/PollHz takes effect immediately).
        if (t - lastIniCheck > 1.0) {
            lastIniCheck = t;
            FILETIME ft = IniWriteTime(ini);
            if (CompareFileTime(&ft, &iniTime) != 0) {
                iniTime = ft;
                Cfg().Load(ini);
                VirtualPadInit();
                PatchesApplyGameResolution();   // GameRes/GameResScale hot-edit
                LOGI("ERVR.ini reloaded");
            }
        }
        BackendsTick();
        VirtualPadUpdate(t);
        GameERTick();
        if ((++tick % (int)(5.0 / period)) == 0) {   // heartbeat every ~5s at debug level
            VRTracking tr; VRInputState in;
            long calls, merged, fab; XInputHookStats(calls, merged, fab);
            if (GetLatestVRState(tr, in))
                LOGD("hb: L%s R%s head yaw %.1f deg | Ltrig %.2f Rtrig %.2f | thumbs (%.2f,%.2f) (%.2f,%.2f) | XInputGetState calls %ld, modified %ld, fabricated %ld",
                     in.controllerConnected[0] ? "+" : "-", in.controllerConnected[1] ? "+" : "-", Rad2Deg(tr.head.rot.Yaw()),
                     in.indexTrigger[0], in.indexTrigger[1], in.thumb[0][0], in.thumb[0][1], in.thumb[1][0], in.thumb[1][1], calls, merged, fab);
            else LOGD("hb: waiting for VR runtime (%s)", ActiveBackend() ? ActiveBackend()->Name() : "none attached");
            GameERStatus gs = GameERGetStatus();
            LOGD("hb: game: version %d camHook %d skelHook %d player %d bones %d arms %d | 5s: camStep %ld calls %.1f ms, hk %ld calls / %ld solves %.1f ms",
                 gs.versionOk, gs.cameraHooked, gs.skeletonHooked, gs.playerFound, gs.boneCount, gs.armsActive, gs.camCalls, gs.camMs, gs.hkCalls, gs.hkSolves, gs.hkMs);
            ReShadeStats rs = ReShadeAddonGetStats();
            StereoStats ss = StereoGetStats();
            static double s_pumpT = 0;
            const double nowT = Now();
            const long pumpN = FramePumpTakePasses();
            const double pumpHz = (s_pumpT > 0 && nowT > s_pumpT) ? pumpN / (nowT - s_pumpT) : 0;
            s_pumpT = nowT;
            // IGT rate: in-game ms per real ms. 1.00 = the sim advances at exactly realtime;
            // ~2.0 = pass 2 is advancing the sim too (the dt swap is not reaching it);
            // below 1.0 = the loop is under 60 pairs/s and the game is in slow motion.
            static double s_igtT = 0; static uint32_t s_igtMs = 0;
            double igtRate = 0;
            { const uint32_t ig = GameERIgtMs();
              if (ig && s_igtMs && ig > s_igtMs && nowT > s_igtT)
                  igtRate = (double)(ig - s_igtMs) / 1000.0 / (nowT - s_igtT);
              s_igtT = nowT; s_igtMs = ig; }
            LOGD("hb: xr: addon %d d3d12 %d bound %d | 5s: presents %ld, submitted %ld (stereo %ld), submitFails %ld, blitFails %ld | back buffer %ux%u fmt %u | crop %.0f%%x%.0f%% | submit %.2f ms | pump: %.0f Hz, %.1f ms, sync %.1f ms | igt x%.2f",
                 rs.registered, rs.d3d12, ss.graphicsBound, rs.presents, ss.frames, ss.stereoFrames, ss.submitFails, ss.blitFails,
                 rs.width, rs.height, (unsigned)rs.format, ss.cropU * 100.f, ss.cropV * 100.f, ss.lastSubmitMs,
                 pumpHz, FramePumpLastMs(), FramePumpGpuSyncLastMs(), igtRate);
            if (ss.eyeBlitL || ss.eyeBlitR || ss.eyeDesync)
                LOGD("hb: eyes: L %ld / R %ld blits, desync %ld", ss.eyeBlitL, ss.eyeBlitR, ss.eyeDesync);
            // The engine's sim step is a FIXED 1/60 s, so a loop below 60 Hz is not just
            // lower fps - the whole game runs at loopHz/60 speed. Under stereo the loop is
            // usually GPU-bound; the fix is lowering the game's resolution/settings.
            if (pumpHz > 5.0 && pumpHz < 54.0)
                LOGW("[stereo] loop at %.0f Hz -> GAME SPEED %.0f%%. GPU cannot hold 60 pairs/s; "
                     "lower the game resolution or graphics settings", pumpHz, pumpHz / 60.0 * 100.0);
            CombatStatus cs = CombatGetStatus();
            LOGD("hb: combat: taeHook %d physical %d learned L%d H%d Left%d stamina %d | 5s: swings %ld, injected %ld",
                 cs.taeHooked, cs.physical, cs.learnedLight, cs.learnedHeavy, cs.learnedLeft, cs.stamina, cs.swings, cs.injected);
        }
        double spent = Now() - t;
        DWORD sleepMs = (DWORD)((period - spent) * 1000.0);
        Sleep(sleepMs > 0 && sleepMs < 100 ? sleepMs : 1);
    }
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_self = hModule;
        DisableThreadLibraryCalls(hModule);
        g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        g_quit.store(true);
        // Do not wait on the worker or unhook here: process is exiting and other
        // modules (LibOVR, XInput) may already be gone. MinHook is process-scoped.
        ervr::LogShutdown();
        break;
    }
    return TRUE;
}
