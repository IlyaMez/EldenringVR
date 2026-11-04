#pragma once
// Text on the CPU canvas (overlay_canvas.h), in a stroke font of capital letters, digits and some punctuation:
// lower case draws as capitals, anything else as a space. Spidy's overlay font - each glyph a few straight
// strokes in a cell 4 wide and 6 tall - drawn with the canvas's anti-aliased strokes, so ERVR needs no font file
// or atlas for the panels it composes itself (the body calibration's).
#include "overlay_canvas.h"
#include <cctype>
#include <string_view>

namespace ervr {
namespace text_detail {

// A glyph in half units of the cell: its width, and its strokes, each a run of points of two characters, x (0-8)
// then y (hex, 0-c, up from the baseline), the runs separated by spaces. Corners are cut at 45 degrees instead of
// rounded, so every stroke is a straight bar.
struct Glyph { char c; unsigned char width; const char* strokes; };
inline constexpr Glyph kGlyphs[] = {
    {'A', 8, "000a2c6c8a80 0585"},
    {'B', 8, "0666848260000c6c8a8866"},
    {'C', 8, "826020020a2c6c8a"},
    {'D', 8, "000c5c89835000"},
    {'E', 8, "80000c8c 0666"},
    {'F', 8, "000c8c 0666"},
    {'G', 8, "8a6c2c0a022060828545"},
    {'H', 8, "000c 808c 0686"},
    {'I', 4, "0040 202c 0c4c"},
    {'J', 8, "4c8c8260200204"},
    {'K', 8, "000c 8c04 3780"},
    {'L', 8, "0c0080"},
    {'M', 8, "000c468c80"},
    {'N', 8, "000c808c"},
    {'O', 8, "2060828a6c2c0a0220"},
    {'P', 8, "000c6c8a886606"},
    {'Q', 8, "2060828a6c2c0a0220 5380"},
    {'R', 8, "000c6c8a886606 4680"},
    {'S', 8, "02206082846626080a2c6c8a"},
    {'T', 8, "0c8c 4c40"},
    {'U', 8, "0c022060828c"},
    {'V', 8, "0c408c"},
    {'W', 8, "0c2047608c"},
    {'X', 8, "008c 0c80"},
    {'Y', 8, "0c468c 4640"},
    {'Z', 8, "0c8c0080"},
    // A zero is slashed, unlike the letter O.
    {'0', 8, "2060828a6c2c0a0220 0a82"},
    {'1', 6, "2a4c40 2060"},
    {'2', 8, "0a2c6c8a880080"},
    {'3', 8, "0a2c6c8a886636 668482602002"},
    {'4', 8, "606c0383"},
    {'5', 8, "8c0c07678582602002"},
    {'6', 8, "8a6c2c0a02206082856707"},
    {'7', 8, "0c8c30"},
    {'8', 8, "26080a2c6c8a88662604022060828466"},
    {'9', 8, "022060828a6c2c0a072585"},
    {'.', 2, "1011"},
    {',', 2, "1200"},
    {':', 2, "1112 1718"},
    {'-', 8, "1676"},
    {'\'', 2, "1c19"},
    {'%', 8, "008c 0c2c2a0a0c 6282806062"},
    {'/', 8, "008c"},
    {'(', 4, "3c1a1230"},
    {')', 4, "1c3a3210"},
    {'!', 2, "1c14 1110"},
    {'?', 8, "0a2c6c8a884543 4140"},
    {'+', 8, "424a 0686"},
    {'=', 8, "0484 0888"},
    {'<', 8, "8c0680"},
    {'>', 8, "0c8600"},
};
constexpr unsigned char kSpaceWidth = 4, kSpacing = 3;   // half units
inline const Glyph* Find(char c) {
    const char upper = (char)std::toupper((unsigned char)c);
    for (const Glyph& g : kGlyphs)
        if (g.c == upper) return &g;
    return nullptr;
}
inline float Hex(char c) { return (float)(c <= '9' ? c - '0' : c - 'a' + 10); }

} // namespace text_detail

// How wide `s` is at capital height `height` (whatever unit the height is in).
inline float TextWidth(std::string_view s, float height) {
    unsigned halves = 0;
    for (const char c : s) {
        const text_detail::Glyph* g = text_detail::Find(c);
        halves += (unsigned)((g ? g->width : text_detail::kSpaceWidth) + text_detail::kSpacing);
    }
    return s.empty() ? 0.f : (float)(halves - text_detail::kSpacing) * height / 12.f;
}
// The largest capital height up to `height` at which `s` is at most `width` wide.
inline float TextFit(std::string_view s, float height, float width) {
    const float wide = TextWidth(s, height);
    return wide > width && wide > 0.f ? height * width / wide : height;
}
// `s` standing on the baseline `baseline` (canvas px, y down), capital height `height` px, from `x` (align 0) or
// centred on it (align 0.5), its strokes `weight` of the height wide.
inline void CanvasText(Canvas& cv, std::string_view s, float x, float baseline, float height, const Px& color,
                       float align = 0.5f, float weight = 0.12f) {
    const float unit = height / 12.f, width = height * weight;
    if (!(unit > 0.f)) return;
    float cx = x - TextWidth(s, height) * align;
    for (const char c : s) {
        const text_detail::Glyph* g = text_detail::Find(c);
        if (g) {
            for (const char* run = g->strokes; *run;) {
                const char* end = run;
                while (*end && *end != ' ') ++end;
                for (const char* p = run; p + 3 < end; p += 2)
                    cv.Stroke(cx + text_detail::Hex(p[0]) * unit, baseline - text_detail::Hex(p[1]) * unit,
                              cx + text_detail::Hex(p[2]) * unit, baseline - text_detail::Hex(p[3]) * unit, width, color);
                run = *end ? end + 1 : end;
            }
        }
        cx += (float)((g ? g->width : text_detail::kSpaceWidth) + text_detail::kSpacing) * unit;
    }
}

} // namespace ervr
