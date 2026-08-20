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

void BackendsInit(const char* pref) {
    std::string p = pref ? pref : "openxr";
    for (auto& c : p) c = (char)tolower((unsigned char)c);
    if (p != "openxr" && p != "auto")
        LOGW("VRRuntime=%s is no longer supported (ERVR owns its own OpenXR session); using openxr", p.c_str());
    g_candidates = { CreateOpenXRBackend() };
    LOGI("VR runtime: openxr");
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
