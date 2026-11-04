#pragma once
// In-process ports of uberhalit/EldenRingFpsUnlockAndMore (MIT) .text patches.
// Driven by [Patches] in ERVR.ini; everything defaults off.
#include <cstdint>
namespace ervr {
void PatchesInit();   // apply once, after the game module is loaded (worker thread)

// The framelock imm32 - the float the game stores as its per-frame delta time (and frame
// limiter target) every frame. The full-frame second pass swaps it to ~0 for the pass and
// restores it, so pass 2 is a complete engine frame that advances nothing.
// Resolved lazily on first use; null when the pattern is missing on this build.
float* PatchesFrameDtImm();
// Patch the imm to `dt` (protection dance + icache flush). False when unresolved/failed.
bool  PatchesWriteFrameDt(float dt);
// [VR] LimiterFollowsHeadset: given the headset's refresh (0 = unknown), raise the frame
// limiter just above it if it sits lower. Cheap to call every worker tick; acts on changes.
void  PatchesFollowHeadsetRefresh(float hz);

// [VR] GameRes: overwrite the game's 1920x1080 resolution-table entry with the VR render
// target and bypass the 16:9 aspect clamp, so the user can pick a headset-sized (near-
// square) resolution in the video settings without touching DSR. Idempotent; re-applies
// when the target changes (ini hot-reload, fresh headset geometry). Worker thread.
void PatchesApplyGameResolution();
// Every swapchain (re)creation reports its size here. Once the OVERSIZED back buffer has
// actually appeared, the desktop-size lies (monitor rect, current display mode) disarm for
// the rest of the session: they are load-bearing for the settings-apply acceptance but the
// game also polls the same metrics during play, and a standing height lie is the measured
// cause of the vertical head-motion warp (2026-08-22, and once before at 1496x1360).
void PatchesNoteBackbufferSize(uint32_t w, uint32_t h);
// The 16:9 letterbox patch is in: a window off 16:9 renders full-size instead of a 16:9
// frame inside black bars. False when it was never needed, or is unavailable.
bool PatchesLetterboxRemoved();
// DllMain(PROCESS_ATTACH)-safe optimistic GraphicsConfig.xml write - kernel32 file APIs
// only, no logging, no patching. Must beat the game's boot-time read of the file, which
// even the first worker-thread statements measurably lose to. `module` is ERVR's own
// HMODULE (void* keeps windows.h out of this header). See patches_er.cpp.
void PatchesEarlyGraphicsConfigWrite(void* module);
// Opt the process into per-monitor DPI awareness, as early as possible. ELDEN RING is
// DPI-virtualized by default: on a 3440x1440 desktop at 150% scaling it measures the screen
// as ~2293x960, so its WINDOWED resolution list stops at 1680x945 and the back buffer it
// hands us is that small - then DWM upscales it. That is the single biggest source of VR
// blur; awareness alone restores the full-size entries.
void PatchesMakeDpiAware();
// The runtime's recommended per-eye size, once known. Cached to ERVR.cache.ini so the NEXT
// boot can patch the table before the game reads its saved graphics settings; re-applies
// the resolution patch live when it changes an "auto" target.
void PatchesNoteHmdEyeSize(uint32_t w, uint32_t h);
// The width/height of the one symmetric frustum holding both eyes, from the runtime's FOV.
// Cached as HmdFrameAspect for the GameResShape=eye auto frame; lands on the next launch.
void PatchesNoteHmdFrameAspect(float aspect);
}
