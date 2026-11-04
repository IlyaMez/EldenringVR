#include "virtual_pad.h"
#include "vr_backend.h"
#include "game_er.h"
#include "game_memory.h"
#include "combat.h"
#include "config.h"
#include "log.h"
#include "weapon_wheel.h"
#include "belt.h"
#include "body_calibration.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace {
ervr::VRInputState input;
ervr::MoveBasis basis;
int menu = 0;
bool foreignCam = false, combatMenu = false;
float yaw = 0.f;
double now = 1.0;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}
XINPUT_GAMEPAD Poll() {
    ervr::VirtualPadUpdate(now += 0.02);
    XINPUT_STATE state{};
    Check(ervr::VirtualPadMerge(state), "virtual controller must be connected");
    return state.Gamepad;
}
}

// Exercise production mapping, deadzone, turn logic and XInput merge. Substitute
// only the external game, combat and OpenXR endpoints.
namespace ervr {
void Log(LogLevel, const char*, ...) {}
IVRBackend* ActiveBackend() { return nullptr; }
bool GetLatestVRState(VRTracking& tr, VRInputState& in) { tr = {}; in = input; return true; }
bool GameERPlayerActive() { return true; }
bool GameERForeignCamActive() { return foreignCam; }
int GameMenuOpen(double) { return menu; }
MoveBasis GameERGetMoveBasis() { return basis; }
void GameERAddCameraYaw(float amount) { yaw += amount; }
void GameERAddBodyYaw(float amount) { yaw += amount; }
void GameHandAimTick(const VRTracking&, const VRInputState&, double) {}
void CombatSetMenuOpen(bool open) { combatMenu = open; }
bool CombatMenuOpen() { return combatMenu; }
bool CombatHandlesSwings() { return !combatMenu; }
void CombatPollHands(const VRTracking&, const VRInputState&, double) {}
void CombatNotifyButton(uint16_t, double) {}
void CombatNotifyPad(uint16_t, double) {}
uint32_t CombatTakeHapticRequests() { return 0; }
uint16_t CombatBlockedButtons() { return combatMenu ? 0 : VP_RT; }
uint16_t CombatHeldButtons() { return 0; }
uint16_t CombatForcedButtons(double) { return 0; }
WheelPadOut wheelOut;
WheelPadOut WeaponWheelUpdate(const VRTracking&, const VRInputState&, double, bool) { return wheelOut; }
BeltPadOut beltOut;
BeltPadOut BeltUpdate(const VRTracking&, const VRInputState&, double, bool) { return beltOut; }
bool BeltOwnsHand(int) { return false; }
CalibPadOut calibOut;
VRInputState calibSaw;
CalibPadOut BodyCalibrationUpdate(const VRTracking&, const VRInputState& in, double, bool) { calibSaw = in; return calibOut; }
}

