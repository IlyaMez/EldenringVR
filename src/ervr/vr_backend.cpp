#include "vr_backend.h"
#include "log.h"
#include <windows.h>
#include <mutex>
#include <string>
#include <vector>

namespace ervr {
namespace {
std::vector<IVRBackend*> g_candidates;
IVRBackend* g_active = nullptr;
std::mutex g_snapMtx;
VRTracking g_snapTracking;
VRInputState g_snapInput;
bool g_snapValid = false;
int g_failStreak = 0;
}

IVRBackend* ActiveBackend() { return g_active; }

void OculusSetHostHeadPositionHidden(bool hidden);   // vr_oculus.cpp
bool OculusHooksInstalled();
void VRSetHostHeadPositionHidden(bool hidden) { OculusSetHostHeadPositionHidden(hidden); }
bool VRHostHeadPositionHideSupported() { return g_active && g_active == CreateOculusBackend() && OculusHooksInstalled(); }

void BackendsInit(const char* pref) {
    std::string p = pref ? pref : "auto";
    for (auto& c : p) c = (char)tolower((unsigned char)c);
    g_candidates.clear();
    if (p == "oculus")      g_candidates = { CreateOculusBackend() };
    else if (p == "openvr") g_candidates = { CreateOpenVRBackend() };
    else                    g_candidates = { CreateOculusBackend(), CreateOpenVRBackend() };
    LOGI("VR runtime preference: %s (%zu candidate backend(s))", p.c_str(), g_candidates.size());
}

void BackendsTick() {
    if (!g_active) {
        for (auto* b : g_candidates) {
            if (b->TryAttach()) { g_active = b; LOGI("Active VR backend: %s", b->Name()); break; }
        }
        if (!g_active) return;
    }
    VRTracking tr; VRInputState in;
    if (g_active->Poll(tr, in)) {
        g_failStreak = 0;
        std::lock_guard<std::mutex> g(g_snapMtx);
        g_snapTracking = tr; g_snapInput = in; g_snapValid = true;
    } else {
        if (++g_failStreak == 200) LOGW("VR backend %s stopped returning data; will keep retrying", g_active->Name());
        if (!g_active->IsReady()) {
            LOGI("VR backend %s lost its runtime; re-scanning", g_active->Name());
            g_active = nullptr;
            std::lock_guard<std::mutex> g(g_snapMtx);
            g_snapValid = false;
        }
    }
}

void BackendsShutdown() {
    if (g_active) g_active->Detach();
    g_active = nullptr;
}

bool GetLatestVRState(VRTracking& tracking, VRInputState& input) {
    std::lock_guard<std::mutex> g(g_snapMtx);
    if (!g_snapValid) return false;
    tracking = g_snapTracking; input = g_snapInput;
    return true;
}
}
