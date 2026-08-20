// Oculus (LibOVR) backend.
//
// R.E.A.L. VR talks to the Meta runtime through LibOVRRT64_1.dll and owns the
// ovrSession. LibOVR only allows one session per process, so instead of
// creating our own we hook a couple of per-frame LibOVR exports to *capture*
// the session handle R.E.A.L. passes in, then call ovr_GetTrackingState /
// ovr_GetInputState / ovr_SetControllerVibration on that same session.
//
// Only the tiny slice of the LibOVR ABI we need is declared below (structs are
// laid out exactly like OVR_CAPI.h 1.x; LibOVR's ABI has been frozen since 1.0).
#include "vr_backend.h"
#include "log.h"
#include <windows.h>
#include <psapi.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <cstring>
#include <cmath>

namespace ervr {
namespace {

// ---- LibOVR ABI subset -------------------------------------------------------
using ovrSession = struct ovrHmdStruct*;
using ovrResult = int32_t;
using ovrBool = char;

struct ovrVector2f { float x, y; };
struct ovrVector3f { float x, y, z; };
struct ovrQuatf { float x, y, z, w; };
struct ovrPosef { ovrQuatf Orientation; ovrVector3f Position; };            // 28 bytes

struct alignas(8) ovrPoseStatef {
    ovrPosef    ThePose;
    ovrVector3f AngularVelocity;
    ovrVector3f LinearVelocity;
    ovrVector3f AngularAcceleration;
    ovrVector3f LinearAcceleration;
    float       Pad0;
    double      TimeInSeconds;
};                                                                          // 88 bytes
static_assert(sizeof(ovrPoseStatef) == 88, "ovrPoseStatef ABI mismatch");

struct alignas(8) ovrTrackingState {
    ovrPoseStatef HeadPose;
    unsigned int  StatusFlags;
    ovrPoseStatef HandPoses[2];
    unsigned int  HandStatusFlags[2];
    ovrPosef      CalibratedOrigin;
};

enum { ovrStatus_OrientationTracked = 0x0001, ovrStatus_PositionTracked = 0x0002 };
enum { ovrControllerType_LTouch = 0x0001, ovrControllerType_RTouch = 0x0002, ovrControllerType_Touch = 0x0003 };
enum {
    ovrButton_A = 0x00000001, ovrButton_B = 0x00000002, ovrButton_RThumb = 0x00000004,
    ovrButton_X = 0x00000100, ovrButton_Y = 0x00000200, ovrButton_LThumb = 0x00000400,
    ovrButton_Enter = 0x00100000,
};

struct alignas(8) ovrInputState {
    double       TimeInSeconds;
    unsigned int Buttons;
    unsigned int Touches;
    float        IndexTrigger[2];
    float        HandTrigger[2];
    ovrVector2f  Thumbstick[2];
    int          ControllerType;
    float        IndexTriggerNoDeadzone[2];
    float        HandTriggerNoDeadzone[2];
    ovrVector2f  ThumbstickNoDeadzone[2];
    float        IndexTriggerRaw[2];
    float        HandTriggerRaw[2];
    ovrVector2f  ThumbstickRaw[2];
};

struct ovrSessionStatus {
    ovrBool IsVisible, HmdPresent, HmdMounted, DisplayLost, ShouldQuit, ShouldRecenter, HasInputFocus, OverlayPresent, DepthRequested;
};

// x64: ovrTrackingState is returned by value via a hidden pointer (first arg).
using PFN_ovr_GetTrackingState = ovrTrackingState* (__cdecl*)(ovrTrackingState* out, ovrSession, double absTime, ovrBool latencyMarker);
using PFN_ovr_GetPredictedDisplayTime = double (__cdecl*)(ovrSession, long long frameIndex);
using PFN_ovr_GetTimeInSeconds = double (__cdecl*)();
using PFN_ovr_GetInputState = ovrResult (__cdecl*)(ovrSession, int controllerType, ovrInputState* out);
using PFN_ovr_SetControllerVibration = ovrResult (__cdecl*)(ovrSession, int controllerType, float frequency, float amplitude);
using PFN_ovr_GetSessionStatus = ovrResult (__cdecl*)(ovrSession, ovrSessionStatus* out);
using PFN_ovr_Destroy = void (__cdecl*)(ovrSession);
using PFN_ovr_GetConnectedControllerTypes = unsigned int (__cdecl*)(ovrSession);

// ---- state -------------------------------------------------------------------
HMODULE g_ovr = nullptr;
std::atomic<ovrSession> g_session{nullptr};
PFN_ovr_GetTrackingState        g_GetTrackingState = nullptr;   // original (via trampoline)
PFN_ovr_GetPredictedDisplayTime g_GetPredictedDisplayTime = nullptr;
PFN_ovr_GetSessionStatus        g_GetSessionStatus = nullptr;
PFN_ovr_Destroy                 g_Destroy = nullptr;
PFN_ovr_GetTimeInSeconds        g_GetTimeInSeconds = nullptr;
PFN_ovr_GetInputState           g_GetInputState = nullptr;
PFN_ovr_SetControllerVibration  g_SetControllerVibration = nullptr;
PFN_ovr_GetConnectedControllerTypes g_GetConnectedControllerTypes = nullptr;
bool g_hooked = false;

void NoteSession(ovrSession s) {
    if (s && g_session.load() != s) {
        g_session.store(s);
        LOGI("[oculus] Captured ovrSession %p from R.E.A.L.", (void*)s);
    }
}

std::atomic<bool> g_hideHostHeadPos{false};
ovrTrackingState* __cdecl Hook_GetTrackingState(ovrTrackingState* out, ovrSession s, double t, ovrBool lm) {
    NoteSession(s);
    ovrTrackingState* r = g_GetTrackingState(out, s, t, lm);
    // R.E.A.L. builds its view from this head pose. With the position hidden it contributes rotation only and
    // ERVR is the sole owner of where the eye sits (see VRSetHostHeadPositionHidden).
    if (r && g_hideHostHeadPos.load(std::memory_order_relaxed)) {
        r->HeadPose.ThePose.Position = {0.f, 0.f, 0.f};
        r->HeadPose.LinearVelocity = {0.f, 0.f, 0.f};
        r->HeadPose.LinearAcceleration = {0.f, 0.f, 0.f};
    }
    return r;
}
double __cdecl Hook_GetPredictedDisplayTime(ovrSession s, long long idx) {
    NoteSession(s);
    return g_GetPredictedDisplayTime(s, idx);
}
ovrResult __cdecl Hook_GetSessionStatus(ovrSession s, ovrSessionStatus* out) {
    NoteSession(s);
    return g_GetSessionStatus(s, out);
}
void __cdecl Hook_Destroy(ovrSession s) {
    if (g_session.load() == s) {
        LOGI("[oculus] ovrSession %p destroyed by R.E.A.L.; detaching", (void*)s);
        g_session.store(nullptr);
    }
    g_Destroy(s);
}

template <class T>
bool HookExport(const char* name, void* detour, T& original) {
    void* target = (void*)GetProcAddress(g_ovr, name);
    if (!target) { LOGW("[oculus] export %s missing", name); return false; }
    if (MH_CreateHook(target, detour, reinterpret_cast<void**>(&original)) != MH_OK) { LOGW("[oculus] MH_CreateHook(%s) failed", name); return false; }
    if (MH_EnableHook(target) != MH_OK) { LOGW("[oculus] MH_EnableHook(%s) failed", name); return false; }
    return true;
}

Pose ToPose(const ovrPoseStatef& p, unsigned flags) {
    Pose out;
    out.pos = {p.ThePose.Position.x, p.ThePose.Position.y, p.ThePose.Position.z};
    out.rot = {p.ThePose.Orientation.x, p.ThePose.Orientation.y, p.ThePose.Orientation.z, p.ThePose.Orientation.w};
    out.linVel = {p.LinearVelocity.x, p.LinearVelocity.y, p.LinearVelocity.z};
    out.angVel = {p.AngularVelocity.x, p.AngularVelocity.y, p.AngularVelocity.z};
    out.valid = (flags & ovrStatus_OrientationTracked) != 0;
    return out;
}

class OculusBackend final : public IVRBackend {
public:
    const char* Name() const override { return "oculus"; }

