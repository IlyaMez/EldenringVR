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
}
