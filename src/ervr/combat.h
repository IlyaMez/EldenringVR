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
// XInput buttons virtual_pad must HOLD: the guard button while the left-hand shield is raised.
uint16_t CombatHeldButtons();
// Pad worker: publish whether any game menu is open (game state or menu-nav mode). While true,
// physical combat stands down completely - no swings, no injection, no guard hold, no chords,
// param edits restored - so the game never sees combat state under a menu (the equipment menu
// refuses weapon changes while it does). CombatMenuOpen returns the gate's current state.
void CombatSetMenuOpen(bool open);
bool CombatMenuOpen();
// Buttons combat FORCES on the pad this tick - the game's own chords (Y + RB/LB two-hand toggle, the
// skill button, a learn press) - applied AFTER the attack-button block. Worker thread; drives the
// chord timing, so call it exactly once per pad update.
uint16_t CombatForcedButtons(double now);
// Off-hand ride: while two-handing, the free hand's arm target sits on the weapon hand's hilt axis,
// `alongToPommel` metres from that controller toward the pommel - down the controller's handle
// (-Y) when `handleAxis`, else back along its aim axis (+Z). game_er applies it. False otherwise.
bool CombatTwoHandPin(int* pinHand, int* weaponHand, float* alongToPommel, bool* handleAxis);
// Weight the hand is carrying (EquipParamWeapon weight; both hands share it when two-handing;
// 0 = empty/unknown). Drives the inertial arm in game_er.
float CombatHandWeight(int hand);
// Haptic requests raised by the game-thread side (stamina empty, swing armed); worker drains them.
// Returns a bitmask: bit0 = left hand, bit1 = right hand, bit2/3 = "fail" flavour for left/right,
// bit4 = blocked hit (left), bit5 = deflect (left).
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
    bool paramsReady = false;       // SoloParamRepository located (kinetic scaling possible)
    float lastQuality = 0.f, lastMult = 0.f;   // last strike's quality and damage multiplier
    bool shield = false, guarding = false;     // shield in the left hand / guard currently raised
    float blockQuality = 0.f;
    int  armStyle = 0;              // ChrAsm arm style: 0 empty, 1 one-handed, 2 left two-handed, 3 right two-handed
    bool twoHandWanted = false;     // the grab says two-hand
    int  fp = -1;
    const char* skill = "idle";     // skill state: idle | recording | replaying | window
    int  skillsRecorded = 0;        // ash-of-war recordings held this session
};
CombatStatus CombatGetStatus();
}
