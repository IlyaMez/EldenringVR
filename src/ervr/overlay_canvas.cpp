#include "overlay_canvas.h"
#include "item_icons.h"
#include <unordered_map>
#include <utility>

namespace ervr {
namespace canvas_detail { float toLin[256]; uint8_t toSrgb[4096]; }

void CanvasInit() {
    // The worker draws, the render thread converts on upload: a function-local static runs this exactly once.
    static const bool s_done = [] {
        for (int i = 0; i < 256; ++i) {
            const float c = i / 255.f;
            canvas_detail::toLin[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        for (int i = 0; i < 4096; ++i) {
            const float l = i / 4095.f;
            const float s = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.f / 2.4f) - 0.055f;
            canvas_detail::toSrgb[i] = (uint8_t)std::lround(Clamp(s, 0.f, 1.f) * 255.f);
        }
        return true;
    }();
    (void)s_done;
}

namespace {
// Decoded icons by (icon id, maxSide): the wheel and the belt draw the same icon at different sizes.
std::unordered_map<long long, Icon> g_icons;   // worker only
}

const Icon* CanvasIcon(int iconId, int maxSide) {
    if (iconId < 0) return nullptr;
    const long long key = (long long)iconId << 16 | (maxSide & 0xFFFF);
    auto it = g_icons.find(key);
    if (it != g_icons.end()) return &it->second;
    std::vector<uint32_t> rgba; uint32_t w = 0, h = 0;
    // Not read yet: ask the loader (a no-op once asked) and draw a number until it lands.
    if (!ItemIconRgba(iconId, rgba, w, h) || w < 2 || h < 2) { ItemIconsRequest(iconId); return nullptr; }
    CanvasInit();
    Icon ic; ic.w = (int)w; ic.h = (int)h; ic.px.resize(rgba.size());
    for (size_t i = 0; i < rgba.size(); ++i) {
        const uint32_t v = rgba[i];
        const float a = (v >> 24) / 255.f;
        ic.px[i] = {SrgbToLinear(v & 255) * a, SrgbToLinear((v >> 8) & 255) * a, SrgbToLinear((v >> 16) & 255) * a, a};
    }
    while (ic.w >= 2 * maxSide && ic.h >= 2 * maxSide && !(ic.w & 1) && !(ic.h & 1)) {
        Icon half; half.w = ic.w / 2; half.h = ic.h / 2; half.px.resize((size_t)half.w * half.h);
        for (int y = 0; y < half.h; ++y)
            for (int x = 0; x < half.w; ++x) {
                const Px& a = ic.px[(size_t)(2 * y) * ic.w + 2 * x];
                const Px& b = ic.px[(size_t)(2 * y) * ic.w + 2 * x + 1];
                const Px& c = ic.px[(size_t)(2 * y + 1) * ic.w + 2 * x];
                const Px& d = ic.px[(size_t)(2 * y + 1) * ic.w + 2 * x + 1];
                half.px[(size_t)y * half.w + x] = {(a.r + b.r + c.r + d.r) * 0.25f, (a.g + b.g + c.g + d.g) * 0.25f,
                                                   (a.b + b.b + c.b + d.b) * 0.25f, (a.a + b.a + c.a + d.a) * 0.25f};
            }
        ic = std::move(half);
    }
    return &g_icons.emplace(key, std::move(ic)).first->second;
}

} // namespace ervr
