#pragma once
// In-process ports of uberhalit/EldenRingFpsUnlockAndMore (MIT) .text patches.
// Driven by [Patches] in ERVR.ini; everything defaults off.
namespace ervr {
void PatchesInit();   // apply once, after the game module is loaded (worker thread)

// The framelock imm32 - the float the game stores as its per-frame delta time (and frame
// limiter target) every frame. The full-frame second pass swaps it to ~0 for the pass and
// restores it, so pass 2 is a complete engine frame that advances nothing.
// Resolved lazily on first use; null when the pattern is missing on this build.
float* PatchesFrameDtImm();
// Patch the imm to `dt` (protection dance + icache flush). False when unresolved/failed.
bool  PatchesWriteFrameDt(float dt);
}
