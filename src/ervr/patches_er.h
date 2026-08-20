#pragma once
// In-process ports of uberhalit/EldenRingFpsUnlockAndMore (MIT) .text patches.
// Driven by [Patches] in ERVR.ini; everything defaults off.
namespace ervr {
void PatchesInit();   // apply once, after the game module is loaded (worker thread)
}
