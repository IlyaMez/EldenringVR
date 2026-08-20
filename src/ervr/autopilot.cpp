#include "autopilot.h"
#include "config.h"
#include "game_er.h"
#include "log.h"
#include <atomic>
#include <cmath>

namespace ervr {
namespace {

std::atomic<double> g_t0{0};        // seconds, from the first merge after init
std::atomic<bool>   g_announced{false}, g_finished{false};

double NowSec() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

WORD ButtonByName(const std::string& n) {
    if (n == "A") return XINPUT_GAMEPAD_A;
    if (n == "B") return XINPUT_GAMEPAD_B;
    if (n == "X") return XINPUT_GAMEPAD_X;
    if (n == "Y") return XINPUT_GAMEPAD_Y;
    if (n == "START") return XINPUT_GAMEPAD_START;
    if (n == "BACK") return XINPUT_GAMEPAD_BACK;
    return XINPUT_GAMEPAD_A;
}

} // namespace

bool AutoPilotActive() {
    return Cfg().autoPilotSeconds > 0 && !g_finished.load(std::memory_order_relaxed);
}

void AutoPilotInit() {
    g_t0.store(0);
    g_announced.store(false);
    g_finished.store(false);
}

bool AutoPilotMerge(XINPUT_GAMEPAD& pad) {
    const Config& c = Cfg();
    // Logged before any early-out so the log distinguishes "the game never polled XInput"
    // from "it polled and the autopilot was switched off".
    static std::atomic<bool> s_sawPoll{false};
    if (!s_sawPoll.exchange(true))
        LOGI("[autopilot] first XInput poll seen (AutoPilotSeconds=%d delay=%.1f button=%s)",
             c.autoPilotSeconds, c.autoPilotDelay, c.autoPilotButton.c_str());
    if (c.autoPilotSeconds <= 0) return false;
    if (g_finished.load(std::memory_order_relaxed)) return false;

    const double now = NowSec();
    double t0 = g_t0.load(std::memory_order_relaxed);
    if (t0 == 0.0) { g_t0.store(now); t0 = now; }
    const double elapsed = now - t0;

    // Poll-rate telemetry: ELDEN RING may poll an empty controller slot rarely, and "the
    // autopilot never pressed anything" has to be distinguishable from "it pressed and the
    // game ignored it".
    static std::atomic<long> s_polls{0};
    static std::atomic<double> s_lastReport{0};
    const long n = s_polls.fetch_add(1) + 1;
    if (now - s_lastReport.load() > 2.0) {
        s_lastReport.store(now);
        LOGI("[autopilot] %ld polls, elapsed %.1f s (press window %.1f..%.1f)",
             n, elapsed, (double)c.autoPilotDelay, c.autoPilotDelay + (double)c.autoPilotSeconds);
    }

    if (elapsed < c.autoPilotDelay) return false;
    if (!g_announced.exchange(true))
        LOGI("[autopilot] pressing %s every %.2f s for %.0f s (unattended test run)",
             c.autoPilotButton.c_str(), c.autoPilotPeriod, (double)c.autoPilotSeconds);

    // Stop the moment the player exists. Its whole job is to walk the title and save menus;
    // once we are in the world, more A presses would be interacting with the user's game -
    // talking to NPCs, using a grace, picking things up - which an unattended test must not do.
    // Exception: WiggleSeconds keeps ONLY the camera stick turning after spawn - it cannot
    // interact with anything, and a continuously panning view is what lets screenshots tell
    // "scene frozen" from "scene rendering" during the M2 gating experiments.
    if (GameERPlayerActive()) {
        if (c.autoPilotWiggleSeconds > 0) {
            static std::atomic<double> s_w0{0};
            double w0 = s_w0.load(std::memory_order_relaxed);
            if (w0 == 0.0) {
                s_w0.store(now); w0 = now;
                LOGI("[autopilot] player active; panning camera for %d s (WiggleSeconds)",
                     c.autoPilotWiggleSeconds);
            }
            if (now - w0 < c.autoPilotWiggleSeconds) {
                pad.sThumbRX = 11000;   // slow steady yaw, nothing else
                return true;
            }
        }
        if (!g_finished.exchange(true)) LOGI("[autopilot] player is in the world; input released");
        return false;
    }
    if (elapsed > c.autoPilotDelay + c.autoPilotSeconds) {
        if (!g_finished.exchange(true)) LOGI("[autopilot] time limit reached; input released");
        return false;
    }

    // Square wave: held for the first half of each period, released for the second. ELDEN
    // RING's menus need to SEE the release, so a permanently-held button advances nothing.
    const double period = c.autoPilotPeriod > 0.05 ? c.autoPilotPeriod : 0.5;
    const double phase = elapsed - c.autoPilotDelay;
    const bool down = std::fmod(phase, period) < period * 0.5;
    if (!down) return false;
    pad.wButtons |= ButtonByName(c.autoPilotButton);
    return true;
}
}
