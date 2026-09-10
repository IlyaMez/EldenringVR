#include "movement_facing.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using namespace ervr;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}

int main() {
    ActionQueueSnapshot cancels;
    cancels.valid = true; cancels.count = 2;
    cancels.entries[0] = {100, 0, (1ull << 35) | (1ull << 36)};
    cancels.entries[1] = {200, 0, 1ull << 35};
    Check(!MovementCancelOpen(6, true, 200, cancels),
          "another animation's move cancel must not re-pin a roll during recovery");
    Check(!MovementCancelOpen(6, true, 300, cancels),
          "missing current animation must leave native motion alone");
    cancels.entries[1].actions |= 1ull << 36;
    Check(MovementCancelOpen(0, true, 200, cancels),
          "queue-mode recovery must resume pinning even without legacy TAE bits");
    Check(!MovementCancelOpen(1u << 11, true, 200, cancels),
          "HKS cancel-disable must veto an otherwise open movement cancel");
    cancels.valid = false;
    Check(!MovementCancelOpen(6, true, 200, cancels), "unreadable queues must leave native facing alone");
    Check(MovementCancelOpen(6, false, -1, cancels), "legacy recovery must work without queue storage");
    Check(!MovementCancelOpen(2, false, -1, cancels), "one frame of movement cancel is insufficient");
    Check(!MovementCancelOpen(4, false, -1, cancels), "expired movement cancel must not re-pin facing");

    MovementFacingState s;
    Check(!CanPinMovementFacing(s, false), "missing game state must not overwrite physics facing");
    s.valid = s.movementCancelable = true;
    Check(CanPinMovementFacing(s, false), "ordinary walking retains body-facing alignment");
    Check(!CanPinMovementFacing(s, true), "B press must yield before the game consumes the input");
    s.movement = 4;
    Check(!CanPinMovementFacing(s, false), "native sprint must steer even after the virtual B state changes");
    s.movement = 0;
    for (uint64_t request : {1ull << 5, 1ull << 16, 1ull << 17}) {
        s.requests = request;
        Check(!CanPinMovementFacing(s, false), "released or buffered dodge must retain the game's turn");
    }
    s.requests = 0; s.movementCancelable = false; s.modifiers = 2;
    Check(!CanPinMovementFacing(s, false), "active roll must keep its native direction");
    s.modifiers = 0;
    Check(!CanPinMovementFacing(s, false), "roll motion outlasts its invulnerability flag and B release");
    s.movementCancelable = true; s.modifiers = 1ull << 15;
    Check(!CanPinMovementFacing(s, false), "TAE turn restrictions must survive movement recovery");
    s.modifiers = 0;
    Check(CanPinMovementFacing(s, false), "walking alignment resumes once the native action permits it");
    std::puts("Directional dash/roll facing regression checks passed.");
}
