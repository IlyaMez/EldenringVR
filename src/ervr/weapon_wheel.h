#pragma once
// The weapon wheel. Hold a grip with that hand still and a wheel of the hand's three weapon slots
// opens around it, showing the game's own item icons (item_icons.h). Move the hand over a weapon and
// let go: ERVR presses the game's own weapon-switch button (D-pad right for the right hand, left for
// the left) until ChrAsm's active slot reads the one picked. Letting go in the middle switches nothing.
//
// The grip has other jobs, and those win. Closed at the other weapon's hilt it is the two-hand grab;
// while two-handing, in a menu or with nothing else in that hand the wheel stays shut and the grip does
// exactly what it always did, and a moving hand never opens it. Otherwise a squeeze the wheel may still
// claim is held back from the grip's [Buttons] mapping, and a quick one is passed on as a single tap
// when it opens again.
//
// The wheel hangs where the hand was when it opened, facing the eyes, in the headset's own space -
// walking or turning in the game does not move it. A cursor dot marks the hand over it, because the
// rendered hand is behind the layer.
//
// Threads: WeaponWheelUpdate on the pad worker, WeaponWheelGameFrame on the game thread (the equipment
// it reads); the image and its XR layers go out through overlay_layers.h.
#include "vr_backend.h"
#include <cstdint>

namespace ervr {

struct WheelPadOut {
    bool holdGrip[2] = {false, false};   // keep the grip's [Buttons] mapping off the pad this tick
    bool tapGrip[2] = {false, false};    // a quick squeeze ended: send that mapping as one tap now
    uint16_t buttons = 0;                // the game's weapon-switch presses (D-pad left / right)
    float hapticAmp[2] = {0.f, 0.f}, hapticSec[2] = {0.f, 0.f};
};
// Worker, once per pad update, after CombatPollHands. `menu` = any menu, the map, or menu-nav mode.
WheelPadOut WeaponWheelUpdate(const VRTracking& tr, const VRInputState& in, double now, bool menu);
// Game thread, once per real frame (never pass 2): ChrAsm's slots and their icon ids.
void WeaponWheelGameFrame(uint8_t* player);
// The hand whose wheel is up: physical combat leaves it alone (moving onto a weapon is not a swing).
bool WeaponWheelOwnsHand(int hand);
// A switch is under way (the wheel may press the D-pad): the belt holds its pouch chord back meanwhile,
// since the game reads a D-pad press under a held Event Action button as a pouch item.
bool WeaponWheelBusy();

struct WheelStats {
    int  openHand = -1;          // the hand whose wheel is open, -1 = none
    long opens = 0, switches = 0, failed = 0;   // since the last read
};
WheelStats WeaponWheelGetStats();

}
