#pragma once
// CPU drawing for the small quads ERVR composes itself (overlay_layers.h): the weapon wheel and the belt.
// Colour is composed in linear light, premultiplied, then encoded as 8-bit sRGB of the premultiplied value -
// what an _SRGB swapchain decodes back to exactly that, the reading the compositor blends with.
// Worker thread only: the icon cache is unguarded.
#include "vrmath.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ervr {

struct Px { float r = 0, g = 0, b = 0, a = 0; };

namespace canvas_detail { extern float toLin[256]; extern uint8_t toSrgb[4096]; }
void CanvasInit();   // the sRGB tables; a no-op after the first call
inline float SrgbToLinear(uint8_t c) { return canvas_detail::toLin[c]; }
inline uint8_t LinearToSrgb8(float l) { return canvas_detail::toSrgb[(int)(Clamp(l, 0.f, 1.f) * 4095.f + 0.5f)]; }
inline Px Col(uint32_t rgb, float a) {
    return {SrgbToLinear((rgb >> 16) & 255) * a, SrgbToLinear((rgb >> 8) & 255) * a, SrgbToLinear(rgb & 255) * a, a};
}
// ELDEN RING's menu palette, as the launcher wears it.
constexpr uint32_t kGold = 0xC9A65A, kGoldHi = 0xF2D892, kParchment = 0xE8DFC8, kDark = 0x0E0D0B, kDarkWarm = 0x2E2513;

struct Icon { int w = 0, h = 0; std::vector<Px> px; };
// The icon of an item (EquipParam iconId) from the game's archive (item_icons.h), linear premultiplied, halved
// while at least twice `maxSide` px. Null while the archive reader is still on it (asking it to, once) or when
// the archive has no such icon - callers draw a number then and look again later.
const Icon* CanvasIcon(int iconId, int maxSide);

