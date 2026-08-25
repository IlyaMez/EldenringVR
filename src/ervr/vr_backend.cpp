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
// 2026-08-25, Quest Pro on VirtualDesktopXR 1.0.10: the runtime hands the LEFT thumbstick a
// corrupted stream - full-deflection samples jumping 30..180 degrees between 4 ms polls
// (both directions, broad angle spread, only ~1/3 matching an earlier sample - so neither a
// fixed axis transform nor pure packet reordering), plus full-deflection spikes while the
// stick is RELEASED. The Quest 3 sessions the same day are clean, and the right stick never
// glitches, so it is the left controller's input path, upstream of every ERVR transform.
// Symptoms: zig-zag "stuck on debris" movement, and a lurch ("pulled back-left") when
// standing still. The "[xr] raw" log lines (written before this filter) show the raw stream.
//
// Physicality filter, run at the poll rate (250 Hz): the output is what a real thumb could
// have done, as close to the raw sample as that allows.
//   - Deflection magnitude may FALL instantly (a release) but may only RISE at
//     GlitchRisePerSec (25/s = idle to full in 40 ms, as fast as a real slam). A garbage
//     full-deflection spike from idle comes out at ~0.1 - under the deadzone - instead of
//     as a one-frame lurch, and the instant the raw stream returns to idle so does the output.
//   - Above half deflection the direction SLEWS toward the raw direction at
//     GlitchSweepDegPerSec (720 deg/s - faster than rolling the stick around the rim). The
//     per-poll garbage jumps (90 deg in 4 ms = 22000 deg/s) collapse to a +-3 degree wobble
//     around the real direction; a genuine fast rim-roll is followed with ~50-100 ms of lag.
//     No hold, no timeout: the output always converges on the raw stream, so a wrong guess
//     costs lag, never a dead stick.
struct StickFilter { float ax = 0, ay = 0; double last = 0; long adjusted = 0; };
StickFilter g_stick[2];
double g_stickReportAt = 0;

void FilterStickGlitches(VRInputState& in, double now) {
    const Config& c = Cfg();
    if (!c.stickGlitchFilter) return;
    for (int h = 0; h < 2; ++h) {
        StickFilter& f = g_stick[h];
        const float rx = in.thumb[h][0], ry = in.thumb[h][1];
        const float m = std::sqrt(rx * rx + ry * ry), am = std::sqrt(f.ax * f.ax + f.ay * f.ay);
        float dt = (float)(now - f.last); f.last = now;
        if (dt <= 0.f || dt > 0.05f) dt = 0.004f;   // first call / hitch: assume one 250 Hz poll
        float outMag = m;
        if (m > am) outMag = std::min(m, am + c.stickGlitchRisePerSec * dt);
        float x, y;
        if (m > 0.5f && am > 0.5f) {
            const float cur = std::atan2(f.ay, f.ax), tgt = std::atan2(ry, rx);
            float d = tgt - cur;
            while (d > 3.14159265f) d -= 6.2831853f;
            while (d < -3.14159265f) d += 6.2831853f;
            const float maxStep = c.stickGlitchSweepDegPerSec * 0.017453292f * dt;
            if (d > maxStep) d = maxStep; else if (d < -maxStep) d = -maxStep;
            const float na = cur + d;
            x = std::cos(na) * outMag; y = std::sin(na) * outMag;
        } else if (m > 1e-6f) {
            x = rx / m * outMag; y = ry / m * outMag;
        } else {
            x = y = 0.f;
        }
        if (std::fabs(x - rx) > 0.10f || std::fabs(y - ry) > 0.10f) {
            ++f.adjusted;
            if (f.adjusted == 1) LOGD("[stick] %s stick (%.2f,%.2f) faster than a thumb; reshaped to (%.2f,%.2f)", h ? "right" : "left", rx, ry, x, y);
        }
        in.thumb[h][0] = f.ax = x;
        in.thumb[h][1] = f.ay = y;
    }
    if (now - g_stickReportAt >= 5.0) {
        g_stickReportAt = now;
        if (g_stick[0].adjusted || g_stick[1].adjusted) {
            LOGI("[stick] glitch filter reshaped %ld left / %ld right implausible samples (5 s)", g_stick[0].adjusted, g_stick[1].adjusted);
            g_stick[0].adjusted = g_stick[1].adjusted = 0;
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
