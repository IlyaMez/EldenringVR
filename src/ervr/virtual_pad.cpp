#include "virtual_pad.h"
#include <atomic>
#include "vr_backend.h"
#include "gestures.h"
#include "combat.h"
#include "game_er.h"
#include "config.h"
#include "log.h"
#include "game_memory.h"
#include "belt.h"
#include "body_calibration.h"
#include "weapon_wheel.h"
#include <mutex>
#include <cctype>
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
    int      padRole[2];     // PadRole, per hand
    uint16_t padButton[2];   // PadButton: the XInput mask
} g_map;

// What a pressed trackpad (Valve Index) is: [Buttons] LeftPad / RightPad.
enum PadRole { PadNone = 0, PadMenu, PadDpad, PadButton };
struct PadPress { bool down = false; int dir = 0; } g_padPress[2];
const uint16_t kDpadMask[] = {0, XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT};

int ParsePadRole(const std::string& raw, uint16_t& button) {
    std::string n = raw;
    for (auto& ch : n) ch = (char)toupper((unsigned char)ch);
    button = 0;
    if (n == "MENU") return PadMenu;
    if (n == "DPAD") return PadDpad;
    button = ParseXInputButton(raw);
    return button ? PadButton : PadNone;
}

// The D-pad direction a press on the pad's edge means (0 none, 1 up, 2 down, 3 left, 4 right). The middle
// third is no direction: a press there is ambiguous, and guessing would switch the wrong slot.
int PadDirection(float x, float y) {
    const float ax = std::fabs(x), ay = std::fabs(y);
    if (ax < 0.3f && ay < 0.3f) return 0;
    return ax > ay ? (x > 0 ? 4 : 3) : (y > 0 ? 1 : 2);
}

// Timed pulses (gesture attacks, tap-vs-hold buttons, snap turns).
constexpr int kMaxPulses = 16;
Pulse g_pulses[kMaxPulses];

// Tap/hold trackers.
struct HoldTracker { bool down = false; double since = 0; bool consumed = false; };
HoldTracker g_menuHold, g_lthumbHold;
double g_lastSnap = -1e9;
int g_lastDpadDir = 0;
bool g_snapActive = false;

// Menu navigation mode: left stick = held D-pad, movement zeroed, gestures muted.
bool g_menuMode = false;        // effective state (memory flag if known, else manual/START-driven)
MoveIntent g_move;
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