struct Canvas {
    int w = 0, h = 0;
    std::vector<Px> px;
    void Reset(int size) { Reset(size, size); }
    void Reset(int width, int height) { w = width; h = height; px.assign((size_t)width * height, Px{}); }
    // A rectangle's cover, its edges anti-aliased (px, y down).
    void Rect(float x0, float y0, float x1, float y1, const Px& c) {
        for (int y = std::max(0, (int)std::floor(y0)); y <= std::min(h - 1, (int)std::ceil(y1)); ++y) {
            const float cy = Clamp(std::min((float)y + 1.f, y1) - std::max((float)y, y0), 0.f, 1.f);
            if (cy <= 0.f) continue;
            for (int x = std::max(0, (int)std::floor(x0)); x <= std::min(w - 1, (int)std::ceil(x1)); ++x)
                Blend(x, y, c, cy * Clamp(std::min((float)x + 1.f, x1) - std::max((float)x, x0), 0.f, 1.f));
        }
    }
    void Blend(int x, int y, const Px& c, float cov) {
        if (cov <= 0.f) return;
        if (cov > 1.f) cov = 1.f;
        Px& d = px[(size_t)y * w + x];
        const float k = 1.f - c.a * cov;
        d.r = c.r * cov + d.r * k; d.g = c.g * cov + d.g * k; d.b = c.b * cov + d.b * k; d.a = c.a * cov + d.a * k;
    }
    // Runs f(x, y, px-centre distance from (cx,cy)) over the square of radius `r` around a point.
    template <class F> void Around(float cx, float cy, float r, F f) {
        const int x0 = std::max(0, (int)std::floor(cx - r)), x1 = std::min(w - 1, (int)std::ceil(cx + r));
        const int y0 = std::max(0, (int)std::floor(cy - r)), y1 = std::min(h - 1, (int)std::ceil(cy + r));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                f(x, y, std::sqrt(dx * dx + dy * dy), dx, dy);
            }
    }
    void Disc(float cx, float cy, float r, const Px& c) {
        Around(cx, cy, r + 1.f, [&](int x, int y, float d, float, float) { Blend(x, y, c, r - d + 0.5f); });
    }
    // A disc whose opacity falls from `cIn` at the centre to `cOut` at the rim.
    void Vignette(float cx, float cy, float r, const Px& cIn, const Px& cOut) {
        Around(cx, cy, r + 1.f, [&](int x, int y, float d, float, float) {
            const float t = Clamp(d / r, 0.f, 1.f);
            const Px c{cIn.r + (cOut.r - cIn.r) * t, cIn.g + (cOut.g - cIn.g) * t, cIn.b + (cOut.b - cIn.b) * t, cIn.a + (cOut.a - cIn.a) * t};
            Blend(x, y, c, r - d + 0.5f);
        });
    }
    void Ring(float cx, float cy, float r, float width, const Px& c, int dashes = 0) {
        const float half = width * 0.5f;
        Around(cx, cy, r + half + 1.f, [&](int x, int y, float d, float dx, float dy) {
            float cov = half - std::fabs(d - r) + 0.5f;
            if (dashes && cov > 0.f) {
                const float t = (std::atan2(dx, -dy) + 3.14159265f) / 6.2831853f * (float)dashes;
                if (t - std::floor(t) > 0.5f) cov = 0.f;
            }
            Blend(x, y, c, cov);
        });
    }
    // A soft halo around a circle: falls off over `spread` px on both sides of radius r.
    void Glow(float cx, float cy, float r, float spread, const Px& c) {
        Around(cx, cy, r + 2.f * spread, [&](int x, int y, float d, float, float) {
            const float u = (d - r) / spread;
            Blend(x, y, c, std::exp(-u * u));
        });
    }
    // The part of an annulus within `half` radians of direction `ang` (clockwise from up).
    void Sector(float cx, float cy, float r0, float r1, float ang, float half, const Px& c) {
        // Only the wedge's own bounding box: its corners and the arc between them.
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
        for (int i = 0; i <= 8; ++i) {
            const float a = ang - half + 2.f * half * i / 8.f;
            for (float r : {r0, r1}) {
                const float x = cx + r * std::sin(a), y = cy - r * std::cos(a);
                minX = std::min(minX, x); maxX = std::max(maxX, x); minY = std::min(minY, y); maxY = std::max(maxY, y);
            }
        }
        for (int y = std::max(0, (int)minY - 2); y <= std::min(h - 1, (int)maxY + 2); ++y)
            for (int x = std::max(0, (int)minX - 2); x <= std::min(w - 1, (int)maxX + 2); ++x) {
                const float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = std::sqrt(dx * dx + dy * dy);
                const float radial = std::min(d - r0, r1 - d) + 0.5f;
                if (radial <= 0.f) continue;
                float da = std::fabs(std::atan2(dx, -dy) - ang);
                if (da > 3.14159265f) da = 6.2831853f - da;
                const float angular = (half - da) * d + 0.5f;   // ~1 px soft edge at that radius
                Blend(x, y, c, std::min(Clamp(radial, 0.f, 1.f), Clamp(angular, 0.f, 1.f)) * Clamp(1.f - (d - r0) / (r1 - r0), 0.f, 1.f));
            }
    }
    void Stroke(float x0, float y0, float x1, float y1, float width, const Px& c) {
        const float half = width * 0.5f;
        const float minX = std::min(x0, x1) - half - 1.f, maxX = std::max(x0, x1) + half + 1.f;
        const float minY = std::min(y0, y1) - half - 1.f, maxY = std::max(y0, y1) + half + 1.f;
        const float vx = x1 - x0, vy = y1 - y0, len2 = std::max(vx * vx + vy * vy, 1e-6f);
        for (int y = std::max(0, (int)minY); y <= std::min(h - 1, (int)maxY); ++y)
            for (int x = std::max(0, (int)minX); x <= std::min(w - 1, (int)maxX); ++x) {
                const float px0 = x + 0.5f - x0, py0 = y + 0.5f - y0;
                const float t = Clamp((px0 * vx + py0 * vy) / len2, 0.f, 1.f);
                const float ex = px0 - vx * t, ey = py0 - vy * t;
                Blend(x, y, c, half - std::sqrt(ex * ex + ey * ey) + 0.5f);
            }
    }
    // The icon fitted into a `size` px square centred on (cx, cy), bilinear, times `alpha`.
    void Image(const Icon& ic, float cx, float cy, float size, float alpha) {
        if (ic.w < 2 || ic.h < 2) return;
        const float x0 = cx - size * 0.5f, y0 = cy - size * 0.5f;
        const float sx = (float)ic.w / size, sy = (float)ic.h / size;
        for (int y = std::max(0, (int)y0); y <= std::min(h - 1, (int)(y0 + size)); ++y)
            for (int x = std::max(0, (int)x0); x <= std::min(w - 1, (int)(x0 + size)); ++x) {
                const float u = (x + 0.5f - x0) * sx - 0.5f, v = (y + 0.5f - y0) * sy - 0.5f;
                if (u < -0.5f || v < -0.5f || u > ic.w - 0.5f || v > ic.h - 0.5f) continue;
                const int iu = (int)std::floor(u), iv = (int)std::floor(v);
                const float fu = u - iu, fv = v - iv;
                auto at = [&](int a, int b) -> const Px& { return ic.px[(size_t)Clamp((float)b, 0.f, ic.h - 1.f) * ic.w + (size_t)Clamp((float)a, 0.f, ic.w - 1.f)]; };
                const Px &p00 = at(iu, iv), &p10 = at(iu + 1, iv), &p01 = at(iu, iv + 1), &p11 = at(iu + 1, iv + 1);
                auto mix = [&](float a, float b, float c, float d) { return (a * (1 - fu) + b * fu) * (1 - fv) + (c * (1 - fu) + d * fu) * fv; };
                const Px s{mix(p00.r, p10.r, p01.r, p11.r) * alpha, mix(p00.g, p10.g, p01.g, p11.g) * alpha,
                           mix(p00.b, p10.b, p01.b, p11.b) * alpha, mix(p00.a, p10.a, p01.a, p11.a) * alpha};
                Px& d = px[(size_t)y * w + x];
                const float k = 1.f - s.a;
                d.r = s.r + d.r * k; d.g = s.g + d.g * k; d.b = s.b + d.b * k; d.a = s.a + d.a * k;
            }
    }
    // Slot numbers for a weapon whose icon the game has not shown yet: strokes in a unit box.
    void Digit(int n, float cx, float cy, float size, const Px& c) {
        static const float k1[] = {0.38f, 0.24f, 0.55f, 0.10f, 0.55f, 0.10f, 0.55f, 0.90f};
        static const float k2[] = {0.25f, 0.28f, 0.36f, 0.14f, 0.36f, 0.14f, 0.62f, 0.12f, 0.62f, 0.12f, 0.75f, 0.25f,
                                   0.75f, 0.25f, 0.74f, 0.40f, 0.74f, 0.40f, 0.25f, 0.90f, 0.25f, 0.90f, 0.78f, 0.90f};
        static const float k3[] = {0.25f, 0.15f, 0.75f, 0.15f, 0.75f, 0.15f, 0.48f, 0.45f, 0.48f, 0.45f, 0.68f, 0.50f,
                                   0.68f, 0.50f, 0.77f, 0.66f, 0.77f, 0.66f, 0.68f, 0.84f, 0.68f, 0.84f, 0.45f, 0.90f,
                                   0.45f, 0.90f, 0.25f, 0.82f};
        const float* s = n == 1 ? k1 : n == 2 ? k2 : k3;
        const int segs = n == 1 ? 2 : n == 2 ? 6 : 7;
        const float ox = cx - size * 0.5f, oy = cy - size * 0.5f;
        for (int i = 0; i < segs; ++i)
            Stroke(ox + s[4 * i] * size, oy + s[4 * i + 1] * size, ox + s[4 * i + 2] * size, oy + s[4 * i + 3] * size, size * 0.11f, c);
    }
    // A non-negative number in seven-segment strokes, `height` px tall, its right edge at `right`.
    // Returns the left edge it reached.
    float Number(int value, float right, float cy, float height, const Px& c) {
        // Segments a..g (top, upper right, lower right, bottom, lower left, upper left, middle) in a 0.6 x 1 box.
        static const float seg[7][4] = {{0.f, 0.f, 0.6f, 0.f}, {0.6f, 0.f, 0.6f, 0.5f}, {0.6f, 0.5f, 0.6f, 1.f}, {0.f, 1.f, 0.6f, 1.f},
                                        {0.f, 0.5f, 0.f, 1.f}, {0.f, 0.f, 0.f, 0.5f}, {0.f, 0.5f, 0.6f, 0.5f}};
        static const uint8_t lit[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
        if (value < 0) value = 0;
        const float glyph = height * 0.6f, gap = height * 0.32f, width = height * 0.16f;
        float x = right - glyph;
        do {
            const int d = value % 10;
            for (int s = 0; s < 7; ++s)
                if (lit[d] & (1 << s))
                    Stroke(x + seg[s][0] * height, cy + (seg[s][1] - 0.5f) * height, x + seg[s][2] * height, cy + (seg[s][3] - 0.5f) * height, width, c);
            value /= 10;
            if (value) x -= glyph + gap;
        } while (value);
        return x - width * 0.5f;
    }
    void EncodeSrgb(std::vector<uint32_t>& out, float opacity) const {
        out.resize(px.size());
        for (size_t i = 0; i < px.size(); ++i) {
            const Px& p = px[i];
            const float a = Clamp(p.a * opacity, 0.f, 1.f);
            out[i] = (uint32_t)LinearToSrgb8(p.r * opacity) | ((uint32_t)LinearToSrgb8(p.g * opacity) << 8) |
                     ((uint32_t)LinearToSrgb8(p.b * opacity) << 16) | ((uint32_t)(a * 255.f + 0.5f) << 24);
        }
    }
};

} // namespace ervr