    // Finds whichever module implements the LibOVR ABI. The stock runtime is
    // LibOVRRT64_1.dll, but Virtual Desktop / other emulators load their shim
    // under a different name, so we identify it by its exports.
    static HMODULE FindLibOVR() {
        for (const wchar_t* n : { L"LibOVRRT64_1.dll", L"VirtualDesktop.LibOVRRT64_1.dll" })
            if (HMODULE byName = GetModuleHandleW(n)) return byName;
        HMODULE mods[1024]; DWORD needed = 0;
        if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return nullptr;
        size_t n = std::min<size_t>(needed / sizeof(HMODULE), 1024);
        for (size_t i = 0; i < n; ++i) {
            if (GetProcAddress(mods[i], "ovr_GetTrackingState") && GetProcAddress(mods[i], "ovr_GetInputState") &&
                GetProcAddress(mods[i], "ovr_Create") && mods[i] != GetModuleHandleW(nullptr)) {
                wchar_t path[MAX_PATH]{}; GetModuleFileNameW(mods[i], path, MAX_PATH);
                LOGI("[oculus] LibOVR ABI provided by %S", path);
                return mods[i];
            }
        }
        return nullptr;
    }

    bool TryAttach() override {
        if (!g_ovr) {
            g_ovr = FindLibOVR();
            if (!g_ovr) return false;
            LOGI("[oculus] LibOVR module at %p", (void*)g_ovr);
            g_GetTimeInSeconds = (PFN_ovr_GetTimeInSeconds)GetProcAddress(g_ovr, "ovr_GetTimeInSeconds");
            g_GetInputState = (PFN_ovr_GetInputState)GetProcAddress(g_ovr, "ovr_GetInputState");
            g_SetControllerVibration = (PFN_ovr_SetControllerVibration)GetProcAddress(g_ovr, "ovr_SetControllerVibration");
            g_GetConnectedControllerTypes = (PFN_ovr_GetConnectedControllerTypes)GetProcAddress(g_ovr, "ovr_GetConnectedControllerTypes");
        }
        if (!g_hooked) {
            bool a = HookExport("ovr_GetTrackingState", (void*)&Hook_GetTrackingState, g_GetTrackingState);
            bool b = HookExport("ovr_GetPredictedDisplayTime", (void*)&Hook_GetPredictedDisplayTime, g_GetPredictedDisplayTime);
            bool c = HookExport("ovr_GetSessionStatus", (void*)&Hook_GetSessionStatus, g_GetSessionStatus);
            HookExport("ovr_Destroy", (void*)&Hook_Destroy, g_Destroy);
            g_hooked = a || b || c;
            if (!g_hooked) return false;
            LOGI("[oculus] Hooks installed; waiting for R.E.A.L. to create its session ...");
        }
        return g_session.load() != nullptr && g_GetTrackingState && g_GetInputState;
    }

