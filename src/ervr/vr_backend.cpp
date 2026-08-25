#include "vr_backend.h"
#include "config.h"
#include "log.h"
#include <windows.h>
#include <chrono>
#include <cmath>
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

double SteadySec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---- stick glitch filter --------------------------------------------------------------
// 2026-08-25, Quest Pro on VirtualDesktopXR 1.0.10: the runtime handed the LEFT thumbstick
// rotated by exactly 90 degrees - (x,y) -> (-y,x), same magnitude - on ~10% of polls,
// alternating frame to frame for anything from one poll to ~0.5 s (161 flips in one 8-minute
// session, 0 in the Quest 3 session the same morning). A forward push rotated 90 degrees IS a
// back-left vector: the character zig-zagged and caught on every step edge, and lurched
// back-left on stopping. The "[xr] raw" log lines flip the same way, so it is upstream of
// every ERVR transform (with Forward=head the stick rotation is identically zero anyway).
//
// This runs at the poll rate (250 Hz). A thumb cannot turn a fully deflected stick a quarter
// turn between two 4 ms polls, so a sample that is a same-magnitude ~90 degree rotation of
// the last ACCEPTED sample is held out and the accepted sample repeated. Comparing against
// the accepted sample (not the previous raw one) means a rotated run of any length is bridged,
// and the flip back lands next to the accepted value and is simply accepted. A real stick
// move sweeps through small angles (accepted as it goes) or through the centre (below half
// deflection = always accepted, state reset), so it never trips this. Safety valve: after
// GlitchHoldSeconds of continuous rejection the new position is believed - the cost of a
// wrong guess is bounded, the cost of a stuck stick would not be.
struct StickFilter { float ax = 0, ay = 0; bool held = false; double holdSince = 0; long rejected = 0; };
StickFilter g_stick[2];
double g_stickReportAt = 0;

void FilterStickGlitches(VRInputState& in, double now) {
    const Config& c = Cfg();
    if (!c.stickGlitchFilter) return;
    for (int h = 0; h < 2; ++h) {
        StickFilter& f = g_stick[h];
        const float x = in.thumb[h][0], y = in.thumb[h][1];
        const float m = std::sqrt(x * x + y * y), am = std::sqrt(f.ax * f.ax + f.ay * f.ay);
        bool rotated = false;
        if (m > 0.5f && am > 0.5f) {
            const float ratio = m / am;
            const float cosA = (x * f.ax + y * f.ay) / (m * am);   // |cos| < cos(75 deg): 75..105 degrees apart
            rotated = ratio > 0.8f && ratio < 1.25f && std::fabs(cosA) < 0.2588f;
        }
        if (rotated) {
            if (!f.held) {
                f.held = true; f.holdSince = now;
                if (f.rejected == 0) LOGD("[stick] %s stick sample (%.2f,%.2f) is a ~90 deg rotation of the accepted (%.2f,%.2f): holding", h ? "right" : "left", x, y, f.ax, f.ay);
            }
            if (now - f.holdSince <= c.stickGlitchHoldSeconds) {
                in.thumb[h][0] = f.ax; in.thumb[h][1] = f.ay;
                ++f.rejected;
                continue;
            }
            // Held out for the whole window: this is where the stick really is now.
        }
        f.held = false; f.ax = x; f.ay = y;
    }
    if (now - g_stickReportAt >= 5.0) {
        g_stickReportAt = now;
        if (g_stick[0].rejected || g_stick[1].rejected) {
            LOGI("[stick] glitch filter held %ld left / %ld right rotated samples (5 s)", g_stick[0].rejected, g_stick[1].rejected);
            g_stick[0].rejected = g_stick[1].rejected = 0;
        }
    }
}
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
        FilterStickGlitches(in, SteadySec());
        std::lock_guard<std::mutex> g(g_snapMtx);
        g_snapTracking = tr; g_snapInput = in; g_snapValid = true;
    } else {
        if (++g_failStreak == 200) LOGW("VR backend %s stopped returning data; will keep retrying", g_active->Name());
        // Poll fails whenever the runtime withholds input (session not FOCUSED: a dashboard,
        // an overlay, the headset off). Keeping the previous snapshot here meant whatever was
        // held at that instant - a trigger, a stick push - was re-injected into the game for
        // as long as the overlay stayed up. Tracking is kept (the head is still where it was);
        // the input side is released so nothing stays pressed and the pad reads idle.
        {
            std::lock_guard<std::mutex> g(g_snapMtx);
            g_snapInput = VRInputState{};
        }
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
