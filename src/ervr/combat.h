#pragma once
#include "vr_backend.h"
#include <cstdint>

namespace ervr {

// Physical melee combat: instead of pressing R1/R2 (which plays an attack animation
// with animation-timed hitboxes), ERVR invokes the game's own TAE "AttackBehavior"
// event (event 1) on the player every frame while a controller swing is in progress.
// The game then does what it does for a real attack: looks up BehaviorParam_PC ->
// AtkParam_Pc for the current weapon, sweeps the weapon dummy-poly capsule (which
// follows the controller-driven arm bones), applies damage/poise/status, hitstop,
// hit-list dedupe and stamina cost. No attack animation plays.
//
// Judge IDs (which BehaviorParam row = which attack) are learned by watching the
// events the game fires for the player when ERVR presses the light/heavy/left
// buttons in "buttons" mode, or configured in [Combat].

bool CombatInit(uint8_t* moduleBase, bool versionOk);   // installs the TAE event hook
void CombatShutdown();

// Worker thread (poll rate): feed controller poses; runs the swing state machine.
void CombatPollHands(const VRTracking& tr, const VRInputState& in, double now);
// True while physical combat is active: virtual_pad must not pulse attack buttons for swings.
bool CombatHandlesSwings();
// Called by virtual_pad whenever it pulses a button (used to learn judge IDs).
void CombatNotifyButton(uint16_t xinputMask, double now);
// XInput button mask virtual_pad must not emit while physical combat is active (attack buttons -> animations).
uint16_t CombatBlockedButtons();
// Haptic requests raised by the game-thread side (stamina empty, swing armed); worker drains them.
// Returns a bitmask: bit0 = left hand, bit1 = right hand, bit2/3 = "fail" flavour for left/right.
uint32_t CombatTakeHapticRequests();

// Game thread, once per rendered frame with the main player ChrIns.
void CombatGameFrame(uint8_t* player, uint32_t frame);

struct CombatStatus {
    bool taeHooked = false;
    bool physical = false;          // physical mode currently active
    bool learnedLight = false, learnedHeavy = false, learnedLeft = false;
    long injected = 0;              // synthetic AttackBehavior calls since last query
    long swings = 0;                // swings started since last query
    int  stamina = -1;
};
CombatStatus CombatGetStatus();
}
