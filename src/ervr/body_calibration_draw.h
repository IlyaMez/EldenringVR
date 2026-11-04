#pragma once
// What the body calibration shows (body_calibration.h): the panel - instructions, what to change, the hold's bar,
// a figure in a T-pose; the result once measured - and the ring at each controller. Composed on the CPU in the
// launcher's palette (overlay_canvas.h) and written in the stroke font (overlay_text.h). Free of game state, so
// it can be drawn and looked at outside the game.
#include "body_calibration_logic.h"
#include "overlay_canvas.h"
#include "overlay_text.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ervr {

constexpr int kCalibPanelPxW = 1024, kCalibPanelPxH = 512;   // the panel's image: 1.2 x 0.6 m, ~850 px a metre
constexpr int kCalibRingPx = 128;
constexpr float kCalibRingM = 0.2f;                          // a ring's quad, metres across
constexpr int kCalibSteps = 50;                              // the hold drawn in fiftieths
constexpr uint32_t kCalibAmber = 0xE8B04A;                   // what to change: a warmer gold than the title's

struct CalibPanelState {
    CalibPhase phase = CalibPhase::idle;
    CalibHint hint = CalibHint::none;
    bool ready = false;   // both arms in place
    int progress = -1;    // kCalibSteps-ths of the hold
    int eyeMm = 0, armMm = 0;   // the result, once done
    bool operator==(const CalibPanelState& o) const {
        return phase == o.phase && hint == o.hint && ready == o.ready && progress == o.progress && eyeMm == o.eyeMm && armMm == o.armMm;
    }
};

namespace calib_draw {
// Panel coordinates: metres, x to the player's right and y up from the panel's centre.
struct Pen {
    Canvas& cv;
    float k = kCalibPanelPxW / kCalibPanelWidth;
    float X(float x) const { return kCalibPanelPxW * 0.5f + x * k; }
    float Y(float y) const { return kCalibPanelPxH * 0.5f - y * k; }
    // On the baseline y, centred on x (align 0.5) or from it (0); shrunk to `width` metres.
    void Text(const char* s, float x, float y, float height, const Px& c, float align = 0.5f, float width = kCalibPanelWidth - 0.1f) const {
        CanvasText(cv, s, X(x), Y(y), TextFit(s, height * k, width * k), c, align);
    }
    void Line(float x0, float y0, float x1, float y1, float width, const Px& c) const { cv.Stroke(X(x0), Y(y0), X(x1), Y(y1), width * k, c); }
    void Box(float x0, float y0, float x1, float y1, const Px& c) const { cv.Rect(X(x0), Y(y1), X(x1), Y(y0), c); }
};
// The figure: left of the instructions, clear of the hint line under it. F() places a point of a figure 0.3 m tall
// (head top to feet) given relative to its waist.
constexpr float kFigureX = -0.44f, kFigureY = 0.03f, kFigureScale = 0.75f, kStroke = 0.01f;
inline float FX(float x) { return kFigureX + x * kFigureScale; }
inline float FY(float y) { return kFigureY + y * kFigureScale; }
} // namespace calib_draw

// The panel's background, frame and the parts that do not change while it asks (`done` false) or once it has
// measured (true).
inline void CalibDrawBase(Canvas& cv, bool done) {
    using calib_draw::FX; using calib_draw::FY; using calib_draw::kFigureScale; using calib_draw::kStroke;
    cv.Reset(kCalibPanelPxW, kCalibPanelPxH);
    const calib_draw::Pen p{cv};
    const float w = kCalibPanelWidth * 0.5f, h = kCalibPanelHeight * 0.5f;
    p.Box(-w, -h, w, h, Col(kDark, 0.88f));
    const float edge = 0.004f, inset = 0.012f;
    p.Box(-w, h - edge, w, h, Col(kGold, 0.9f));
    p.Box(-w, -h, w, -h + edge, Col(kGold, 0.9f));
    p.Box(-w, -h, -w + edge, h, Col(kGold, 0.9f));
    p.Box(w - edge, -h, w, h, Col(kGold, 0.9f));
    p.Box(-w + inset, h - inset - 0.0015f, w - inset, h - inset, Col(kGold, 0.35f));
    p.Box(-w + inset, -h + inset, w - inset, -h + inset + 0.0015f, Col(kGold, 0.35f));
    if (done) return;
    p.Text("BODY CALIBRATION", 0.f, 0.19f, 0.05f, Col(kGoldHi, 1.f));
    p.Box(-0.22f, 0.165f, 0.22f, 0.1665f, Col(kGold, 0.6f));
    const float left = -0.31f, column = w - 0.04f - left;
    p.Text("STAND TALL, LOOK AHEAD", left, 0.09f, 0.034f, Col(kParchment, 0.9f), 0.f, column);
    p.Text("ARMS STRAIGHT OUT TO THE SIDES", left, 0.03f, 0.034f, Col(kParchment, 0.9f), 0.f, column);
    p.Text("HOLD BOTH TRIGGERS", left, -0.03f, 0.034f, Col(kParchment, 0.9f), 0.f, column);
    p.Text("PRESS B TO SKIP", 0.f, -0.26f, 0.026f, Col(kParchment, 0.5f));
    // A figure in a T-pose (its arms come with the state: gold while both are in place).
    const Px ink = Col(kParchment, 0.9f);
    cv.Ring(p.X(FX(0.f)), p.Y(FY(0.105f)), 0.028f * kFigureScale * p.k, kStroke * p.k, ink);
    p.Line(FX(0.f), FY(0.075f), FX(0.f), FY(-0.04f), kStroke, ink);
    p.Line(FX(0.f), FY(-0.04f), FX(-0.045f), FY(-0.15f), kStroke, ink);
    p.Line(FX(0.f), FY(-0.04f), FX(0.045f), FY(-0.15f), kStroke, ink);
}