int main() {
    using namespace ervr;
    Config& c = Cfg();
    c.gesturesEnabled = false; c.menuNavHaptics = false;
    basis.active = true;
    input.controllerConnected[0] = input.controllerConnected[1] = true;
    VirtualPadInit();
    for (bool snap : {false, true}) {
        c.snapTurn = snap;
        for (int state : {-1, 0, 1}) {
            menu = state;
            for (float zoom : {-1.f, 1.f}) {
                input.thumb[HandRight][0] = 0;
                input.thumb[HandRight][1] = zoom;
                const float before = yaw;
                auto pad = Poll();
                Check(pad.sThumbRY == (zoom > 0 ? 32767 : -32767),
                      "map zoom must reach XInput both ways with unknown/closed menu flags and unchanged FOV");
                Check(before == yaw, "vertical map zoom must not turn the VR view");
            }
        }
    }
    menu = 0; c.snapTurn = false;
    input.thumb[HandRight][0] = 1; input.thumb[HandRight][1] = 0;
    float before = yaw;
    auto pad = Poll();
    Check(yaw > before && pad.sThumbRX == 0, "horizontal gameplay turning must still drive ERVR only");
    basis.lockedOn = true;
    Check(Poll().sThumbRX == 32767, "lock-on target switching must retain horizontal stick input");
    basis.lockedOn = false; foreignCam = true;
    c.rightStickSensitivity = 0.1f;
    input.thumb[HandRight][1] = 1; input.thumb[HandRight][0] = 0;
    before = yaw; pad = Poll();
    Check(pad.sThumbRY == 32767 && before == yaw, "recognized map input must not inherit gameplay camera sensitivity");
    Check(!VirtualPadGetMove().valid, "map input must not become a gameplay dodge/sprint intent");

    // Dash/roll must receive the same movement vector as walking, through every
    // forward reference and lock-on state. Changing physical body facing must
    // not feed back into the movement vector when native roll turning takes over.
    foreignCam = false; input = {}; input.controllerConnected[0] = input.controllerConnected[1] = true;
    c.moveRelativeToCamera = false; c.swapSticks = false;
    for (const char* forward : {"head", "camera", "body"}) {
        c.moveForward = forward;
        for (bool locked : {false, true}) {
            basis.lockedOn = locked; basis.headValid = true;
            basis.camYaw = 0.4f; basis.headYaw = 0.7f; basis.viewYaw = 1.1f; basis.bodyYaw = -0.6f;
            for (int dir = 0; dir < 8; ++dir) {
                const float angle = dir * 3.14159265f / 4.f;
                input.thumb[HandLeft][0] = std::sin(angle); input.thumb[HandLeft][1] = std::cos(angle);
                input.buttons = 0;
                const auto walk = Poll();
                input.buttons = VRB_B;
                basis.chrYaw += 1.2f;
                const auto dash = Poll();
                Check(dash.sThumbLX == walk.sThumbLX && dash.sThumbLY == walk.sThumbLY &&
                      (dash.wButtons & XINPUT_GAMEPAD_B), "dash must follow the walking joystick direction");
                Check(VirtualPadGetMove().valid && VirtualPadGetMove().sprint, "held B must release the physics pin");
                input.buttons = 0;
                const auto roll = Poll();
                Check(roll.sThumbLX == walk.sThumbLX && roll.sThumbLY == walk.sThumbLY &&
                      !(roll.wButtons & XINPUT_GAMEPAD_B), "roll release must retain the directional joystick");
            }
        }
    }
    // Direct movement yields to Torrent. Suppressing the virtual stick as well
    // would leave the mount unable to move, even though its native pose is correct.
    basis.lockedOn = false; basis.camYaw = basis.headYaw = basis.viewYaw = 0.f;
    c.moveForward = "head"; c.locomotion = "direct"; c.locoFeedStick = false;
    input.thumb[HandLeft][0] = 0.f; input.thumb[HandLeft][1] = 1.f;
    for (bool riding : {false, true, true, false}) {
        basis.riding = riding;
        const auto move = Poll();
        Check(move.sThumbLY == (riding ? 32767 : 0),
              "Torrent receives native movement input; dismount restores direct-mode stick suppression");
        Check(VirtualPadGetMove().valid && VirtualPadGetMove().y > 0.99f,
              "processed movement intent is preserved across mount transitions");
    }
    basis.riding = true; basis.active = false;
    Check(Poll().sThumbLY == 32767, "mounted input also passes through with game-owned camera yaw");
    // Free look on Torrent: looking 40 degrees aside must not steer - the stick, resolved in the 40-degree view
    // frame, comes back to the free-look direction (forward = straight ahead).
    basis.active = true; c.locomotion = "game"; basis.headValid = true;
    basis.camYaw = 0.f; basis.headYaw = basis.viewYaw = 0.6981317f;
    basis.rideFreeLook = true; basis.rideForwardYaw = 0.f;
    {
        const auto move = Poll();
        Check(move.sThumbLX < -20000 && move.sThumbLY > 24000 && move.sThumbLY < 26000,
              "on Torrent a glance inside the free look does not steer the horse");
    }
    basis.rideFreeLook = false;
    const int headLx = Poll().sThumbLX;
    Check(headLx > -100 && headLx < 100, "without the free look Forward=head steers where you look");
    basis.headYaw = basis.viewYaw = 0.f;
    basis.active = true; basis.riding = false; c.locomotion = "game";
    input.buttons = VRB_B; Poll();
    input.controllerConnected[0] = input.controllerConnected[1] = false;
    VirtualPadUpdate(now += 0.02);
    Check(!VirtualPadGetMove().valid && !VirtualPadGetMove().sprint, "disconnect must clear stale held dash intent");

    // The weapon wheel's say over the grips: a grip it holds back never reaches the game, a quick
    // squeeze arrives as one tap of the grip's mapping, and its switch presses reach the D-pad.
    input = {}; input.controllerConnected[0] = input.controllerConnected[1] = true;
    input.handTrigger[HandLeft] = 1.f;   // LeftGrip=LT
    wheelOut = {};
    Check(Poll().bLeftTrigger == 255, "a grip the wheel does not claim sends its mapping as before");
    wheelOut.holdGrip[HandLeft] = true;
    Check(Poll().bLeftTrigger == 0, "a grip the wheel may still claim stays off the pad");
    input.handTrigger[HandLeft] = 0.f;
    wheelOut = {}; wheelOut.tapGrip[HandLeft] = true;
    Check(Poll().bLeftTrigger == 255, "a quick squeeze comes back as a tap of the grip's mapping");
    wheelOut = {};
    for (int i = 0; i < 10; ++i) Poll();
    Check(Poll().bLeftTrigger == 0, "the tap ends");
    wheelOut.buttons = XINPUT_GAMEPAD_DPAD_RIGHT;
    Check((Poll().wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0, "the wheel's switch press reaches the game");
    wheelOut = {};

    // The belt: X is the plain X unless it uses an item in hand; a grip holding an item stays off the pad; the
    // pouch shortcut it plays reaches the game.
    input = {}; input.controllerConnected[0] = input.controllerConnected[1] = true;
    input.buttons = VRB_X;
    beltOut = {};
    Check((Poll().wButtons & XINPUT_GAMEPAD_X) != 0, "X with nothing taken from the belt reaches the game as X");
    beltOut.swallow = VRB_X;
    Check((Poll().wButtons & XINPUT_GAMEPAD_X) == 0, "X that uses a belt item does not also reach the game");
    input.buttons = VRB_A | VRB_X;
    Check((Poll().wButtons & XINPUT_GAMEPAD_A) != 0, "the belt keeps only its own button from the game");
    input.buttons = 0;
    beltOut = {};
    input.handTrigger[HandLeft] = 1.f;   // LeftGrip=LT
    Check(Poll().bLeftTrigger == 255, "a grip the belt does not hold sends its mapping as before");
    beltOut.holdGrip[HandLeft] = true;
    Check(Poll().bLeftTrigger == 0, "a grip holding a belt item stays off the pad");
    input.handTrigger[HandLeft] = 0.f;
    beltOut = {};
    beltOut.buttons = XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_DPAD_RIGHT;
    Check((Poll().wButtons & (XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_DPAD_RIGHT)) == (XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_DPAD_RIGHT),
          "the belt's pouch shortcut (Event Action + D-pad) reaches the game");
    beltOut = {};

    // The body calibration's T-pose panel: it sees the controls as they are, and while it holds the triggers, grips,
    // A and B back the game sees them released; the sticks, X and Y still reach the game.
    input = {}; input.controllerConnected[0] = input.controllerConnected[1] = true;
    input.indexTrigger[HandRight] = 1.f; input.handTrigger[HandLeft] = 1.f;   // RightTrigger=RB, LeftGrip=LT
    input.buttons = VRB_A | VRB_B | VRB_X;
    calibOut = {};
    {
        const auto free = Poll();
        Check((free.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) && free.bLeftTrigger == 255 && (free.wButtons & XINPUT_GAMEPAD_A) &&
                  (free.wButtons & XINPUT_GAMEPAD_B), "with no calibration panel the triggers, grips, A and B reach the game");
    }
    calibOut.trigger[HandRight] = calibOut.grip[HandLeft] = true; calibOut.buttons = VRB_A | VRB_B; calibOut.showing = true;
    {
        const auto held = Poll();
        Check(calibSaw.indexTrigger[HandRight] == 1.f && (calibSaw.buttons & VRB_B), "the calibration reads the controls before anything holds them back");
        Check(!(held.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) && held.bLeftTrigger == 0,
              "a trigger or grip the calibration holds never reaches the game");
        Check(!(held.wButtons & XINPUT_GAMEPAD_A) && !(held.wButtons & XINPUT_GAMEPAD_B), "neither do A (jump) and B (dodge)");
        Check((held.wButtons & XINPUT_GAMEPAD_X) != 0, "X still reaches the game while the panel asks");
    }
    calibOut = {};
    input = {}; input.controllerConnected[0] = input.controllerConnected[1] = true;
    Poll();

    // Valve Index trackpads (LeftPad=MENU, RightPad=DPAD by default): the left press is the menu button -
    // tap START, hold the map - and the right press holds the D-pad in the direction it started in.
    input = {}; input.controllerConnected[0] = input.controllerConnected[1] = true;
    menu = 0;
    input.padForce[HandLeft] = 0.8f;
    Poll();
    input.padForce[HandLeft] = 0.f;
    bool start = false;
    for (int i = 0; i < 3; ++i) start |= (Poll().wButtons & XINPUT_GAMEPAD_START) != 0;
    Check(start, "a tap on the left trackpad is the menu button's tap: START");
    for (int i = 0; i < 10; ++i) Poll();
    input.padForce[HandLeft] = 0.8f;
    bool back = false;
    for (int i = 0; i < 40; ++i) back |= (Poll().wButtons & XINPUT_GAMEPAD_BACK) != 0;
    Check(back, "holding the left trackpad is the menu button's hold: the map");
    input.padForce[HandLeft] = 0.f;
    for (int i = 0; i < 10; ++i) Poll();
    input.pad[HandRight][0] = 0.1f; input.pad[HandRight][1] = -0.9f;
    input.padForce[HandRight] = 0.6f;
    Check((Poll().wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0, "a press on the right trackpad's bottom edge holds D-pad down");
    input.pad[HandRight][0] = 0.9f; input.pad[HandRight][1] = 0.f;
    input.padForce[HandRight] = 0.35f;   // above the release point (0.6 x PadForce = 0.3): still pressed
    const uint16_t rolled = Poll().wButtons;
    Check((rolled & XINPUT_GAMEPAD_DPAD_DOWN) && !(rolled & XINPUT_GAMEPAD_DPAD_RIGHT),
          "the direction stays the one the press started in while the thumb rolls on the pad");
    input.padForce[HandRight] = 0.2f;
    Check((Poll().wButtons & (XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT)) == 0, "letting the pad up releases the D-pad");
    input.pad[HandRight][0] = 0.1f; input.pad[HandRight][1] = 0.1f;
    input.padForce[HandRight] = 0.9f;
    Check((Poll().wButtons & 0x000F) == 0, "a press in the trackpad's middle is no direction");
    input.padForce[HandRight] = 0.f; Poll();
    input.pad[HandRight][0] = 0.f; input.pad[HandRight][1] = 0.9f;
    input.padForce[HandRight] = 0.45f;
    Check((Poll().wButtons & XINPUT_GAMEPAD_DPAD_UP) == 0, "a light touch below PadForce is not a press");
    c.btnRightPad = "B"; VirtualPadInit();
    input.padForce[HandRight] = 0.9f;
    Check((Poll().wButtons & XINPUT_GAMEPAD_B) != 0, "RightPad takes any button name");
    c.btnRightPad = "none"; VirtualPadInit();
    input.padForce[HandRight] = 0.f; Poll();
    input.padForce[HandRight] = 0.9f;
    Check(Poll().wButtons == 0, "RightPad=none presses nothing");
    input = {}; input.controllerConnected[0] = input.controllerConnected[1] = true;
    c.btnRightPad = "DPAD"; VirtualPadInit();
    Poll();
    std::puts("Virtual pad/map zoom and directional dash/roll regression checks passed.");
}
