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

// [VR] GameRes: overwrite the game's 1920x1080 resolution-table entry with the VR render
// target and bypass the 16:9 aspect clamp, so the user can pick a headset-sized (near-
// square) resolution in the video settings without touching DSR. Idempotent; re-applies
// when the target changes (ini hot-reload, fresh headset geometry). Worker thread.
void PatchesApplyGameResolution();
// The runtime's recommended per-eye size, once known. Cached to ERVR.cache.ini so the NEXT
// boot can patch the table before the game reads its saved graphics settings; re-applies
// the resolution patch live when it changes an "auto" target.
void PatchesNoteHmdEyeSize(uint32_t w, uint32_t h);
}
