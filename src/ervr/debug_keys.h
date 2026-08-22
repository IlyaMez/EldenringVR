#pragma once
// Numpad hotkeys for the stereo A/B experiments.
//
// The settings that need testing (eye routing, which task groups the draw-only pass keeps)
// hot-reload from the ini, but editing an ini is impossible while wearing a headset. These
// keys toggle the same values live and confirm each change with a haptic pattern, so the
// experiment can be run without taking the headset off. GetAsyncKeyState is global, so the
// presses land whether or not the game window has focus.
//
//   Numpad 1  PresentLag        0 <-> 1        (which pass feeds which eye)
//   Numpad 2  Pass2Keep         cycles the candidate task-group sets
//   Numpad 3  pitch probe       cycles off -> tilt pass 2 -> tilt pass 1 (which content
//                               lands where, and whether the render camera is captured
//                               per pass or once per sim frame)
//   Numpad 4  GpuSync           cycles 0 -> 1 -> 2 (drain the GPU between the engine passes)
//   Numpad 5  Pass2FullFrame    0 <-> 1 (pass 2 = complete engine frame at dt~0)
//   Numpad 7  HUD layer        on <-> off (the UI on its own world quad, or back in the frame)
//   Numpad 8  HUD FirstDraw    +1, wrapping at 8 -> 0: how many draws into the frame's final
//                              target stay in the frame. Step it until the UI leaves the world
//                              image and lands on the panel, then bake it into [HUD] FirstDraw
//   Numpad .  re-centre the HUD panel on the current view
//   Numpad 6 / 9  hand grip calibration: -15 / +15 deg pitch on the controller->hand
//                 mapping (HandRotOffsetX), for dialling out the "weapon points at me"
//                 grip-pose offset live; bake the logged winner into the ini
//   Numpad 0  re-announce the current state (haptic + log)
//
// Haptics: one pulse = value returned to default/off, two = alternative/on, three = the
// third or later entry in a cycle.
namespace ervr {

void DebugKeysTick();          // worker thread; edge-detected, cheap

// -1 / negative means "not overridden - use the ini value".
int   DebugKeysPresentLag();
float DebugKeysPass2Pitch();
float DebugKeysPass1Pitch();   // capture probe: tilts the REAL pass's camera
int   DebugKeysGpuSync();
int   DebugKeysPass2FullFrame();
// True (and fills *outDeg) only while the hotkeys override the ini's HandRotOffsetX.
bool  DebugKeysHandRotX(float* outDeg);
}
