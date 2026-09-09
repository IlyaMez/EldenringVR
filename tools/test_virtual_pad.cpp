#include "virtual_pad.h"
#include "vr_backend.h"
#include "game_er.h"
#include "game_memory.h"
#include "combat.h"
#include "config.h"
#include "log.h"
#include <cstdio>
#include <cstdlib>

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
    std::puts("Virtual pad/map zoom regression checks passed.");
}