// The whole panel for `s`, over `base` (CalibDrawBase of the same `done`).
inline void CalibDrawPanel(const Canvas& base, Canvas& cv, const CalibPanelState& s) {
    using calib_draw::FX; using calib_draw::FY; using calib_draw::kStroke;
    cv = base;
    const calib_draw::Pen p{cv};
    char line[64];
    if (s.phase == CalibPhase::done) {
        p.Text("CALIBRATED", 0.f, 0.17f, 0.055f, Col(kGoldHi, 1.f));
        std::snprintf(line, sizeof(line), "HEIGHT %.2f M", CalibHeight(s.eyeMm / 1000.f));
        p.Text(line, 0.f, 0.06f, 0.04f, Col(kParchment, 1.f));
        std::snprintf(line, sizeof(line), "ARM LENGTH %.2f M", s.armMm / 1000.f);
        p.Text(line, 0.f, -0.02f, 0.04f, Col(kParchment, 1.f));
        p.Text("THE TARNISHED'S ARMS NOW MATCH YOURS", 0.f, -0.11f, 0.032f, Col(kParchment, 0.75f));
        p.Text("REDO IT IN THE LAUNCHER: OPTIONS, BODY", 0.f, -0.22f, 0.027f, Col(kParchment, 0.5f));
        return;
    }
    // The figure's arms: faint until both of yours are in place, then gold.
    p.Line(FX(-0.115f), FY(0.055f), FX(0.115f), FY(0.055f), s.ready ? kStroke * 1.3f : kStroke,
           s.ready ? Col(kGoldHi, 1.f) : Col(kParchment, 0.3f));
    const bool holding = s.phase == CalibPhase::holding;
    p.Text(holding ? "HOLD STILL" : CalibHintText(s.hint), 0.f, -0.13f, 0.034f, holding ? Col(kGoldHi, 1.f) : Col(kCalibAmber, 1.f));
    const float x0 = -0.4f, x1 = 0.4f, y0 = -0.195f, y1 = -0.17f, rim = 0.004f;
    p.Box(x0 - rim, y0 - rim, x1 + rim, y1 + rim, Col(kGold, 0.6f));
    p.Box(x0, y0, x1, y1, Col(kDark, 1.f));
    if (s.progress > 0) p.Box(x0, y0, x0 + (x1 - x0) * std::min(s.progress / (float)kCalibSteps, 1.f), y1, Col(kGoldHi, 1.f));
}

// A controller's ring: gold while its arm is in place (`lit`), the hold filling it from the top, clockwise.
inline void CalibDrawRing(Canvas& cv, bool lit, int progress) {
    cv.Reset(kCalibRingPx);
    const float k = kCalibRingPx / kCalibRingM, c = kCalibRingPx * 0.5f;
    cv.Ring(c, c, 0.074f * k, 0.008f * k, lit ? Col(kGoldHi, 1.f) : Col(kParchment, 0.6f));
    if (progress <= 0) return;
    const float r0 = 0.078f * k, r1 = 0.09f * k, share = std::min(progress / (float)kCalibSteps, 1.f);
    const Px gold = Col(kGoldHi, 1.f);
    cv.Around(c, c, r1 + 1.f, [&](int x, int y, float d, float dx, float dy) {
        const float radial = std::min(d - r0, r1 - d) + 0.5f;
        if (radial <= 0.f) return;
        float a = std::atan2(dx, -dy);
        if (a < 0.f) a += 6.2831853f;
        if (a <= share * 6.2831853f) cv.Blend(x, y, gold, Clamp(radial, 0.f, 1.f));
    });
}

} // namespace ervr
