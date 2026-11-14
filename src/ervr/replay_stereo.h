#pragma once
// Single-frame stereo by GPU replay (work in progress).
//
// The engine runs ONE frame per headset frame, for the left eye. Its recorded D3D12 command
// lists are then executed a second time with the frame's camera constants moved to the right
// eye - every pass, lighting and post effect included, re-run from a camera 64 mm to the side,
// with no shader changes and none of the second engine pass's CPU cost.
//
// Stage 1 (this file today): the submission census - which queues the game uses, how its
// command lists, fence signals and waits line up against its presents.
#include "reshade_addon.h"

namespace ervr {

// Render thread, first thing in every present (before the stereo layer). Installs the queue
// hooks on the first call and drives the census windows.
void ReplayStereoOnPresent(const Dx12Frame& f);
// Marks the present thread as inside ERVR's own present work (true at the start of the
// present event, false at its end): the census labels those submissions as ours.
void ReplayStereoPresentScope(bool inside);
// ReShade resource events (any thread): CPU-visible buffers are tracked by GPU address so a
// constant buffer bound by address can be read. `heap` is reshade::api::memory_heap.
void ReplayStereoOnInitResource(ID3D12Resource* r, int heap, bool buffer);
// The game's D3D12 device was created (ReShade init_device): descriptor hooks go in before its first view.
void ReplayStereoOnInitDevice(ID3D12Device* d);

// ---- [VR] StereoMode=replay: the stereo layer's side --------------------------------------------------
// True once shadow recording runs (hooks in, first frame seen).
bool ReplayStereoActive();
// Inside the present, before the first eye's blit: whether this frame can be replayed (a complete set of
// shadows, the world steady, the frame's camera known, the patch ready). Nothing is consumed.
bool ReplayStereoCanSubmit();
// Why the last ReplayStereoCanSubmit said no, in words for the log ("" when it said yes).
const char* ReplayStereoBlocker();
// Inside the present, after the first eye's blit: the GPU camera patch and the frame's replay go on the
// queue (the second eye lands in the back buffer). True when they did; false = no second eye this frame.
bool ReplayStereoSubmitSecondEye(const Dx12Frame& f);
// End of the present event: if the stereo layer did not replay this frame, its shadows are dropped and the
// game's held fence signals released.
void ReplayStereoPresentEnd(const Dx12Frame& f);
// Frame thread, at the end of every engine frame: the camera the frame was drawn from and the second eye's
// offset from it, for the next present's patch.
void ReplayStereoFrameEnd();
// 3 under StereoMode=replay, else [Experimental] ReplayStereo (bench levels).
int ReplayStereoLevel();
// Game thread, camera write: metres to move the game's own camera along its right axis (bench capture only; else 0).
float ReplayStereoBenchCameraShift();
// ... along a skew camera-space direction (0.8 right, 0.36 up, 0.48 forward) instead of the right axis (census dumps).
bool ReplayStereoBenchShiftSkew();
void ReplayStereoOnDestroyResource(ID3D12Resource* r);

}
