#include "autopilot.h"
#include "config.h"
#include "game_er.h"
#include "log.h"
#include "reshade_addon.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <string>

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
    if (n == "UP") return XINPUT_GAMEPAD_DPAD_UP;
    if (n == "DOWN") return XINPUT_GAMEPAD_DPAD_DOWN;
    if (n == "LEFT") return XINPUT_GAMEPAD_DPAD_LEFT;
    if (n == "RIGHT") return XINPUT_GAMEPAD_DPAD_RIGHT;
    if (n == "LB") return XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (n == "RB") return XINPUT_GAMEPAD_RIGHT_SHOULDER;
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
        // Menu probe: open and close a full-screen menu on a slow cycle. A menu is drawn by
        // a different part of the renderer than the in-world HUD, so "does the HUD layer see
        // it" has to be asked separately - and this is the only way to ask it unattended.
        if (c.autoPilotMenuSeconds > 0) {
            static std::atomic<double> s_m0{0};
            double m0 = s_m0.load(std::memory_order_relaxed);
            if (m0 == 0.0) {
                s_m0.store(now); m0 = now;
                LOGI("[autopilot] player active; tapping %s every %.1f s for %d s (menu probe)",
                     c.autoPilotMenuButton.c_str(), c.autoPilotMenuPeriod, c.autoPilotMenuSeconds);
            }
            const double m = now - m0;
            if (m < c.autoPilotMenuSeconds) {
                // A short tap at the start of each period: held long enough for the game to
                // see the edge, released long enough that the menu stays open in between.
                const double period = c.autoPilotMenuPeriod > 0.5 ? c.autoPilotMenuPeriod : 6.0;
                const double phase = std::fmod(m, period);
                if (phase < 0.15) {
                    pad.wButtons |= ButtonByName(c.autoPilotMenuButton);
                    return true;
                }
                return false;   // still inside the probe window: hold the input released
            }
        }
        // The script: tap a button, wait, tap the next (a menu walked unattended, e.g. to the ERVR tab).
        if (!c.autoPilotScript.empty()) {
            static std::atomic<double> s_s0{0};
            static std::atomic<int> s_step{-1};
            double s0 = s_s0.load(std::memory_order_relaxed);
            if (s0 == 0.0) {
                s_s0.store(now); s0 = now;
                LOGI("[autopilot] player active; script: %s", c.autoPilotScript.c_str());
            }
            double at = s0;
            int index = 0;
            size_t i = 0;
            const std::string& s = c.autoPilotScript;
            while (i < s.size()) {
                while (i < s.size() && (s[i] == ' ' || s[i] == ',')) ++i;
                size_t j = i;
                while (j < s.size() && s[j] != ' ' && s[j] != ',') ++j;
                if (j == i) break;
                const std::string step = s.substr(i, j - i);
                i = j;
                const size_t plus = step.find('+');
                const std::string name = step.substr(0, plus);
                const double wait = plus == std::string::npos ? 1.0 : atof(step.c_str() + plus + 1);
                if (now < at + wait) {
                    // Sticks are held for the whole step (LU/LD/LL/LR walk, RL/RR turn the camera), so the
                    // script can walk up to a prompt; every other step is a 0.12 s tap.
                    const bool stick = name.size() == 2 && (name[0] == 'L' || name[0] == 'R') &&
                                       (name[1] == 'U' || name[1] == 'D' || name[1] == 'L' || name[1] == 'R') &&
                                       !(name[0] == 'L' && name[1] == 'B') && !(name[0] == 'R' && name[1] == 'B');
                    if (stick) {
                        if (s_step.exchange(index) != index)
                            LOGI("[autopilot] script step %d: %s for %.1f s", index + 1, name.c_str(), wait);
                        const SHORT full = 32767;
                        SHORT x = 0, y = 0;
                        if (name[1] == 'U') y = full; else if (name[1] == 'D') y = -full;
                        else if (name[1] == 'L') x = -full; else x = full;
                        if (name[0] == 'L') { pad.sThumbLX = x; pad.sThumbLY = y; }
                        else { pad.sThumbRX = x; pad.sThumbRY = y; }
                        return true;
                    }
                    if (name == "WAIT" || now - at > 0.12) return false;
                    if (s_step.exchange(index) != index) {
                        LOGI("[autopilot] script step %d: %s", index + 1, name.c_str());
                        if (name == "SHOT") ReShadeAddonRequestScreenshot();   // ReShade saves the next frame
                    }
                    if (name == "SHOT") return false;
                    if (name == "LT") pad.bLeftTrigger = 255;
                    else if (name == "RT") pad.bRightTrigger = 255;
                    else pad.wButtons |= ButtonByName(name);
                    return true;
                }
                at += wait;
                ++index;
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
