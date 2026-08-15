// OpenVR (SteamVR) backend.
//
// When R.E.A.L. VR is configured for SteamVR it loads openvr_api.dll into the
// game process and initialises it as a scene application. openvr_api keeps a
// single process-wide runtime context, so we simply ask it for IVRSystem via
// VR_GetGenericInterface. If it is not initialised yet (or R.E.A.L. is on the
// Oculus path and openvr_api is unused) we fall back to initialising it
// ourselves as a *background* application, which is allowed alongside a scene
// app and is enough for poses + legacy controller state.
#define OPENVR_BUILD_STATIC 1
#include <openvr.h>
#include "vr_backend.h"
#include "log.h"
#include <windows.h>
#include <cmath>

// openvr.h's inline COpenVRContext references these two C symbols. We are not
// linking openvr_api.lib (we bind at runtime to whatever openvr_api.dll R.E.A.L.
// loaded), so provide forwarding definitions.
namespace {
FARPROC OpenVRExport(const char* name) {
    HMODULE m = GetModuleHandleW(L"openvr_api.dll");
    return m ? GetProcAddress(m, name) : nullptr;
}
}
extern "C" void* VR_GetGenericInterface(const char* iface, vr::EVRInitError* err) {
    auto f = (void* (*)(const char*, vr::EVRInitError*))OpenVRExport("VR_GetGenericInterface");
    if (!f) { if (err) *err = vr::VRInitError_Init_InterfaceNotFound; return nullptr; }
    return f(iface, err);
}
extern "C" uint32_t VR_GetInitToken() {
    auto f = (uint32_t (*)())OpenVRExport("VR_GetInitToken");
    return f ? f() : 0;
}

