#pragma once
// Camera-capture hunt ([Experimental] CamWatch).
//
// Measured fact (2026-08-20, in-headset): tilting the REAL pass's camera tilts BOTH eyes -
// the renderer captures its camera exactly once per sim frame, from somewhere no Pass2Keep
// task-group set covers, so nothing written for the draw-only second pass can ever reach its
// image through the camera objects alone.
//
// This probe finds the capture: PAGE_GUARD on the render camera's matrix+fov, a vectored
// handler records the RIP, registers and instruction bytes of every reader (single-step
// re-arms the guard), and a census is dumped after a few seconds. The once-per-frame reader
// between the camera step and the draw is the capture; the register holding its destination
// names the renderer's private camera copy - the thing pass 2 must write instead.
//
// Flat-friendly: the capture happens every normal frame, no headset needed.
namespace ervr {
void CamWatchTick();   // worker thread; spawns its own thread once, no-op unless enabled
}
