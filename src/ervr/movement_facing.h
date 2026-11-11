#pragma once
#include "action_queue_snapshot.h"

namespace ervr {

// fromsoftware-rs CSChrActionRequestModule / CSChrActionFlagModule. Movement
// cancels describe the whole action, unlike dodge invulnerability flags, which
// end before the roll's root motion does.
struct MovementFacingState {
    bool valid = false, movementCancelable = false;
    uint64_t requests = 0, modifiers = 0;
    uint32_t movement = 0;
};

inline bool MovementCancelOpen(uint32_t tae, bool queueMode, int32_t currentTae,
                               const ActionQueueSnapshot& cancels) {
    if (tae & (1u << 11)) return false;   // HKS cancel-disable
    if (!queueMode) return (tae & 6u) == 6u;
    if (!cancels.valid || currentTae < 0) return false;
    constexpr uint64_t movementBothFrames = (1ull << 35) | (1ull << 36);
    for (size_t i = 0; i < cancels.count; ++i)
        if (cancels.entries[i].tae == currentTae &&
            (cancels.entries[i].actions & movementBothFrames) == movementBothFrames) return true;
    return false;
}

constexpr uint64_t kDodgeRequests = (1ull << 5) | (1ull << 16) | (1ull << 17);
constexpr uint64_t kDodgeModifier = 1ull << 1;

// A dodge is the game's free movement: the body turns toward the move stick as the roll starts, then rolls
// straight ahead. The strafe flag (ChrCtrl unlocked = 0) holds the body where it faces, so with it held every
// roll went where you look whatever the stick said. From the button's press (the game's dodge request, or ERVR's
// own B while the game has not seen it yet) through the roll's start, the flag is left to the game - erfps2 lets
// go of its lock state the same way while an action request is pending.
inline bool DodgeOwnsMovement(const MovementFacingState& s, bool dodgeSprintDown) {
    return dodgeSprintDown || (s.valid && ((s.requests & kDodgeRequests) || (s.modifiers & kDodgeModifier)));
}

inline bool CanPinMovementFacing(const MovementFacingState& s, bool dodgeSprintDown) {
    constexpr uint64_t dodgeRequests = kDodgeRequests;
    constexpr uint64_t dodgeOrNoTurning = kDodgeModifier | (1ull << 15);
    // Leave native facing alone from the button press through startup, active
    // motion and recovery. Unknown game state must also leave physics alone.
    return s.valid && s.movementCancelable && !dodgeSprintDown &&
           !(s.requests & dodgeRequests) && !(s.movement & (1u << 2)) &&
           !(s.modifiers & dodgeOrNoTurning);
}

} // namespace ervr
