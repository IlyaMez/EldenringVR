#pragma once
#include <cstdint>

namespace ervr {

// Which render target the HUD layer lifts the UI from - the rules, free of ReShade so they
// can be tested without a GPU (tools/test_hud_target.cpp). hud.cpp feeds them its census.

// One contiguous run of draws onto one render target, in submission order.
struct HudRun {
    uint64_t res = 0;
    int      draws = 0;
    bool     backBuffer = false;
    bool     texture2d = true;
    uint32_t w = 0, h = 0;
    uint32_t fmt = 0;   // DXGI_FORMAT; reshade::api::format uses the same values
};

// Formats a UI target can have. ELDEN RING draws its UI onto a full-size R8G8B8A8 target
// (1.16.2 and 1.17.2 alike) and the boot/title screens onto the back buffer (R10G10B10A2 or
// 8-bit). The scene's own full-size buffers are float: 2026-11-01, in two boss phase-2
// cutscenes, the relearn took a full-size R11G11B10_FLOAT target with 2 draws a frame for
// the UI, and from the first stereo pair on one of the SCENE's draws went onto the HUD
// layer every frame while the real HUD stayed in the eye images - "rendering broke until I
// respawned" (the respawn's loading screen was the next relearn).
inline bool HudUiFormat(uint32_t fmt) {
    switch (fmt) {
    case 23: case 24:            // R10G10B10A2 typeless / unorm
    case 27: case 28: case 29:   // R8G8B8A8 typeless / unorm / srgb
    case 87: case 90: case 91:   // B8G8R8A8 unorm / typeless / srgb
        return true;
    default:
        return false;
    }
}

// The run the UI is on now: the last full-size target in the frame that took at least
// `want` draws and could hold a UI. -1 = no candidate, leave the target where it is.
// `rejected` (optional) gets the latest full-size multi-draw run passed over for its format.
inline int HudPickUiTarget(const HudRun* runs, int n, int want, uint32_t bbW, uint32_t bbH,
                           int* rejected = nullptr) {
    if (rejected) *rejected = -1;
    for (int i = n - 1; i >= 0; --i) {
        const HudRun& r = runs[i];
        if (r.draws < want) continue;
        if (r.backBuffer) return i;
        // Full frame size only. A half-res bloom or shadow target can easily take a dozen
        // draws in a row and would otherwise look just like a UI batch.
        if (!r.texture2d || r.w != bbW || r.h != bbH) continue;
        if (!HudUiFormat(r.fmt)) {
            if (rejected && *rejected < 0) *rejected = i;
            continue;
        }
        return i;
    }
    return -1;
}

// A pass with no UI draw, on a LEARNED target that still took the frame's own draws (the
// tonemap that precedes the UI): the UI is absent, not moved - a cutscene, a fade, the HUD
// hidden. The target only moves when the old one stops being drawn to at all (a load, the
// title, a resize), so there is nothing to relearn here, and relearning anyway is how the
// cutscene above found a scene buffer: with the UI target down to its one tonemap draw, the
// last full-size multi-draw run in the frame was the scene's.
inline bool HudUiAbsentNotMoved(bool learnedTarget, int drawsIntoTarget) {
    return learnedTarget && drawsIntoTarget > 0;
}

} // namespace ervr
