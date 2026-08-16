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

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        LOGE("MinHook init failed"); return 0;
    }
    MidHookSelfTest();
    BackendsInit(Cfg().vrRuntime.c_str());
    VirtualPadInit();
    if (Cfg().fpEnabled) GameERInit();

    FILETIME iniTime = IniWriteTime(ini);
    bool xinputHooked = false;
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
        }
        // Hot-reload the ini (any key except VRRuntime/LogLevel/PollHz takes effect immediately).
        if (t - lastIniCheck > 1.0) {
            lastIniCheck = t;
            FILETIME ft = IniWriteTime(ini);
            if (CompareFileTime(&ft, &iniTime) != 0) {
                iniTime = ft;
                Cfg().Load(ini);
                VirtualPadInit();
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
            LOGD("hb: game: version %d camHook %d skelHook %d player %d bones %d arms %d", gs.versionOk, gs.cameraHooked, gs.skeletonHooked, gs.playerFound, gs.boneCount, gs.armsActive);
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
