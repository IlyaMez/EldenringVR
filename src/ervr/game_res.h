#pragma once
#include <cmath>
#include <cstdint>

namespace ervr {

// The oversize auto target: a 16:9 frame carrying the same pixel budget as the headset's own
// (near-square) eye recommendation, times GameResScale. 16:9 on purpose - the game's
// projection carries a constant 1.778 aspect (camera struct +0x54, measured 2026-08-23 even
// on a 0.94 buffer), so a 16:9 frame has exactly the geometry of the fitted frame the stereo
// math is proven on, just denser; the eye-shaped frame of 08-22/23 is where every geometry
// regression lived. Equal pixel budget = equal GPU cost to the shape the user already ran.
// Unrounded: SnapGameRes turns it into a size the game renders 1:1.
inline void Auto169Target(uint32_t eyeW, uint32_t eyeH, double scale, uint32_t& w, uint32_t& h) {
    const double budget = (double)eyeW * (double)eyeH;
    double fh = std::sqrt(budget * 9.0 / 16.0) * scale;
    if (fh < 480.0) fh = 480.0;
    h = (uint32_t)(fh + 0.5);
    w = (uint32_t)(fh * 16.0 / 9.0 + 0.5);
}

// The eye-shaped auto target: one frame of `aspect` (width/height) carrying the same pixel
// budget times `scale`. The aspect is the one symmetric frustum that holds both eyes - the
// widest horizontal half-tangent over the widest vertical one (FrameAspectForFov) - so the
// frame is no wider than an eye needs and every row the 16:9 frame spent on unused side
// field becomes eye pixels: 2026-11-03 on a Quest 3 at the same 7.7 MP, 2154x2327 per eye
// against the 16:9 frame's 1588x1716. Unrounded: SnapGameRes rounds it.
inline void AutoEyeTarget(uint32_t eyeW, uint32_t eyeH, double aspect, double scale,
                          uint32_t& w, uint32_t& h) {
    const double budget = (double)eyeW * (double)eyeH;
    if (!(aspect > 0.3 && aspect < 3.0)) aspect = 1.0;
    double fh = std::sqrt(budget / aspect) * scale;
    if (fh < 480.0) fh = 480.0;
    h = (uint32_t)(fh + 0.5);
    w = (uint32_t)(fh * aspect + 0.5);
}

// Width/height of the one symmetric frustum that contains both eyes' fields of view, from
// the four half-tangents of the widest eye (the runtime's angles as |tan|). 0.964 on a Quest
// 3 through Virtual Desktop (L54 R40 U44 D55: tan 54 / tan 55).
inline double FrameAspectForFov(double tanLeft, double tanRight, double tanUp, double tanDown) {
    const double th = std::fmax(std::fabs(tanLeft), std::fabs(tanRight));
    const double tv = std::fmax(std::fabs(tanUp), std::fabs(tanDown));
    return tv > 1e-3 ? th / tv : 1.0;
}

// A size the game renders 1:1. Its renderer clamps the render size to 16:9 (width = height
// x 16/9, rounded up to a multiple of 4) and pads the rest of the window with black bars, so
// a window even a few pixels off 16:9 renders every target narrower than the back buffer.
// 2026-10-31: a 3632x2040 window rendered its world and its UI at 3628x2040; the HUD layer,
// which looks for a UI target the back buffer's size, never found the UI, and the UI stayed
// in the eye images - the menu outside both eye crops, the map magnified across the view.
// Rounding width and height to multiples of 8 separately is what had put it off 16:9.
// Near-16:9 requests therefore snap to the exact 16:9 grid of 128x72 (both sides stay
// multiples of 8; 3200x1800 is the point the old resolution-table snap always landed on).
// Requests clearly off 16:9 are deliberate - the letterbox patch handles those - and keep
// their shape, rounded to multiples of 8. `maxH` (0 = none) is a ceiling the result stays
// inside: the fitted path's desktop. Pure arithmetic: the DllMain-time config write and the
// worker must land on the identical size.
inline void SnapGameRes(uint32_t& w, uint32_t& h, uint32_t maxH = 0) {
    if (h && std::fabs((double)w / (double)h - 16.0 / 9.0) < 0.01) {
        uint32_t m = (h + 36u) / 72u;
        if (maxH && m * 72u > maxH) m = maxH / 72u;
        m = m < 7u ? 7u : (m > 64u ? 64u : m);   // 896x504 .. 8192x4608
        w = m * 128u;
        h = m * 72u;
        return;
    }
    w = (w + 4u) & ~7u;
    h = (h + 4u) & ~7u;
    w = w < 640u ? 640u : (w > 8192u ? 8192u : w);
    h = h < 480u ? 480u : (h > 8192u ? 8192u : h);
}

} // namespace ervr