std::atomic<ULONGLONG> g_lastATick{0};
ULONGLONG VirtualPadLastATick() { return g_lastATick.load(std::memory_order_relaxed); }

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
    g_map.padRole[HandLeft] = ParsePadRole(c.btnLeftPad, g_map.padButton[HandLeft]);
    g_map.padRole[HandRight] = ParsePadRole(c.btnRightPad, g_map.padButton[HandRight]);
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
        // Keep watching the game's menu state without controllers too (same thread, a few
        // reads): the readers earn trust as menus open and close, and unattended runs -
        // no headset, autopilot tapping START - can verify them from the [menu] log lines.
        if (c.menuNavEnabled && GameERPlayerActive()) CombatSetMenuOpen(GameMenuOpen(now) == 1 || GameERForeignCamActive());
        std::lock_guard<std::mutex> g(g_mtx); g_pad = pad; g_move = {}; return;
    }
    pad.active = true;
    uint16_t held = 0;   // buttons held as long as the physical control is held
    uint8_t lt = 0, rt = 0;

    // ---- body calibration ------------------------------------------------------
    // The T-pose panel reads the controls first: while it asks, the triggers, grips, A and B are its own (each until
    // let go), so everything below sees them released - no attack, guard, wheel, belt or grab. Last update's menu
    // state gates it (this update's is worked out below, from controls the panel may not have).
    static bool s_menuBefore = false;
    const CalibPadOut calib = BodyCalibrationUpdate(tr, in, now, s_menuBefore);
    for (int h = 0; h < 2; ++h) {
        if (calib.trigger[h]) { in.indexTrigger[h] = 0.f; in.buttons &= ~(h == HandLeft ? VRB_LTrigger : VRB_RTrigger); }
        if (calib.grip[h]) { in.handTrigger[h] = 0.f; in.buttons &= ~(h == HandLeft ? VRB_LGrip : VRB_RGrip); }
        if (calib.hapticAmp[h] > 0.f) Haptic((Hand)h, calib.hapticAmp[h], now, calib.hapticSec[h]);
    }
    in.buttons &= ~calib.buttons;

    auto mapHeld = [&](uint16_t mask, bool down) {
        if (!down || !mask) return;
        if (mask == VP_LT) lt = 255; else if (mask == VP_RT) rt = 255; else held |= mask;
    };

    // The face buttons are mapped after the belt has seen them (below): its use button, pressed on an item in
    // hand, is the belt's.

    // ---- triggers / grips ----------------------------------------------------
    bool lTrig = in.indexTrigger[HandLeft]  >= c.triggerThreshold;
    bool rTrig = in.indexTrigger[HandRight] >= c.triggerThreshold;
    bool lGrip = in.handTrigger[HandLeft]  >= c.gripThreshold;
    bool rGrip = in.handTrigger[HandRight] >= c.gripThreshold;
    mapHeld(g_map.ltrig, lTrig); mapHeld(g_map.rtrig, rTrig);
    // The grips are mapped after the weapon wheel has seen them (below): it may hold one back.

    // ---- trackpads (Valve Index) -----------------------------------------------
    // Pressed = the pad's force past PadForce, released below 0.6x of it. The D-pad direction is the one
    // the press started in, so the thumb rolling on the pad mid-press cannot switch it.
    bool padMenu = false;
    for (int h = 0; h < 2; ++h) {
        PadPress& p = g_padPress[h];
        if (!p.down && c.padForce > 0.f && in.padForce[h] >= c.padForce) p = {true, PadDirection(in.pad[h][0], in.pad[h][1])};
        else if (p.down && in.padForce[h] < c.padForce * 0.6f) p.down = false;
        if (!p.down) continue;
        if (g_map.padRole[h] == PadMenu) padMenu = true;
        else if (g_map.padRole[h] == PadDpad) held |= kDpadMask[p.dir];
        else mapHeld(g_map.padButton[h], true);
    }

    // ---- Menu: tap = START, hold = BACK (map) ---------------------------------
    // Both stick clicks together are the menu button too ([Buttons] MenuStickChord): SteamVR keeps a Quest's left
    // menu button for its own dashboard (Link, Air Link, Steam Link, Virtual Desktop's SteamVR mode), so there
    // the game's menu could not be opened at all (2026-11-04, Quest 2 and 3). While the chord is held neither
    // click reaches the game (no crouch, no lock-on) and the left click's hold does not toggle menu navigation.
    static bool s_stickChord = false;
    const uint32_t bothSticks = VRB_LThumb | VRB_RThumb;
    s_stickChord = c.menuStickChord &&
                   ((in.buttons & bothSticks) == bothSticks || (s_stickChord && (in.buttons & bothSticks) != 0));
    if (s_stickChord) { in.buttons &= ~bothSticks; g_lthumbHold.consumed = true; }
    bool menuDown = (in.buttons & VRB_Menu) != 0 || padMenu || s_stickChord;
    if (menuDown && !g_menuHold.down) { g_menuHold = {true, now, false}; }
    bool pressedStart = false, pressedBack = false;
    if (menuDown && !g_menuHold.consumed && now - g_menuHold.since >= c.menuHoldForBack) {
        AddPulse(XINPUT_GAMEPAD_BACK, now, c.gesturePressDuration); g_menuHold.consumed = true; pressedBack = true;
    }
    if (!menuDown && g_menuHold.down) {
        if (!g_menuHold.consumed) { AddPulse(g_map.menu, now, c.gesturePressDuration); pressedStart = (g_map.menu == XINPUT_GAMEPAD_START); }
        g_menuHold.down = false;
    }

    // ---- sticks -------------------------------------------------------------
    float lx = in.thumb[HandLeft][0],  ly = in.thumb[HandLeft][1];
    float rx = in.thumb[HandRight][0], ry = in.thumb[HandRight][1];
    if (c.swapSticks) { std::swap(lx, rx); std::swap(ly, ry); }
    ApplyDeadzone(lx, ly, c.stickDeadzone);
    ApplyDeadzone(rx, ry, c.stickDeadzone);

    // ---- menu navigation mode ------------------------------------------------
    // Source of truth: the game's own menu state (GameMenuOpen - CSFeMan HUD state, pad routing
    // or MenuChain, see game_memory.cpp). Only until one of those has proven itself does the mod
    // guess from its own START/BACK presses - the guess cannot see a menu closed with B, which
    // left the stick in D-pad mode until the next START cycle. A manual toggle (hold left stick
    // click without tilting) overrides the game state until the game agrees with it or the next
    // START press; BACK (map) forces the stick back to a stick the same way.
    int gameMenu = -1;   // GameMenuOpen this update (-1 = not read / no trusted reader)
    bool lThumbDown = (in.buttons & VRB_LThumb) != 0;
    bool stickIdle = std::fabs(lx) < 0.3f && std::fabs(ly) < 0.3f;
    if (lThumbDown && !g_lthumbHold.down) { g_lthumbHold = {true, now, false}; g_lastDpadDir = 0; g_menuToggleFired = false; }
    bool wasMenu = g_menuMode;
    if (!c.menuNavEnabled) {
        g_menuMode = g_menuManual = g_menuOverride = false;
    } else if (!GameERPlayerActive()) {
        // Title screen, loading, character creation: these screens navigate from the ANALOG
        // stick, and a held D-pad there stopped the selection moving (reported 2026-08-21).
        g_menuMode = g_menuManual = g_menuOverride = false;
    } else if (GameERPlayerDead()) {
        // Dead: the only thing left to steer is the "Stake of Marika / last site of grace" choice, which neither
        // menu reader sees - its cursor did not move and the stake was the only answer (2026-11-04). The left
        // stick is its D-pad until the player is back; the manual toggle is left as it was.
        g_menuMode = true;
    } else {
        int flag = GameMenuOpen(now);   // -1 = no trusted game-state reader yet
        gameMenu = flag;
        bool toggle = c.menuNavToggleHold > 0.f && lThumbDown && !g_menuToggleFired && stickIdle && !g_lthumbHold.consumed &&
                      now - g_lthumbHold.since >= c.menuNavToggleHold;
        if (toggle) { g_menuToggleFired = true; g_lthumbHold.consumed = true; }
        if (flag >= 0) {
            bool changed = flag != g_menuLastFlag; g_menuLastFlag = flag;
            if (g_menuOverride && changed && (flag == 1) == g_menuManual) g_menuOverride = false;   // the game caught up
            if (pressedStart) g_menuOverride = false;                            // a new menu: the game state decides again
            if (pressedBack)  { g_menuOverride = true; g_menuManual = false; }   // map: the stick pans there
            if (toggle)       { g_menuOverride = true; g_menuManual = !g_menuMode; }
            g_menuMode = g_menuOverride ? g_menuManual : (flag == 1);
        } else {
            g_menuLastFlag = -1; g_menuOverride = false;
            if (c.menuNavFollowStart) {
                if (pressedStart) g_menuManual = !g_menuManual;   // START opens the menu, and closes it again
                if (pressedBack)  g_menuManual = false;           // map: the stick pans there, keep it a stick
            }
            if (toggle) g_menuManual = !g_menuManual;
            g_menuMode = g_menuManual;
        }
    }
    // A takeover camera (map above all, cutscenes too) means the screen on top navigates with
    // ANALOG sticks - the map pans with the left and zooms with the right - even when the menu
    // readers still say "menu open" (the map entered through the pause menu keeps menuman's
    // open bit set, and 14:39 on 2026-08-31 shows menu-nav D-pad mode hijacking the pan there).
    // Not while dead: the death camera is no map, and the stake/grace choice needs the D-pad.
    const bool mapCam = GameERForeignCamActive() && !GameERPlayerDead();
    if (mapCam && g_menuMode) g_menuMode = false;
    if (g_menuMode != wasMenu) {
        LOGI("[menu] navigation mode %s", g_menuMode ? "ON (left stick = D-pad, gestures muted)" : "off");
        if (c.menuNavHaptics) { Haptic(HandLeft, g_menuMode ? 0.6f : 0.3f, now, g_menuMode ? 0.12 : 0.06); }
        g_menuDpadDir = 0;
    }
    // Combat must stand down in menus even when the nav-mode guess is wrong: publish the union of
    // the game's own menu state and the nav mode (manual toggles included). Combat gates its swing
    // detection, injection, guard hold and chords on this directly (see CombatSetMenuOpen).
    // The MAP is invisible to both menu readers (menuman state 00, hud raw 0 the whole time it is
    // up - 2026-08-31 log), so combat stayed live there and swings kept injecting into the world
    // sim behind the map. The foreign-camera detector covers exactly that hole - it arms within
    // ~300 ms of the map (or a cutscene) taking the view and gameplay blips never arm it. anyMenu
    // is the full union; the stick shaping below keys off it too, because the map reads BOTH
    // sticks raw (left pans, RIGHT ZOOMS - 2026-08-31 "can navigate but not zoom": ervrTurn was
    // consuming the right stick for view yaw with the world sim still live behind the map).
    const bool anyMenu = g_menuMode || gameMenu == 1 || mapCam;
    s_menuBefore = anyMenu;
    CombatSetMenuOpen(anyMenu);

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

    // ---- movement basis --------------------------------------------------------
    // The game resolves the move stick in the frame of the camera matrix it sees. With CameraRotation=hmd that
    // matrix is ERVR's stick yaw composed with the full HMD orientation (mb.viewYaw = camYaw + headYaw), so
    // "forward" already follows your head; with CameraRotation=yaw/game it is the stick yaw alone. Either way the
    // rule is the same: rotate the stick by (yaw the player means by forward) - (yaw of the game's frame),
    // measured in the game convention (positive = right):
    //   head   = where you look (default; what every VR game does)
    //   hand   = where the left controller points
    //   body   = where the character body is turned (BodyYaw)
    //   camera = the stick yaw itself (forward is wherever you last turned with the stick, whatever the head does)
    // (Rotating by headYaw unconditionally - the old code - double-counted the head yaw under CameraRotation=hmd:
    // physically turning 90 degrees sent stick-forward 180 degrees off.)
    MoveBasis mb = GameERGetMoveBasis();
    auto rotateStick = [&](float r) {   // rotate the stick vector by r about Y (stick y = forward, positive r = right)
        if (r == 0.f) return;
        float cs = std::cos(r), sn = std::sin(r);
        float nx = lx * cs + ly * sn, ny = -lx * sn + ly * cs;
        lx = nx; ly = ny;
    };
    if (mb.active && !anyMenu) {   // first person only: in third person the game steers its own camera
        // VR-frame Yaw() is positive to the LEFT (0 = -Z); the game's yaw is positive to the right.
        float want = mb.viewYaw;                                             // default: leave the stick alone
        if (c.moveForward == "hand" && tr.hand[HandLeft].valid) want = mb.camYaw - tr.hand[HandLeft].rot.Yaw();
        else if (c.moveForward == "body") want = mb.bodyYaw;
        else if (c.moveForward == "camera") want = mb.camYaw;
        else if (mb.rideFreeLook) want = mb.rideForwardYaw;                 // on Torrent: the free-look direction, a glance does not steer
        else if (mb.headValid) want = mb.camYaw + mb.headYaw;               // head (and hand without a controller)
        rotateStick(WrapPi(want - mb.viewYaw));
    } else if (c.moveForward == "hand" && tr.hand[HandLeft].valid && tr.head.valid && !anyMenu) {
        // Game-owned view yaw (default): forward is where you look; hand-relative = rotate by (controller - head) yaw.
        rotateStick(WrapPi(tr.hand[HandLeft].rot.Yaw() - tr.head.rot.Yaw()));
    }

    // ---- turning ---------------------------------------------------------------
    // When ERVR owns the view yaw the turn stick drives *our* yaw, not the game's camera.
    bool ervrTurn = mb.active && c.cameraTurnErvr && !anyMenu;   // menus AND the map keep the stock right stick (map zoom)
    if (ervrTurn) {
        static double s_lastTurn = 0;
        float dt = (float)Clamp((float)(now - s_lastTurn), 0.f, 0.1f);
        s_lastTurn = now;
        float delta = 0.f;
        if (c.snapTurn) {
            if (std::fabs(rx) > 0.7f) {
                if (!g_snapActive || now - g_lastSnap >= c.snapTurnRepeatSeconds) { g_lastSnap = now; g_snapActive = true; delta = (rx > 0 ? 1.f : -1.f) * c.snapTurnDegrees; }
            } else g_snapActive = false;
        } else {
            delta = rx * c.turnSpeedDegPerSec * c.rightStickSensitivity * dt;
        }
        if (c.rightStick == "camera") GameERAddCameraYaw(Deg2Rad(delta));
        else GameERAddBodyYaw(Deg2Rad(delta));   // body only: the view stays where it is
        if (!mb.lockedOn) rx = 0.f;   // locked on the right stick still switches targets; otherwise it is ours
        // ERVR consumes X for yaw only. Keep Y available for map zoom even when
        // the map leaves both menu flags and the gameplay FOV unchanged. The
        // first-person camera derives pitch from the HMD, not this stick axis.
    } else if (c.snapTurn && !anyMenu) {
        // Snap turn without yaw ownership: convert right-stick X into short full-deflection pulses.
        if (std::fabs(rx) > 0.7f) {
            if (!g_snapActive || now - g_lastSnap >= c.snapTurnRepeatSeconds) { g_lastSnap = now; g_snapActive = true; }
            rx = (now - g_lastSnap) < c.snapTurnPulseSeconds ? (rx > 0 ? 1.f : -1.f) : 0.f;
        } else { g_snapActive = false; rx = 0.f; }
    } else if (!anyMenu) {
        rx *= c.rightStickSensitivity; ry *= c.rightStickSensitivity;
    }
    if (mb.active && c.moveRelativeToCamera && !anyMenu) {
        float d = WrapPi(mb.camYaw - mb.chrYaw);
        float cs = std::cos(d), sn = std::sin(d);
        float nx = lx * cs + ly * sn, ny = -lx * sn + ly * cs;
        lx = nx; ly = ny;
    }

    // ---- belt ------------------------------------------------------------------
    // The pouch's four shortcut items hang at the waist: a grip closed on one takes it, the use button uses it
    // (the game's own Event Action + D-pad shortcut), an open hand puts it back. Before combat, so a grip closed
    // on a belt item is never read as a two-hand grab, and the hand holding one never swings.
    const BeltPadOut belt = BeltUpdate(tr, in, now, anyMenu || CombatMenuOpen() || calib.showing);

    // ---- face buttons ------------------------------------------------------
    const uint32_t faces = in.buttons & ~belt.swallow;
    mapHeld(g_map.a, faces & VRB_A);
    mapHeld(g_map.b, faces & VRB_B);
    mapHeld(g_map.x, faces & VRB_X);
    mapHeld(g_map.rthumb, faces & VRB_RThumb);
    const bool yDown = (faces & VRB_Y) != 0;
    mapHeld(g_map.y, yDown);

    // ---- gestures ----------------------------------------------------------
    // Physical combat (swing -> game AttackBehavior event) takes over from button pulses when active.
    // The T-pose is no swing: combat sees still hands while the calibration panel asks.
    bool physical = CombatHandlesSwings();
    if (physical && !g_menuMode && !calib.showing) CombatPollHands(tr, in, now);
    else CombatPollHands(VRTracking{}, VRInputState{}, now);   // keeps its state machine idle
    if (uint32_t hr = CombatTakeHapticRequests(); hr && c.gestureHaptics) {
        for (int h = 0; h < 2; ++h) {
            if (hr & (4u << h)) Haptic((Hand)h, 1.0f, now, 0.25);          // rejected (no stamina): long buzz
            else if (hr & (1u << h)) Haptic((Hand)h, 0.35f, now, 0.05);    // swing armed: short tick
        }
        if (hr & 32u) Haptic(HandLeft, 1.0f, now, 0.18);                    // deflect: hard, long
        else if (hr & 16u) Haptic(HandLeft, 0.6f, now, 0.08);               // blocked hit: thud
        for (int h = 0; h < 2; ++h) if (hr & (64u << h)) Haptic((Hand)h, 0.9f, now, 0.07);   // strike landed: thud in the weapon hand
        if (hr & 256u) { Haptic(HandLeft, 0.7f, now, 0.15); Haptic(HandRight, 0.7f, now, 0.15); }   // two-handed: both hands close on the weapon
        else if (hr & 512u) { Haptic(HandLeft, 0.4f, now, 0.06); Haptic(HandRight, 0.4f, now, 0.06); }   // back to one hand
    }
    // A hand holding a belt item is not swinging (its pose reads as untracked to the detector).
    auto gesturePose = [&](int h) { Pose p = tr.hand[h]; if (BeltOwnsHand(h)) p.valid = false; return p; };
    Swing sr = g_gest.Update(HandRight, gesturePose(HandRight), rGrip, now);
    Swing sl = g_gest.Update(HandLeft,  gesturePose(HandLeft),  lGrip, now);
    auto fire = [&](uint16_t mask, Hand h, float amp) {
        if (mask == VP_LT || mask == VP_RT) {
            // analog pulse: represent as a pulse on the pseudo mask; resolved below
            AddPulse(mask, now, c.gesturePressDuration);
        } else AddPulse(mask, now, c.gesturePressDuration);
        CombatNotifyButton(mask, now);
        if (c.gestureHaptics) Haptic(h, amp, now, 0.08);
    };
    if (!g_menuMode && !physical && !CombatMenuOpen() && !calib.showing) {   // muted in menus: a swing there would press RB/RT/LB = tab switching / item use
        if (sr == Swing::Light) fire(g_map.light, HandRight, 0.5f);
        if (sr == Swing::Heavy) fire(g_map.heavy, HandRight, 1.0f);
        if (sl == Swing::LeftSwing) fire(g_map.leftSwing, HandLeft, 0.6f);
    }

    // ---- weapon wheel ------------------------------------------------------------
    // A grip held still opens that hand's weapon wheel, so a squeeze the wheel may still claim stays
    // off the pad; a quick one comes back as a single tap of the grip's mapping. After combat, whose
    // two-hand grab and swing state the wheel's gate reads.
    const WheelPadOut wheel = WeaponWheelUpdate(tr, in, now, anyMenu || CombatMenuOpen() || calib.showing);
    // A grip holding a belt item is the belt's: its [Buttons] mapping stays off the pad too.
    mapHeld(g_map.lgrip, lGrip && !wheel.holdGrip[HandLeft] && !belt.holdGrip[HandLeft]);
    mapHeld(g_map.rgrip, rGrip && !wheel.holdGrip[HandRight] && !belt.holdGrip[HandRight]);
    if (wheel.tapGrip[HandLeft]) AddPulse(g_map.lgrip, now, c.gesturePressDuration);
    if (wheel.tapGrip[HandRight]) AddPulse(g_map.rgrip, now, c.gesturePressDuration);
    held |= wheel.buttons;   // the game's own weapon-switch presses (D-pad left/right)
    held |= belt.buttons;    // the game's own pouch shortcut (Event Action + D-pad)
    for (int h = 0; h < 2; ++h) {
        if (wheel.hapticAmp[h] > 0.f) Haptic((Hand)h, wheel.hapticAmp[h], now, wheel.hapticSec[h]);
        else if (belt.hapticAmp[h] > 0.f) Haptic((Hand)h, belt.hapticAmp[h], now, belt.hapticSec[h]);
    }

    // ---- pulses -> outputs ----------------------------------------------------
    uint16_t pulses = ActivePulses(now);
    if (pulses & VP_LT) lt = 255;
    if (pulses & VP_RT) rt = 255;
    if (!g_menuMode) {   // physical combat: trigger/grip must not start attack animations (menus still need RB/RT tabs)
        uint16_t blocked = CombatBlockedButtons();
        // With the game's two-hand modifier (Y) physically held, trigger/grip are a CHORD (Y + RB / Y + LB
        // two-hands), not an attack: let them through so the chord can be made by hand.
        if (yDown) blocked &= ~(g_map.light | g_map.heavy | g_map.leftSwing | g_map.ltrig | g_map.rtrig);
        if (blocked) { held &= ~blocked; pulses &= ~blocked; if (blocked & VP_RT) rt = 0; if (blocked & VP_LT) lt = 0; }
        held |= CombatHeldButtons();   // raised shield = guard held
        // The game's own chords combat plays through the block: two-hand toggle (Y + RB/LB), skill, learn press.
        const uint16_t forced = CombatForcedButtons(now);
        if (forced & VP_LT) lt = 255; if (forced & VP_RT) rt = 255;
        held |= forced & ~(VP_LT | VP_RT);
    }
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
    // Cast aim: combat learns the casting hand from what the GAME is handed (a menu's presses are navigation, not casts).
    CombatNotifyPad((g_menuMode || anyMenu || CombatMenuOpen()) ? (uint16_t)0
                    : (uint16_t)(pad.buttons | (pad.lt > 127 ? VP_LT : 0) | (pad.rt > 127 ? VP_RT : 0)), now);
    pad.lx = ToStick(lx); pad.ly = ToStick(ly); pad.rx = ToStick(rx); pad.ry = ToStick(ry);

    // What the GAME is handed, logged on change. Pairs with "[xr] raw" (what the runtime
    // handed US), so a control that does nothing can be blamed on exactly one of the two
    // hops instead of both being suspect at once.
    {
        auto dir8 = [](int16_t x, int16_t y) {
            const int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
            if (ax < 8000 && ay < 8000) return 0;
            return ax > ay ? (x > 0 ? 1 : 2) : (y > 0 ? 3 : 4);
        };
        static uint16_t s_btn = 0; static int s_l = 0, s_r = 0; static int s_menu = -1;
        const int l = dir8(pad.lx, pad.ly), r = dir8(pad.rx, pad.ry);
        if (pad.buttons != s_btn || l != s_l || r != s_r || (int)g_menuMode != s_menu) {
            s_btn = pad.buttons; s_l = l; s_r = r; s_menu = (int)g_menuMode;
            LOGD("[pad] -> game: btn 0x%04X lt %u rt %u stick L(%d,%d) R(%d,%d)%s",
                 pad.buttons, pad.lt, pad.rt, pad.lx, pad.ly, pad.rx, pad.ry,
                 g_menuMode ? "  [menu-nav ON: left stick replaced by held D-pad]" : "");
        }
    }
    {
        MoveIntent mi; mi.x = lx; mi.y = ly; mi.sprint = (pad.buttons & XINPUT_GAMEPAD_B) != 0; mi.valid = !anyMenu;
        std::lock_guard<std::mutex> g(g_mtx); g_move = mi;
        if (c.locomotion == "direct" && !c.locoFeedStick && !mb.riding && !g_menuMode) { pad.lx = 0; pad.ly = 0; }
    }
    std::lock_guard<std::mutex> g(g_mtx);
    g_pad = pad;
}
MoveIntent VirtualPadGetMove() { std::lock_guard<std::mutex> g(g_mtx); return g_move; }

bool VirtualPadMerge(XINPUT_STATE& st) {
    VPad p;
    { std::lock_guard<std::mutex> g(g_mtx); p = g_pad; }
    if (!p.active) return false;
    XINPUT_GAMEPAD& g = st.Gamepad;
    uint16_t before = g.wButtons;
    g.wButtons |= p.buttons;
    // Confirm-press telemetry for the stereo pump: every dangerous menu confirm (fast
    // travel above all) is an A press. The pump drops pass 2 to the SAFE keep set for a
    // few seconds after each one, which makes the confirm-window state corruption
    // structurally impossible at the cost of briefly flattened depth. See Pass2SafeOnA.
    // Deliberately UNGATED. A stash-quiet "menu context" gate stood here for one build and
    // caused the 2026-08-22 20:13 fast-travel crash: ER's map does NOT stop the world sim -
    // stereo pairs flowed at full rate through the whole map screen - so the travel confirm
    // read as gameplay and SAFE never armed for the one window it exists for.
    if (g.wButtons & XINPUT_GAMEPAD_A)
        g_lastATick.store(GetTickCount64(), std::memory_order_relaxed);
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