namespace ervr {
namespace {

using PFN_VR_InitInternal2 = uint32_t (*)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
using PFN_VR_GetGenericInterface = void* (*)(const char*, vr::EVRInitError*);
using PFN_VR_IsHmdPresent = bool (*)();
using PFN_VR_GetInitToken = uint32_t (*)();

Pose FromMat(const vr::HmdMatrix34_t& m, const vr::HmdVector3_t& v, const vr::HmdVector3_t& w, bool valid) {
    Pose p;
    p.pos = {m.m[0][3], m.m[1][3], m.m[2][3]};
    // rotation matrix -> quaternion
    float tr = m.m[0][0] + m.m[1][1] + m.m[2][2];
    Quat q;
    if (tr > 0) {
        float s = std::sqrt(tr + 1.f) * 2.f;
        q.w = 0.25f * s; q.x = (m.m[2][1] - m.m[1][2]) / s; q.y = (m.m[0][2] - m.m[2][0]) / s; q.z = (m.m[1][0] - m.m[0][1]) / s;
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        float s = std::sqrt(1.f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.f;
        q.w = (m.m[2][1] - m.m[1][2]) / s; q.x = 0.25f * s; q.y = (m.m[0][1] + m.m[1][0]) / s; q.z = (m.m[0][2] + m.m[2][0]) / s;
    } else if (m.m[1][1] > m.m[2][2]) {
        float s = std::sqrt(1.f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.f;
        q.w = (m.m[0][2] - m.m[2][0]) / s; q.x = (m.m[0][1] + m.m[1][0]) / s; q.y = 0.25f * s; q.z = (m.m[1][2] + m.m[2][1]) / s;
    } else {
        float s = std::sqrt(1.f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.f;
        q.w = (m.m[1][0] - m.m[0][1]) / s; q.x = (m.m[0][2] + m.m[2][0]) / s; q.y = (m.m[1][2] + m.m[2][1]) / s; q.z = 0.25f * s;
    }
    p.rot = q;
    p.linVel = {v.v[0], v.v[1], v.v[2]};
    p.angVel = {w.v[0], w.v[1], w.v[2]};
    p.valid = valid;
    return p;
}

class OpenVRBackend final : public IVRBackend {
    HMODULE m_api = nullptr;
    vr::IVRSystem* m_sys = nullptr;
    PFN_VR_InitInternal2 m_init = nullptr;
    PFN_VR_GetGenericInterface m_getIf = nullptr;
    PFN_VR_GetInitToken m_getToken = nullptr;
    bool m_weInitialised = false;
    double m_lastAttempt = 0;

public:
    const char* Name() const override { return "openvr"; }

    bool TryAttach() override {
        if (!m_api) {
            m_api = GetModuleHandleW(L"openvr_api.dll");
            if (!m_api) return false;   // R.E.A.L. hasn't loaded it (Oculus mode) -> not our runtime
            m_init = (PFN_VR_InitInternal2)GetProcAddress(m_api, "VR_InitInternal2");
            m_getIf = (PFN_VR_GetGenericInterface)GetProcAddress(m_api, "VR_GetGenericInterface");
            m_getToken = (PFN_VR_GetInitToken)GetProcAddress(m_api, "VR_GetInitToken");
            if (!m_getIf) { LOGW("[openvr] openvr_api.dll lacks VR_GetGenericInterface"); m_api = nullptr; return false; }
            LOGI("[openvr] openvr_api.dll found at %p", (void*)m_api);
        }
        vr::EVRInitError err = vr::VRInitError_None;
        // 1) already initialised in-process by R.E.A.L.?
        uint32_t token = m_getToken ? m_getToken() : 0;
        if (token != 0) {
            m_sys = (vr::IVRSystem*)m_getIf(vr::IVRSystem_Version, &err);
            if (m_sys && err == vr::VRInitError_None) { LOGI("[openvr] Attached to R.E.A.L.'s OpenVR context (token %u)", token); return true; }
        }
        // 2) otherwise init as a background app ourselves. Rate limited (every 5 s) and only after a
        //    grace period: R.E.A.L. hooks LoadLibrary and refuses vrclient loads from foreign threads
        //    during its own startup, and on the Oculus/Virtual Desktop path this will never succeed.
        double nowS = (double)GetTickCount64() / 1000.0;
        if (m_lastAttempt == 0) m_lastAttempt = nowS + 10.0;      // first try ~15 s after load
        if (m_init && nowS - m_lastAttempt >= 5.0) {
            m_lastAttempt = nowS;
            m_init(&err, vr::VRApplication_Background, nullptr);
            if (err == vr::VRInitError_None) {
                m_sys = (vr::IVRSystem*)m_getIf(vr::IVRSystem_Version, &err);
                if (m_sys && err == vr::VRInitError_None) { m_weInitialised = true; LOGI("[openvr] Initialised as background application"); return true; }
            }
            LOGD("[openvr] VR_InitInternal2(Background) -> %d", (int)err);
        }
        return false;
    }

    bool IsReady() const override { return m_sys != nullptr; }

    bool Poll(VRTracking& tr, VRInputState& in) override {
        if (!m_sys) return false;
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
        m_sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.f, poses, vr::k_unMaxTrackedDeviceCount);
        LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
        tr.time = (double)c.QuadPart / (double)f.QuadPart;
        const auto& hp = poses[vr::k_unTrackedDeviceIndex_Hmd];
        tr.head = FromMat(hp.mDeviceToAbsoluteTracking, hp.vVelocity, hp.vAngularVelocity, hp.bPoseIsValid);

        in.buttons = 0;
        for (int h = 0; h < 2; ++h) {
            vr::TrackedDeviceIndex_t idx = m_sys->GetTrackedDeviceIndexForControllerRole(
                h == HandLeft ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
            in.controllerConnected[h] = idx != vr::k_unTrackedDeviceIndexInvalid;
            tr.hand[h] = Pose{};
            if (!in.controllerConnected[h]) continue;
            const auto& p = poses[idx];
            tr.hand[h] = FromMat(p.mDeviceToAbsoluteTracking, p.vVelocity, p.vAngularVelocity, p.bPoseIsValid);

            vr::VRControllerState_t st{};
            if (!m_sys->GetControllerState(idx, &st, sizeof(st))) continue;
            // Legacy axis layout for Touch / Index / WMR: axis0 = joystick/trackpad, axis1 = trigger, axis2 = grip.
            in.thumb[h][0] = st.rAxis[0].x; in.thumb[h][1] = st.rAxis[0].y;
            in.indexTrigger[h] = st.rAxis[1].x;
            in.handTrigger[h]  = st.rAxis[2].x;
            uint64_t bp = st.ulButtonPressed;
            auto down = [&](vr::EVRButtonId b) { return (bp & vr::ButtonMaskFromId(b)) != 0; };
            if (down(vr::k_EButton_Grip) && in.handTrigger[h] < 0.01f) in.handTrigger[h] = 1.f;
            if (down(vr::k_EButton_SteamVR_Trigger) && in.indexTrigger[h] < 0.01f) in.indexTrigger[h] = 1.f;
            if (h == HandRight) {
                if (down(vr::k_EButton_A)) in.buttons |= VRB_A;
                if (down(vr::k_EButton_ApplicationMenu)) in.buttons |= VRB_B;   // Touch "B" is reported as ApplicationMenu on the right
                if (down(vr::k_EButton_SteamVR_Touchpad)) in.buttons |= VRB_RThumb;
            } else {
                if (down(vr::k_EButton_A)) in.buttons |= VRB_X;
                if (down(vr::k_EButton_ApplicationMenu)) in.buttons |= VRB_Y;
                if (down(vr::k_EButton_SteamVR_Touchpad)) in.buttons |= VRB_LThumb;
                if (down(vr::k_EButton_System)) in.buttons |= VRB_Menu;        // usually reserved by SteamVR; Y long-press substitutes (see virtual_pad)
            }
        }
        return true;
    }

    void Haptic(Hand hand, float amplitude, float durationSec) override {
        if (!m_sys || amplitude <= 0.f) return;
        vr::TrackedDeviceIndex_t idx = m_sys->GetTrackedDeviceIndexForControllerRole(
            hand == HandLeft ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
        if (idx == vr::k_unTrackedDeviceIndexInvalid) return;
        // Legacy haptics: single pulse up to 3999 us; scale by amplitude. Caller repeats for longer durations.
        unsigned short us = (unsigned short)Clamp(amplitude * 3999.f, 0.f, 3999.f);
        (void)durationSec;
        m_sys->TriggerHapticPulse(idx, 0, us);
    }

    void Detach() override {
        // Never call VR_Shutdown: R.E.A.L. owns the context. Background init we did is process-scoped anyway.
        m_sys = nullptr;
    }
};

} // namespace

IVRBackend* CreateOpenVRBackend() { static OpenVRBackend b; return &b; }
}
