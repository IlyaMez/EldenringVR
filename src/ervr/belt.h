#pragma once
// The belt. The four pouch items the game gives a shortcut (top left, top right, middle left, middle right in the
// pouch) hang around the waist as objects with the game's own item icons. Close a grip on one to take it, press
// X (UseButton) while holding it to use it, open the hand to put it back. Using it is the game's own pouch
// shortcut - ERVR holds Event Action (Y) and presses that slot's D-pad direction - so the game's rules,
// animation and counts all apply. X with nothing taken from the belt is the plain X it always was.
//
// The belt hangs under the neck, not the eyes, so looking down at it does not move it, and it turns with the
// body: the head may look around within FollowDegrees of it, and it settles under the head once the head keeps a
// heading. While a hand reaches for it, or the eyes are down on it, it holds still.
//
// A grip that takes an item is the belt's until it opens: not a two-hand grab, not a weapon wheel, not the grip's
// [Buttons] mapping, and that hand does not swing. Every use is checked against what the game did next (the
// item's count, the item the game queued, a selection a plain D-pad press would move) and said in the log.
//
// Threads: BeltUpdate on the pad worker, BeltGameFrame on the game thread (the pouch it reads); the item images
// and their XR layers go out through overlay_layers.h.
#include "vr_backend.h"
#include <cstdint>

namespace ervr {

struct BeltPadOut {
    uint32_t swallow = 0;                // VR buttons (VRB_*) kept from the game this tick: UseButton while it uses an item
    bool holdGrip[2] = {false, false};   // a hand holding an item: its grip's [Buttons] mapping stays off the pad
    uint16_t buttons = 0;                // the pouch shortcut being played: Event Action + a D-pad direction
    float hapticAmp[2] = {0.f, 0.f}, hapticSec[2] = {0.f, 0.f};
};
// Worker, once per pad update, BEFORE CombatPollHands: a grip closed on a belt item is never a two-hand grab or a
// swing. `menu` = any menu, the map, or menu-nav mode.
BeltPadOut BeltUpdate(const VRTracking& tr, const VRInputState& in, double now, bool menu);
// Game thread, once per real frame (never pass 2): the pouch, its counts, and what a use changed.
void BeltGameFrame(uint8_t* player);
// The hand holds a belt item: no swing, no two-hand grab, no weapon wheel for it. Any thread.
bool BeltOwnsHand(int hand);
// The pouch shortcut is being pressed (Event Action held): a D-pad press from anything else now would use a pouch
// item, so the weapon wheel's switch waits. Any thread.
bool BeltPadBusy();

struct BeltStats {
    int held[2] = {-1, -1};   // the slot in each hand, -1 = none
    int items = 0;            // pouch slots 1-4 holding an item
    long grabs = 0, uses = 0, confirmed = 0, refused = 0;   // since the last read
};
BeltStats BeltGetStats();

}