    bool IsReady() const override { return g_hooked && g_session.load() != nullptr; }

    bool Poll(VRTracking& tr, VRInputState& in) override {
        ovrSession s = g_session.load();
        if (!s) return false;
        double now = g_GetTimeInSeconds ? g_GetTimeInSeconds() : 0.0;
        ovrTrackingState ts{};
        g_GetTrackingState(&ts, s, now, 0);
        tr.time = now;
        tr.head = ToPose(ts.HeadPose, ts.StatusFlags);
        tr.hand[HandLeft]  = ToPose(ts.HandPoses[0], ts.HandStatusFlags[0]);
        tr.hand[HandRight] = ToPose(ts.HandPoses[1], ts.HandStatusFlags[1]);

        ovrInputState is{};
        ovrResult r = g_GetInputState(s, ovrControllerType_Touch, &is);
        static int s_lastLogged = 1;
        if (r < 0) { if (s_lastLogged != r) { s_lastLogged = r; LOGW("[oculus] ovr_GetInputState failed: %d", r); } return false; }
        unsigned connected = g_GetConnectedControllerTypes ? g_GetConnectedControllerTypes(s) : ovrControllerType_Touch;
        if (s_lastLogged != 0) { s_lastLogged = 0; LOGI("[oculus] Input OK; connected controller mask 0x%x (L=%d R=%d), state.ControllerType 0x%x", connected, (connected & 1) != 0, (connected & 2) != 0, is.ControllerType); }
        // Some runtimes/shims (Virtual Desktop) return 0 from ovr_GetConnectedControllerTypes even though
        // input works, so treat a hand as connected if ANY signal says so.
        auto anyInput = [&](int h) {
            return is.IndexTrigger[h] > 0.01f || is.HandTrigger[h] > 0.01f ||
                   std::fabs(is.Thumbstick[h].x) > 0.01f || std::fabs(is.Thumbstick[h].y) > 0.01f;
        };
        const unsigned lBtns = ovrButton_X | ovrButton_Y | ovrButton_LThumb | ovrButton_Enter;
        const unsigned rBtns = ovrButton_A | ovrButton_B | ovrButton_RThumb;
        in.controllerConnected[HandLeft]  = (connected & ovrControllerType_LTouch) || (is.ControllerType & ovrControllerType_LTouch) ||
                                            (ts.HandStatusFlags[0] & ovrStatus_OrientationTracked) || (is.Buttons & lBtns) || anyInput(0);
        in.controllerConnected[HandRight] = (connected & ovrControllerType_RTouch) || (is.ControllerType & ovrControllerType_RTouch) ||
                                            (ts.HandStatusFlags[1] & ovrStatus_OrientationTracked) || (is.Buttons & rBtns) || anyInput(1);
        // Debug: raw dump every ~2 s so we can see what the runtime really delivers.
        static double s_lastDump = 0;
        if (now - s_lastDump > 2.0) {
            s_lastDump = now;
            LOGD("[oculus] raw: conn=0x%x type=0x%x btn=0x%08x touch=0x%08x trig(%.2f,%.2f) grip(%.2f,%.2f) stick L(%.2f,%.2f) R(%.2f,%.2f) handFlags(0x%x,0x%x) headFlags 0x%x handPosR(%.2f,%.2f,%.2f)",
                 connected, is.ControllerType, is.Buttons, is.Touches, is.IndexTrigger[0], is.IndexTrigger[1], is.HandTrigger[0], is.HandTrigger[1],
                 is.Thumbstick[0].x, is.Thumbstick[0].y, is.Thumbstick[1].x, is.Thumbstick[1].y,
                 ts.HandStatusFlags[0], ts.HandStatusFlags[1], ts.StatusFlags,
                 ts.HandPoses[1].ThePose.Position.x, ts.HandPoses[1].ThePose.Position.y, ts.HandPoses[1].ThePose.Position.z);
        }
        uint32_t b = 0;
        if (is.Buttons & ovrButton_A) b |= VRB_A;
        if (is.Buttons & ovrButton_B) b |= VRB_B;
        if (is.Buttons & ovrButton_X) b |= VRB_X;
        if (is.Buttons & ovrButton_Y) b |= VRB_Y;
        if (is.Buttons & ovrButton_LThumb) b |= VRB_LThumb;
        if (is.Buttons & ovrButton_RThumb) b |= VRB_RThumb;
        if (is.Buttons & ovrButton_Enter) b |= VRB_Menu;
        in.buttons = b;
        for (int h = 0; h < 2; ++h) {
            in.indexTrigger[h] = is.IndexTrigger[h];
            in.handTrigger[h] = is.HandTrigger[h];
            in.thumb[h][0] = is.Thumbstick[h].x;
            in.thumb[h][1] = is.Thumbstick[h].y;
        }
        return true;
    }

    void Haptic(Hand hand, float amplitude, float /*durationSec*/) override {
        ovrSession s = g_session.load();
        if (!s || !g_SetControllerVibration) return;
        // LibOVR's simple API is on/off style: frequency 0/0.5/1.0, amplitude 0..1. Caller re-issues 0 to stop.
        g_SetControllerVibration(s, hand == HandLeft ? ovrControllerType_LTouch : ovrControllerType_RTouch,
                                 amplitude > 0.f ? 1.0f : 0.f, Clamp(amplitude, 0.f, 1.f));
    }

    void Detach() override {
        g_session.store(nullptr);
        // Hooks stay installed until MH_Uninitialize at process exit; cheap pass-throughs.
    }
};

} // namespace

IVRBackend* CreateOculusBackend() { static OculusBackend b; return &b; }
void OculusSetHostHeadPositionHidden(bool hidden) { g_hideHostHeadPos.store(hidden); }
bool OculusHooksInstalled() { return g_hooked; }
}
