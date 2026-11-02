#pragma once
// Body haptics in ELDEN RING ([bHaptics]): what the game does to the Tarnished, posted to the bHaptics
// output (bhaptics_output.h), whose patterns body_haptics.h makes.
//
//   combat.cpp     blows landed, blocks and deflects, the two-hand grab, swings refused for stamina,
//                  bullets leaving the player (spells, incantations, arrows, throws)
//   game thread    HP: blows taken (placed on the side the attacker stands, from the game's own
//                  last_hit_by), heals, death; the game's is_falling flag and the player's velocity:
//                  the wind of a fall and the landing
//   xinput_hook    the game's own rumble, onto the vest
//   pad worker     what goes on (in the world, HP, falling, rumble) and where the torso faces
//
// Off with [bHaptics] Level=0: bhaptics_library.dll is then never loaded.
#include "body_haptics.h"
#include <cstdint>

namespace ervr {

// Main worker, every tick: starts the output the first time Level > 0, feeds it the state, logs what
// the bHaptics Player and its devices do.
void BodyHapticsTick(double now);
// Main worker, with the 5 s heartbeat (debug).
void BodyHapticsHeartbeat();
// Game thread, every real frame (never pass 2), with the main player ChrIns; the Havok hook calls it
// several times a frame, `frame` keeps it to once.
void BodyHapticsGameFrame(uint8_t* player, uint32_t frame);
// Any thread: something happened. Cheap and dropped while body haptics are off.
void BodyHapticsPost(BodyKind kind, int hand = -1, float strength = 1.f);
// XInputSetState: the game's rumble for pad 0, 0..1 per motor (left the heavy one).
void BodyHapticsRumble(float left, float right);

}
