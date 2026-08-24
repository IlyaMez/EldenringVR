#pragma once
#include <windows.h>
#include <xinput.h>
#include <cstdint>

namespace ervr {

// Builds a synthetic XINPUT_GAMEPAD from VR controller state + gestures.
// Update() runs on the ERVR worker thread; Merge() runs inside the XInput hook.
void VirtualPadInit();
void VirtualPadUpdate(double now);
// Merges the current virtual pad into `st`. Returns true if the virtual pad is
// active (a VR runtime is attached and at least one controller is connected).
bool VirtualPadMerge(XINPUT_STATE& st);
// Rumble from the game (XInputSetState) -> controllers.
void VirtualPadRumble(float left, float right);
unsigned long long VirtualPadLastATick();   // tick of the last A press sent to the game (0 = never)
// The processed move stick (after forward-mapping) and sprint state, for Locomotion=direct.
struct MoveIntent { float x = 0, y = 0; bool sprint = false; bool valid = false; };
MoveIntent VirtualPadGetMove();
}
