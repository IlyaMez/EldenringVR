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

inline bool CanPinMovementFacing(const MovementFacingState& s, bool dodgeSprintDown) {
    constexpr uint64_t dodgeRequests = (1ull << 5) | (1ull << 16) | (1ull << 17);
    constexpr uint64_t dodgeOrNoTurning = (1ull << 1) | (1ull << 15);
    // Leave native facing alone from the button press through startup, active
    // motion and recovery. Unknown game state must also leave physics alone.
    return s.valid && s.movementCancelable && !dodgeSprintDown &&
           !(s.requests & dodgeRequests) && !(s.movement & (1u << 2)) &&
           !(s.modifiers & dodgeOrNoTurning);
}

} // namespace ervr
