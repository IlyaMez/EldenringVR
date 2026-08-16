#include "virtual_pad.h"
#include "vr_backend.h"
#include "gestures.h"
#include "config.h"
#include "log.h"
#include "game_memory.h"
#include <mutex>
#include <cmath>
#include <cstring>

namespace ervr {
namespace {

struct Pulse { uint16_t mask = 0; double until = 0; };

struct VPad {
    uint16_t buttons = 0;
    uint8_t  lt = 0, rt = 0;
    int16_t  lx = 0, ly = 0, rx = 0, ry = 0;
    bool     active = false;
};

std::mutex g_mtx;
VPad g_pad;
GestureDetector g_gest;

// Config-derived button masks (resolved once at init / reload).
struct Map {
    uint16_t a, b, x, y, lthumb, rthumb, menu, ltrig, rtrig, lgrip, rgrip, light, heavy, leftSwing;
} g_map;

// Timed pulses (gesture attacks, tap-vs-hold buttons, snap turns).
constexpr int kMaxPulses = 16;
Pulse g_pulses[kMaxPulses];

// Tap/hold trackers.
struct HoldTracker { bool down = false; double since = 0; bool consumed = false; };
HoldTracker g_menuHold, g_lthumbHold, g_yHold;
double g_lastSnap = -1e9;
int g_lastDpadDir = 0;
bool g_snapActive = false;

// Menu navigation mode: left stick = held D-pad, movement zeroed, gestures muted.
bool g_menuMode = false;        // effective state (memory flag if known, else manual/START-driven)
bool g_menuManual = false;      // state driven by START/toggle when the memory flag is unavailable
int  g_menuDpadDir = 0;         // 0 none, 1 up, 2 down, 3 left, 4 right (with hysteresis)
bool g_menuToggleFired = false; // stick-click hold already toggled during this press
int  g_menuLastFlag = -1;       // last value of the memory flag (to notice when it changes)
bool g_menuOverride = false;    // manual toggle currently overriding the memory flag


// Haptic scheduling (worker thread stops vibration after a short duration).
double g_hapticUntil[2] = {0, 0};
float  g_rumble[2] = {0, 0};   // from XInputSetState, 0..1
bool   g_rumbleDirty = false;

int16_t ToStick(float v) { v = Clamp(v, -1.f, 1.f); return (int16_t)(v * 32767.f); }
uint8_t ToTrig(float v) { return (uint8_t)(Clamp(v, 0.f, 1.f) * 255.f); }

void AddPulse(uint16_t mask, double now, double dur) {
    if (!mask) return;
    for (auto& p : g_pulses) if (p.mask == mask) { p.until = now + dur; return; }
    for (auto& p : g_pulses) if (p.until <= now) { p.mask = mask; p.until = now + dur; return; }
}

uint16_t ActivePulses(double now) {
    uint16_t m = 0;
    for (auto& p : g_pulses) if (p.until > now) m |= p.mask;
    return m;
}

void Haptic(Hand h, float amp, double now, double dur) {
    if (auto* b = ActiveBackend()) { b->Haptic(h, amp, (float)dur); g_hapticUntil[h] = now + dur; }
}

void ApplyDeadzone(float& x, float& y, float dz) {
    float m = std::sqrt(x * x + y * y);
    if (m < dz) { x = y = 0.f; return; }
    float scale = (m - dz) / (1.f - dz) / m;   // rescale so output ramps from 0 at the deadzone edge
    x *= scale; y *= scale;
}

} // namespace

void VirtualPadInit() {
    const Config& c = Cfg();
    g_map.a = ParseXInputButton(c.btnA);   g_map.b = ParseXInputButton(c.btnB);
    g_map.x = ParseXInputButton(c.btnX);   g_map.y = ParseXInputButton(c.btnY);
    g_map.lthumb = ParseXInputButton(c.btnLThumb); g_map.rthumb = ParseXInputButton(c.btnRThumb);
    g_map.menu = ParseXInputButton(c.btnMenu);
    g_map.ltrig = ParseXInputButton(c.btnLTrigger); g_map.rtrig = ParseXInputButton(c.btnRTrigger);
    g_map.lgrip = ParseXInputButton(c.btnLGrip);   g_map.rgrip = ParseXInputButton(c.btnRGrip);
    g_map.light = ParseXInputButton(c.lightAttackButton);
    g_map.heavy = ParseXInputButton(c.heavyAttackButton);
    g_map.leftSwing = ParseXInputButton(c.leftSwingButton);
    g_gest.Reset();
    LOGI("Virtual pad map: A=%s B=%s X=%s Y=%s LS=%s RS=%s Menu=%s LTrig=%s RTrig=%s LGrip=%s RGrip=%s | swings: light=%s heavy=%s left=%s",
         XInputButtonName(g_map.a), XInputButtonName(g_map.b), XInputButtonName(g_map.x), XInputButtonName(g_map.y),
         XInputButtonName(g_map.lthumb), XInputButtonName(g_map.rthumb), XInputButtonName(g_map.menu),
         XInputButtonName(g_map.ltrig), XInputButtonName(g_map.rtrig), XInputButtonName(g_map.lgrip), XInputButtonName(g_map.rgrip),
         XInputButtonName(g_map.light), XInputButtonName(g_map.heavy), XInputButtonName(g_map.leftSwing));
}

void VirtualPadUpdate(double now) {
    const Config& c = Cfg();
    VRTracking tr; VRInputState in;
    VPad pad;
    if (!GetLatestVRState(tr, in) || (!in.controllerConnected[0] && !in.controllerConnected[1])) {
        std::lock_guard<std::mutex> g(g_mtx); g_pad = pad; return;
    }
    pad.active = true;
    uint16_t held = 0;   // buttons held as long as the physical control is held
    uint8_t lt = 0, rt = 0;

    auto mapHeld = [&](uint16_t mask, bool down) {
        if (!down || !mask) return;
        if (mask == VP_LT) lt = 255; else if (mask == VP_RT) rt = 255; else held |= mask;
    };

    // ---- face buttons ------------------------------------------------------
    mapHeld(g_map.a, in.buttons & VRB_A);
    mapHeld(g_map.b, in.buttons & VRB_B);
    mapHeld(g_map.x, in.buttons & VRB_X);
    mapHeld(g_map.rthumb, in.buttons & VRB_RThumb);

    // ---- triggers / grips ----------------------------------------------------
    bool lTrig = in.indexTrigger[HandLeft]  >= c.triggerThreshold;
    bool rTrig = in.indexTrigger[HandRight] >= c.triggerThreshold;
    bool lGrip = in.handTrigger[HandLeft]  >= c.gripThreshold;
    bool rGrip = in.handTrigger[HandRight] >= c.gripThreshold;
    mapHeld(g_map.ltrig, lTrig); mapHeld(g_map.rtrig, rTrig);
    mapHeld(g_map.lgrip, lGrip); mapHeld(g_map.rgrip, rGrip);

    // ---- Menu: tap = START, hold = BACK (map) ---------------------------------
    bool menuDown = (in.buttons & VRB_Menu) != 0;
    if (menuDown && !g_menuHold.down) { g_menuHold = {true, now, false}; }
    bool pressedStart = false, pressedBack = false;
    if (menuDown && !g_menuHold.consumed && now - g_menuHold.since >= c.menuHoldForBack) {
        AddPulse(XINPUT_GAMEPAD_BACK, now, c.gesturePressDuration); g_menuHold.consumed = true; pressedBack = true;
    }
    if (!menuDown && g_menuHold.down) {
        if (!g_menuHold.consumed) { AddPulse(g_map.menu, now, c.gesturePressDuration); pressedStart = (g_map.menu == XINPUT_GAMEPAD_START); }
        g_menuHold.down = false;
    }

    // ---- Y: normal Y, plus long-hold = START on runtimes without a Menu button (OpenVR) ---
    bool yDown = (in.buttons & VRB_Y) != 0;
    bool yFallback = ActiveBackend() && std::strcmp(ActiveBackend()->Name(), "openvr") == 0;
    if (!yFallback) mapHeld(g_map.y, yDown);
    else {
        if (yDown && !g_yHold.down) g_yHold = {true, now, false};
        if (yDown && !g_yHold.consumed && now - g_yHold.since >= 0.6) { AddPulse(g_map.menu, now, c.gesturePressDuration); g_yHold.consumed = true; pressedStart = (g_map.menu == XINPUT_GAMEPAD_START); }
        if (!yDown && g_yHold.down) { if (!g_yHold.consumed) AddPulse(g_map.y, now, c.gesturePressDuration); g_yHold.down = false; }
    }

    // ---- sticks -------------------------------------------------------------
    float lx = in.thumb[HandLeft][0],  ly = in.thumb[HandLeft][1];
    float rx = in.thumb[HandRight][0], ry = in.thumb[HandRight][1];
    if (c.swapSticks) { std::swap(lx, rx); std::swap(ly, ry); }
    ApplyDeadzone(lx, ly, c.stickDeadzone);
    ApplyDeadzone(rx, ry, c.stickDeadzone);

    // ---- menu navigation mode ------------------------------------------------
    // Source of truth, in order: memory flag (CSMenuMan) if configured; else our own START/BACK presses
    // and the manual toggle (hold left stick click without tilting).
    bool lThumbDown = (in.buttons & VRB_LThumb) != 0;
    bool stickIdle = std::fabs(lx) < 0.3f && std::fabs(ly) < 0.3f;
    if (lThumbDown && !g_lthumbHold.down) { g_lthumbHold = {true, now, false}; g_lastDpadDir = 0; g_menuToggleFired = false; }
    bool wasMenu = g_menuMode;
    if (c.menuNavEnabled) {
        int flag = GameMenuOpen(now);   // -1 = no memory flag configured/resolved
        bool toggle = c.menuNavToggleHold > 0.f && lThumbDown && !g_menuToggleFired && stickIdle && !g_lthumbHold.consumed &&
                      now - g_lthumbHold.since >= c.menuNavToggleHold;
        if (toggle) { g_menuToggleFired = true; g_lthumbHold.consumed = true; }
        if (flag >= 0) {
            // Memory flag drives the mode; a manual toggle overrides it until the flag next changes.
            if (flag != g_menuLastFlag) { g_menuOverride = false; g_menuLastFlag = flag; }
            if (toggle) { g_menuOverride = true; g_menuManual = !g_menuMode; }
            g_menuMode = g_menuOverride ? g_menuManual : (flag == 1);
        } else {
            if (c.menuNavFollowStart) {
                if (pressedStart) g_menuManual = !g_menuManual;   // START opens the menu, and closes it again
                if (pressedBack)  g_menuManual = false;           // map: the stick pans there, keep it a stick
            }
            if (toggle) g_menuManual = !g_menuManual;
            g_menuMode = g_menuManual;
        }
    } else {
        g_menuMode = g_menuManual = false;
    }
    if (g_menuMode != wasMenu) {
        LOGI("[menu] navigation mode %s", g_menuMode ? "ON (left stick = D-pad, gestures muted)" : "off");
        if (c.menuNavHaptics) { Haptic(HandLeft, g_menuMode ? 0.6f : 0.3f, now, g_menuMode ? 0.12 : 0.06); }
        g_menuDpadDir = 0;
    }

    // Left stick click: tap = LS (crouch); hold + tilt = D-pad (quick slots).
    bool dpadMode = !g_menuMode && c.leftGripPlusStickIsDpad && lThumbDown;
    if (g_menuMode) {
        // Held D-pad from stick tilt (the game auto-repeats), with hysteresis so the cursor does not stutter.
        float on = c.menuNavThreshold, off = c.menuNavThreshold * 0.6f;
        int dir = g_menuDpadDir;
        float ax = std::fabs(lx), ay = std::fabs(ly);
        bool stillHeld = dir && ((dir <= 2 ? ay : ax) >= off);
        if (!stillHeld) {
            dir = 0;
            if (ax >= on || ay >= on) dir = (ax > ay) ? (lx > 0 ? 4 : 3) : (ly > 0 ? 1 : 2);
        }
        if (dir != g_menuDpadDir) {
            g_menuDpadDir = dir;
            if (dir && c.menuNavHaptics) Haptic(HandLeft, 0.25f, now, 0.02);
        }
        static const uint16_t kDpadHeld[] = {0, XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT};
        held |= kDpadHeld[dir];
        lx = ly = 0.f;
    } else if (dpadMode) {
        int dir = 0;
        if (std::fabs(lx) > 0.6f || std::fabs(ly) > 0.6f) {
            if (std::fabs(lx) > std::fabs(ly)) dir = lx > 0 ? 4 : 3; else dir = ly > 0 ? 1 : 2;
        }
        if (dir && dir != g_lastDpadDir) {
            static const uint16_t kDpad[] = {0, XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT};
            AddPulse(kDpad[dir], now, c.gesturePressDuration);
            g_lthumbHold.consumed = true;
        }
        g_lastDpadDir = dir;
        lx = ly = 0.f;
    } else if (!c.leftGripPlusStickIsDpad) {
        mapHeld(g_map.lthumb, lThumbDown);
    }
    if (!lThumbDown && g_lthumbHold.down) {
        if ((c.leftGripPlusStickIsDpad || g_menuMode) && !g_lthumbHold.consumed && now - g_lthumbHold.since < 0.4) AddPulse(g_map.lthumb, now, c.gesturePressDuration);
        g_lthumbHold.down = false;
    }

    // Hand-relative movement: rotate the stick vector by (controller yaw - head yaw).
    if (c.handRelativeMovement && tr.hand[HandLeft].valid && tr.head.valid) {
        float d = WrapPi(tr.hand[HandLeft].rot.Yaw() - tr.head.rot.Yaw());
        float cs = std::cos(d), sn = std::sin(d);
        float nx = lx * cs + ly * sn, ny = -lx * sn + ly * cs;   // rotate around Y (stick y = forward)
        lx = nx; ly = ny;
    }

    // Snap turn: convert right-stick X into short full-deflection pulses.
    if (c.snapTurn) {
        if (std::fabs(rx) > 0.7f) {
            if (!g_snapActive || now - g_lastSnap >= c.snapTurnRepeatSeconds) { g_lastSnap = now; g_snapActive = true; }
            rx = (now - g_lastSnap) < c.snapTurnPulseSeconds ? (rx > 0 ? 1.f : -1.f) : 0.f;
        } else { g_snapActive = false; rx = 0.f; }
    } else {
        rx *= c.rightStickSensitivity; ry *= c.rightStickSensitivity;
    }

    // ---- gestures ----------------------------------------------------------
    Swing sr = g_gest.Update(HandRight, tr.hand[HandRight], rGrip, now);
    Swing sl = g_gest.Update(HandLeft,  tr.hand[HandLeft],  lGrip, now);
    auto fire = [&](uint16_t mask, Hand h, float amp) {
        if (mask == VP_LT || mask == VP_RT) {
            // analog pulse: represent as a pulse on the pseudo mask; resolved below
            AddPulse(mask, now, c.gesturePressDuration);
        } else AddPulse(mask, now, c.gesturePressDuration);
        if (c.gestureHaptics) Haptic(h, amp, now, 0.08);
    };
    if (!g_menuMode) {   // muted in menus: a swing there would press RB/RT/LB = tab switching / item use
        if (sr == Swing::Light) fire(g_map.light, HandRight, 0.5f);
        if (sr == Swing::Heavy) fire(g_map.heavy, HandRight, 1.0f);
        if (sl == Swing::LeftSwing) fire(g_map.leftSwing, HandLeft, 0.6f);
    }

    // ---- pulses -> outputs ----------------------------------------------------
    uint16_t pulses = ActivePulses(now);
    if (pulses & VP_LT) lt = 255;
    if (pulses & VP_RT) rt = 255;
    pulses &= ~(VP_LT | VP_RT);
    held &= ~(VP_LT | VP_RT);

    // ---- stop haptics that expired; keep game rumble alive ------------------------
    for (int h = 0; h < 2; ++h) {
        if (g_hapticUntil[h] > 0 && now >= g_hapticUntil[h]) {
            g_hapticUntil[h] = 0;
            if (auto* b = ActiveBackend()) b->Haptic((Hand)h, g_rumble[h], 0.f);
        }
    }
    if (g_rumbleDirty && c.rumbleToControllers) {
        g_rumbleDirty = false;
        if (auto* b = ActiveBackend()) for (int h = 0; h < 2; ++h) if (g_hapticUntil[h] == 0) b->Haptic((Hand)h, g_rumble[h], 0.f);
    }

    // ---- experimental hand aim ------------------------------------------------
    if (c.handAimEnabled) GameHandAimTick(tr, in, now);

    pad.buttons = held | pulses;
    pad.lt = lt; pad.rt = rt;
    pad.lx = ToStick(lx); pad.ly = ToStick(ly); pad.rx = ToStick(rx); pad.ry = ToStick(ry);
    std::lock_guard<std::mutex> g(g_mtx);
    g_pad = pad;
}

bool VirtualPadMerge(XINPUT_STATE& st) {
    VPad p;
    { std::lock_guard<std::mutex> g(g_mtx); p = g_pad; }
    if (!p.active) return false;
    XINPUT_GAMEPAD& g = st.Gamepad;
    uint16_t before = g.wButtons;
    g.wButtons |= p.buttons;
    if (p.lt > g.bLeftTrigger)  g.bLeftTrigger = p.lt;
    if (p.rt > g.bRightTrigger) g.bRightTrigger = p.rt;
    auto mag = [](int16_t x, int16_t y) { return (long)x * x + (long)y * y; };
    if (mag(p.lx, p.ly) > mag(g.sThumbLX, g.sThumbLY)) { g.sThumbLX = p.lx; g.sThumbLY = p.ly; }
    if (mag(p.rx, p.ry) > mag(g.sThumbRX, g.sThumbRY)) { g.sThumbRX = p.rx; g.sThumbRY = p.ry; }
    (void)before;
    return true;
}

void VirtualPadRumble(float left, float right) {
    const Config& c = Cfg();
    float l = Clamp(left * c.rumbleScale, 0.f, 1.f), r = Clamp(right * c.rumbleScale, 0.f, 1.f);
    if (l != g_rumble[0] || r != g_rumble[1]) { g_rumble[0] = l; g_rumble[1] = r; g_rumbleDirty = true; }
}
}
