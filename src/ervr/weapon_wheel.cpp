#include "weapon_wheel.h"
#include "weapon_wheel_logic.h"
#include "combat.h"
#include "config.h"
#include "equipment_layout.h"
#include "game_er.h"
#include "game_memory.h"
#include "item_icons.h"
#include "log.h"
#include "params_er.h"
#include "reshade_addon.h"
#include <windows.h>
#include <d3d12.h>
#include <xinput.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ervr {
namespace {

std::atomic<int> g_openHandShared{-1};   // worker -> combat: the hand whose wheel is up
constexpr int kWheelPx = 512;      // the wheel image, square
constexpr int kCursorPx = 64;
constexpr float kCursorM = 0.028f; // the cursor dot's quad, metres across
const char* HandName(int h) { return h == HandRight ? "right" : "left"; }
uint8_t* Deref(const void* a) { uint8_t* p = nullptr; return SafeRead(a, &p, sizeof(p)) ? p : nullptr; }

// ---------------------------------------------------------------- equipment (game thread -> worker)
struct Equip {
    bool valid = false;
    uint32_t seq = 0;
    int armStyle = 0;
    int active[2] = {0, 0};
    int paramId[2][kWheelSlots] = {};
    int iconId[2][kWheelSlots] = {};   // -1 = not resolved (params not loaded, no row)
    uint16_t wepType[2][kWheelSlots] = {};
    bool SameAs(const Equip& o) const {
        return valid == o.valid && armStyle == o.armStyle && active[0] == o.active[0] && active[1] == o.active[1] &&
               std::memcmp(paramId, o.paramId, sizeof(paramId)) == 0 && std::memcmp(iconId, o.iconId, sizeof(iconId)) == 0;
    }
};
std::mutex g_equipMtx;
Equip g_equip;

void ReadEquip(uint8_t* player) {
    Equip e;
    uint8_t* asmp = player ? Deref(player + Player_ChrAsm) : nullptr;
    uint32_t sl = 0, sr = 0; int ids[2 * kWheelSlots] = {};
    if (asmp && SafeRead(asmp + Asm_ArmStyle, &e.armStyle, 4) && SafeRead(asmp + Asm_LeftSlot, &sl, 4) &&
        SafeRead(asmp + Asm_RightSlot, &sr, 4) && SafeRead(asmp + Asm_ParamIds, ids, sizeof(ids)) && sl < 3 && sr < 3) {
        e.valid = true;
        e.active[HandLeft] = (int)sl; e.active[HandRight] = (int)sr;
        // param id -> (icon id, wepType); resolved once per weapon (a linear row scan), game thread only.
        static std::unordered_map<int, std::pair<int, uint16_t>> s_rows;
        for (int h = 0; h < 2; ++h)
            for (int k = 0; k < kWheelSlots; ++k) {
                const int id = ids[k * 2 + h];
                e.paramId[h][k] = id; e.iconId[h][k] = -1;
                if (!WheelSlotFilled(id)) continue;
                auto it = s_rows.find(id);
                if (it == s_rows.end() && ParamsReady()) {
                    // Every affinity and upgrade level of a weapon shares its base row's icon.
                    uint8_t* row = ParamsRow(PI_EquipParamWeapon, (uint32_t)(id / 100) * 100u);
                    uint16_t icon = 0, type = 0;
                    if (row) { SafeRead(row + Wep_IconId, &icon, 2); SafeRead(row + Wep_Type, &type, 2); }
                    it = s_rows.emplace(id, std::make_pair(row ? (int)icon : -1, type)).first;
                }
                if (it != s_rows.end()) { e.iconId[h][k] = it->second.first; e.wepType[h][k] = it->second.second; }
            }
    }
    std::lock_guard<std::mutex> g(g_equipMtx);
    if (!e.SameAs(g_equip)) { e.seq = g_equip.seq + 1; g_equip = e; }
}
// SafeRead never faults; this is for the unexpected, and nothing in ReadEquip holds a lock while it
// reads game memory.
void ReadEquipGuarded(uint8_t* player) {
    __try { ReadEquip(player); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------- the image (worker)
// Composed in linear light, premultiplied, then encoded as 8-bit sRGB of the premultiplied value -
// what an _SRGB swapchain decodes back to exactly that, the reading the compositor blends with.
struct Px { float r = 0, g = 0, b = 0, a = 0; };
float g_toLin[256];
uint8_t g_toSrgb[4096];
void InitLuts() {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    for (int i = 0; i < 256; ++i) {
        const float c = i / 255.f;
        g_toLin[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    for (int i = 0; i < 4096; ++i) {
        const float l = i / 4095.f;
        const float s = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.f / 2.4f) - 0.055f;
        g_toSrgb[i] = (uint8_t)std::lround(Clamp(s, 0.f, 1.f) * 255.f);
    }
}
uint8_t Enc(float l) { return g_toSrgb[(int)(Clamp(l, 0.f, 1.f) * 4095.f + 0.5f)]; }
Px Col(uint32_t rgb, float a) {
    return {g_toLin[(rgb >> 16) & 255] * a, g_toLin[(rgb >> 8) & 255] * a, g_toLin[rgb & 255] * a, a};
}
// ELDEN RING's menu palette, as the launcher wears it.
constexpr uint32_t kGold = 0xC9A65A, kGoldHi = 0xF2D892, kParchment = 0xE8DFC8, kDark = 0x0E0D0B, kDarkWarm = 0x2E2513;

struct Icon { int w = 0, h = 0; std::vector<Px> px; };

struct Canvas {
    int w = 0, h = 0;
    std::vector<Px> px;
    void Reset(int size) { w = h = size; px.assign((size_t)size * size, Px{}); }
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
    void EncodeSrgb(std::vector<uint32_t>& out, float opacity) const {
        out.resize(px.size());
        for (size_t i = 0; i < px.size(); ++i) {
            const Px& p = px[i];
            const float a = Clamp(p.a * opacity, 0.f, 1.f);
            out[i] = (uint32_t)Enc(p.r * opacity) | ((uint32_t)Enc(p.g * opacity) << 8) | ((uint32_t)Enc(p.b * opacity) << 16) |
                     ((uint32_t)(a * 255.f + 0.5f) << 24);
        }
    }
};

// Decoded icons, linear premultiplied, halved until at most twice the size they are drawn at.
std::unordered_map<int, Icon> g_icons;        // worker only
const Icon* IconFor(int iconId, double) {
    if (iconId < 0) return nullptr;
    auto it = g_icons.find(iconId);
    if (it != g_icons.end()) return &it->second;
    std::vector<uint32_t> rgba; uint32_t w = 0, h = 0;
    // Not read yet: ask the loader (a no-op once asked) and show the slot number until it lands.
    if (!ItemIconRgba(iconId, rgba, w, h) || w < 2 || h < 2) { ItemIconsRequest(iconId); return nullptr; }
    Icon ic; ic.w = (int)w; ic.h = (int)h; ic.px.resize(rgba.size());
    for (size_t i = 0; i < rgba.size(); ++i) {
        const uint32_t v = rgba[i];
        const float a = (v >> 24) / 255.f;
        ic.px[i] = {g_toLin[v & 255] * a, g_toLin[(v >> 8) & 255] * a, g_toLin[(v >> 16) & 255] * a, a};
    }
    while (ic.w >= 256 && ic.h >= 256 && !(ic.w & 1) && !(ic.h & 1)) {
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
    return &g_icons.emplace(iconId, std::move(ic)).first->second;
}

// ---------------------------------------------------------------- the gesture (worker)
enum class St { Idle, Pass, Pending, Open, Swallow };
struct HandState {
    St st = St::Idle;
    double downAt = 0, stillSince = 0;
    // Why this squeeze has not opened the wheel - said in the log when a long one ends without it.
    const char* why = nullptr;
    bool expected = false;    // a block the player meant (a grab, two-handing, a menu): no log line
    float maxSpeed = 0.f;
};
HandState g_hand[2];

struct OpenWheel {
    int hand = -1;                       // -1 = closed
    WheelFrame frame;
    double openedAt = 0;
    int hover = -1, active = 0;
    bool filled[kWheelSlots] = {};
    int paramId[kWheelSlots] = {}, iconId[kWheelSlots] = {};
    bool iconMissing = false;            // a filled slot had no icon when last drawn
    double nextIconLook = 0;
    float cx = 0.f, cy = 0.f;            // the hand over the wheel, metres
} g_open;
WheelCommit g_commit[2];
double g_commitStart[2] = {0.0, 0.0};
int g_commitFrom[2] = {0, 0};
// Jump: a target more than one press away gets the active slot moved to the one before it first, so
// the game's own switch runs once. Worker asks, the game thread writes on its own frame.
struct Jump { bool tried = false; int slot = -1; double at = 0.0; };
Jump g_jump[2];
std::atomic<int> g_jumpReq[2] = {-1, -1};     // worker -> game thread: put the active slot here
std::atomic<int> g_jumpResult[2] = {0, 0};    // game thread -> worker: 1 written, -1 refused

// PlayerGameData's own ChrAsm, the save-side copy the character's is applied from (the cheat table's
// CurrentWepSlotOffset: left +0x328, right +0x32C = ChrAsm +0xC / +0x10).
constexpr size_t PGD_ChrAsm = 0x31C;
// Game thread. Moves the hand's active slot in both ChrAsm copies - only after checking that the save
// copy mirrors the character's exactly (arm style, active slots, the six weapons), so a layout that moved
// in a patch can never be written into. False = refused (logged once), the switch cycles instead.
bool WriteActiveSlot(uint8_t* player, int hand, int slot) {
    uint8_t* chrAsm = player ? Deref(player + Player_ChrAsm) : nullptr;
    uint8_t* pgd = GameERPlayerGameData();
    if (!chrAsm || !pgd || slot < 0 || slot >= kWheelSlots) return false;
    uint8_t* saveAsm = pgd + PGD_ChrAsm;
    uint8_t a[0x1C], b[0x1C], wa[24], wb[24];
    const bool read = SafeRead(chrAsm + Asm_ArmStyle, a, sizeof(a)) && SafeRead(saveAsm + Asm_ArmStyle, b, sizeof(b)) &&
                      SafeRead(chrAsm + Asm_ParamIds, wa, sizeof(wa)) && SafeRead(saveAsm + Asm_ParamIds, wb, sizeof(wb));
    if (!read || std::memcmp(a, b, sizeof(a)) != 0 || std::memcmp(wa, wb, sizeof(wa)) != 0) {
        static bool s_warned = false;
        if (!s_warned) { s_warned = true; LOGW("[wheel] PlayerGameData+0x%zX does not mirror the character's ChrAsm (moved in a patch?): no jumps, the switch cycles through the slots", PGD_ChrAsm); }
        return false;
    }
    const int32_t v = slot;
    const size_t at = hand == HandRight ? Asm_RightSlot : Asm_LeftSlot;
    return SafeWrite(saveAsm + at, &v, 4) && SafeWrite(chrAsm + at, &v, 4);
}
std::atomic<long> g_opens{0}, g_switches{0}, g_failed{0};

// What the render thread shows: the latest image and where the layers hang.
struct Published {
    bool visible = false;
    ULONGLONG tick = 0;                  // when the worker last refreshed it (a stalled worker hides the wheel)
    Pose wheelPose; float wheelSize = 0.3f;
    bool cursor = false; Pose cursorPose;
    uint32_t version = 0;                // of the wheel image
};
std::mutex g_pubMtx;
Published g_pub;
std::vector<uint32_t> g_pubImg;          // kWheelPx^2, RGBA8: sRGB of the premultiplied linear colour
std::vector<uint32_t> g_cursorImg;       // kCursorPx^2, same encoding; drawn once
Canvas g_canvas;                         // worker only

void Render(double now) {
    const Config& c = Cfg();
    const float S = (float)kWheelPx, C = S * 0.5f;
    const float size = std::max(c.wheelSize, 0.1f);
    const float rBg = 0.47f * S, rItems = 0.30f * S, rSlot = 0.135f * S;
    const float rDead = Clamp(c.wheelDeadZone / size * S, 8.f, rItems - rSlot);
    const bool right = g_open.hand == HandRight;
    // The disc and its rings never change while the settings do not: drawn once, copied per render,
    // so a hover change only costs the slots (the worker also polls the controllers).
    static Canvas s_base;
    static float s_baseDead = -1.f;
    if (s_base.px.empty() || s_baseDead != rDead) {
        s_baseDead = rDead;
        s_base.Reset(kWheelPx);
        s_base.Vignette(C, C, rBg, Col(kDark, 0.66f), Col(kDark, 0.46f));
        s_base.Ring(C, C, rBg - 1.5f, 2.5f, Col(kGold, 0.85f));
        s_base.Ring(C, C, rBg - 7.f, 1.f, Col(kGold, 0.30f));
        s_base.Ring(C, C, rDead, 1.5f, Col(kGold, 0.35f));
    }
    Canvas& cv = g_canvas;
    cv = s_base;
    if (g_open.hover >= 0)
        cv.Sector(C, C, rDead + 4.f, rItems + rSlot, WheelSlotAngle(g_open.hover, right), 0.42f, Col(kGold, 0.22f));
    g_open.iconMissing = false;
    for (int k = 0; k < kWheelSlots; ++k) {
        const float a = WheelSlotAngle(k, right);
        const float x = C + rItems * std::sin(a), y = C - rItems * std::cos(a);
        const bool hover = k == g_open.hover, active = k == g_open.active;
        if (!g_open.filled[k]) {
            cv.Disc(x, y, rSlot * 0.82f, Col(kDark, 0.35f));
            cv.Ring(x, y, rSlot * 0.82f, 1.5f, Col(kGold, 0.35f), 16);
            continue;
        }
        if (hover) cv.Glow(x, y, rSlot + 2.f, 9.f, Col(kGoldHi, 0.55f));
        cv.Disc(x, y, rSlot, hover ? Col(kDarkWarm, 0.92f) : Col(kDark, 0.80f));
        cv.Ring(x, y, rSlot, hover ? 4.f : active ? 2.5f : 1.8f, hover ? Col(kGoldHi, 1.f) : Col(kGold, active ? 0.95f : 0.70f));
        const float iconSize = (hover ? 1.78f : 1.62f) * rSlot;
        if (const Icon* ic = IconFor(g_open.iconId[k], now)) cv.Image(*ic, x, y, iconSize, hover || active ? 1.f : 0.85f);
        else { cv.Digit(k + 1, x, y, rSlot * 0.95f, Col(kParchment, 0.9f)); g_open.iconMissing = true; }
        if (active) {   // the weapon in hand: a gold pip just outside its slot, toward the rim
            const float r = rItems + rSlot + 11.f;
            cv.Disc(C + r * std::sin(a), C - r * std::cos(a), 7.5f, Col(kDark, 0.8f));
            cv.Disc(C + r * std::sin(a), C - r * std::cos(a), 5.5f, Col(kGoldHi, 1.f));
        }
    }
    std::vector<uint32_t> img;
    cv.EncodeSrgb(img, Clamp(c.wheelOpacity, 0.f, 1.f));
    std::lock_guard<std::mutex> g(g_pubMtx);
    g_pubImg.swap(img);
    ++g_pub.version;
}
void RenderCursor() {
    Canvas cv; cv.Reset(kCursorPx);
    const float C = kCursorPx * 0.5f;
    cv.Glow(C, C, 10.f, 6.f, Col(kGoldHi, 0.35f));
    cv.Disc(C, C, 9.f, Col(kParchment, 0.95f));
    cv.Ring(C, C, 16.f, 2.2f, Col(kGold, 0.9f));
    std::vector<uint32_t> img;
    cv.EncodeSrgb(img, 1.f);
    std::lock_guard<std::mutex> g(g_pubMtx);
    g_cursorImg.swap(img);
}

void Publish(double now, const Config& c) {
    g_openHandShared.store(g_open.hand, std::memory_order_relaxed);
    std::lock_guard<std::mutex> g(g_pubMtx);
    g_pub.tick = GetTickCount64();
    g_pub.visible = g_open.hand >= 0;
    if (!g_pub.visible) { g_pub.cursor = false; return; }
    const WheelFrame& f = g_open.frame;
    // Opens from 70% to full size over 120 ms: enough to read as "it appeared here", too short to aim at.
    const float t = Clamp((float)((now - g_open.openedAt) / 0.12), 0.f, 1.f);
    const float grow = 0.7f + 0.3f * t * t * (3.f - 2.f * t);
    g_pub.wheelPose.pos = f.center;
    g_pub.wheelPose.rot = WheelOrientation(f);
    g_pub.wheelPose.valid = true;
    g_pub.wheelSize = std::max(c.wheelSize, 0.1f) * grow;
    // The cursor rides the plane a few mm toward the eyes, clamped to the wheel's rim.
    const float rim = 0.47f * std::max(c.wheelSize, 0.1f);
    float x = g_open.cx, y = g_open.cy;
    const float r = std::sqrt(x * x + y * y);
    if (r > rim) { x *= rim / r; y *= rim / r; }
    g_pub.cursor = true;
    g_pub.cursorPose.pos = f.center + f.right * x + f.up * y + f.normal * 0.004f;
    g_pub.cursorPose.rot = g_pub.wheelPose.rot;
    g_pub.cursorPose.valid = true;
}

void Open(int h, const VRTracking& tr, const Equip& eq, double now) {
    g_open = OpenWheel{};
    g_open.hand = h;
    g_open.frame = WheelFrameFacing(tr.hand[h].pos, tr.head.pos, tr.head.rot.Up());
    g_open.openedAt = now;
    g_open.active = eq.active[h];
    char slots[384]; int n = 0;
    for (int k = 0; k < kWheelSlots; ++k) {
        g_open.filled[k] = WheelSlotFilled(eq.paramId[h][k]);
        g_open.paramId[k] = eq.paramId[h][k];
        g_open.iconId[k] = g_open.filled[k] ? eq.iconId[h][k] : -1;
        const bool icon = IconFor(g_open.iconId[k], now) != nullptr;
        if (n < 0 || n >= (int)sizeof(slots)) break;
        if (!g_open.filled[k]) n += snprintf(slots + n, sizeof(slots) - n, "%s slot %d empty", k ? "," : "", k + 1);
        else n += snprintf(slots + n, sizeof(slots) - n, "%s slot %d weapon %d icon %d%s%s", k ? "," : "", k + 1,
                           g_open.paramId[k], g_open.iconId[k], icon ? "" : " (icon not read yet)",
                           k == g_open.active ? " [in hand]" : "");
    }
    g_opens.fetch_add(1, std::memory_order_relaxed);
    LOGI("[wheel] %s hand: open -%s", HandName(h), slots);
    Render(now);
}

void Close(const char* why) {
    if (g_open.hand < 0) return;
    if (why) LOGI("[wheel] %s hand: closed without switching (%s)", HandName(g_open.hand), why);
    g_open.hand = -1;
}

bool HandAllowed(const std::string& hands, int h) {
    if (hands == "off" || hands == "none") return false;
    if (hands == "right") return h == HandRight;
    if (hands == "left") return h == HandLeft;
    return true;
}

} // namespace

WheelPadOut WeaponWheelUpdate(const VRTracking& tr, const VRInputState& in, double now, bool menu) {
    const Config& c = Cfg();
    WheelPadOut out;
    InitLuts();
    if (c.wheelEnabled) {
        static bool s_started = false;
        if (!s_started) {
            s_started = true;
            ParamsInit();   // combat normally did this already; the wheel must not depend on it
            RenderCursor();
            LOGI("[wheel] hold a grip still for %.2f s to open that hand's weapon wheel (hands: %s, switch: %s); a shorter squeeze sends the grip's own button",
                 c.wheelHoldSeconds, c.wheelHands.c_str(), c.wheelSwitch.c_str());
        }
    }
    Equip eq; { std::lock_guard<std::mutex> g(g_equipMtx); eq = g_equip; }
    // A new loadout: decode its icons now, not in the tick the wheel opens, and say what the wheel sees.
    static uint32_t s_warmSeq = 0;
    if (c.wheelEnabled && eq.valid && eq.seq != s_warmSeq) {
        s_warmSeq = eq.seq;
        for (int h = 0; h < 2; ++h)
            for (int k = 0; k < kWheelSlots; ++k)
                if (WheelSlotFilled(eq.paramId[h][k])) IconFor(eq.iconId[h][k], now);
        static int s_logged[2][kWheelSlots] = {};
        if (std::memcmp(s_logged, eq.paramId, sizeof(s_logged)) != 0) {
            std::memcpy(s_logged, eq.paramId, sizeof(s_logged));
            auto slot = [&](int h, int k) { return WheelSlotFilled(eq.paramId[h][k]) ? eq.paramId[h][k] : 0; };
            LOGI("[wheel] equipment: right %d / %d / %d, left %d / %d / %d (0 = empty)", slot(HandRight, 0), slot(HandRight, 1),
                 slot(HandRight, 2), slot(HandLeft, 0), slot(HandLeft, 1), slot(HandLeft, 2));
        }
    }
    const bool playerActive = GameERPlayerActive(), transition = GameERTransitionActive();
    const bool twoHanded = eq.valid && (eq.armStyle == 2 || eq.armStyle == 3);
    for (int h = 0; h < 2; ++h) {
        HandState& s = g_hand[h];
        const float grip = in.handTrigger[h];
        const bool down = grip >= c.gripThreshold, up = grip < 0.5f * c.gripThreshold;
        bool holding = false;
        const bool nearHilt = CombatTwoHandGrip(h, &holding);
        const bool tracked = tr.hand[h].valid && tr.head.valid && in.controllerConnected[h];
        const float speed = tr.hand[h].linVel.Length();
        WheelGate gate;
        gate.enabled = c.wheelEnabled;
        gate.handAllowed = HandAllowed(c.wheelHands, h);
        gate.playerActive = playerActive; gate.transition = transition; gate.menu = menu;
        gate.twoHanded = twoHanded;
        gate.grabbing = holding;
        bool filled[kWheelSlots];
        for (int k = 0; k < kWheelSlots; ++k) filled[k] = eq.valid && WheelSlotFilled(eq.paramId[h][k]);
        gate.weapons = eq.valid && WheelHasWeapons(filled);
        const char* blocked = WheelBlocked(gate);
        // A squeeze that ends without the wheel after the hold time says why, unless the block was meant.
        auto ended = [&] {
            if (c.wheelEnabled && !s.expected && now - s.downAt >= c.wheelHoldSeconds)
                LOGI("[wheel] %s grip held %.1f s without opening the wheel: %s", HandName(h), now - s.downAt,
                     s.why ? s.why : s.maxSpeed > c.wheelStillSpeed ? "the hand kept moving faster than StillSpeed - hold it still" : "let go before HoldSeconds");
            s.st = St::Idle;
        };
        switch (s.st) {
        case St::Idle:
            if (!down) break;
            s.downAt = s.stillSince = now; s.maxSpeed = 0.f; s.why = nullptr; s.expected = false;
            // Closed at the other weapon's hilt it is a two-hand grab: combat's, not ours.
            if (blocked || nearHilt) {
                s.st = St::Pass;
                s.why = blocked ? blocked : "it closed at the other weapon's hilt (a two-hand grab)";
                s.expected = nearHilt || WheelBlockExpected(blocked, gate);
            } else s.st = St::Pending;
            break;
        case St::Pass:
        case St::Swallow:
            if (up) ended();
            break;
        case St::Pending:
            if (up) {
                // Let go before the wheel could have opened: the grip's own button, as one tap.
                if (now - s.downAt <= std::max(c.wheelHoldSeconds, 0.1f)) { out.tapGrip[h] = true; s.st = St::Idle; }
                else ended();
            } else if (blocked) {
                s.st = St::Swallow;   // it became a two-hand grab, a menu, ...: no wheel and no tap
                s.why = blocked; s.expected = WheelBlockExpected(blocked, gate);
            } else if (nearHilt || !tracked) {
                s.stillSince = now;   // passing the other hilt, or a tracking gap: the hold waits
                s.why = !tracked ? "the controller lost tracking" : "the hand was near the other weapon's hilt";
            } else {
                if (speed > s.maxSpeed) s.maxSpeed = speed;
                if (WheelHoldReady(s.stillSince, now, speed, c.wheelStillSpeed, c.wheelHoldSeconds)) {
                    if (g_open.hand >= 0) s.why = "the other hand's wheel was open";
                    else {
                        Open(h, tr, eq, now);
                        s.st = St::Open;
                        out.hapticAmp[h] = 0.35f; out.hapticSec[h] = 0.03f;
                    }
                }
            }
            break;
        case St::Open: {
            if (blocked) {
                Close(blocked); s.st = St::Swallow;
                s.why = blocked; s.expected = true;   // already said by Close
                break;
            }
            // A tracking gap freezes the pick where it was; letting go still counts.
            const int pick = tracked ? (WheelProject(g_open.frame, tr.hand[h].pos, g_open.cx, g_open.cy),
                                        WheelPick(g_open.cx, g_open.cy, g_open.filled, h == HandRight, c.wheelDeadZone, g_open.hover))
                                     : g_open.hover;
            if (pick != g_open.hover) {
                g_open.hover = pick;
                if (pick >= 0) { out.hapticAmp[h] = 0.2f; out.hapticSec[h] = 0.015f; }
                Render(now);
            } else if (g_open.iconMissing && now >= g_open.nextIconLook) {
                g_open.nextIconLook = now + 0.2;   // an icon the loader finishes while the wheel is up
                Render(now);
            }
            if (!up) break;
            const int target = g_open.hover;
            if (target >= 0 && target != eq.active[h]) {
                g_commit[h].Start(target);
                g_commitStart[h] = now; g_commitFrom[h] = eq.active[h];
                g_jump[h] = Jump{};
                LOGI("[wheel] %s hand: picked slot %d (weapon %d) - pressing %s until the game's slot %d becomes %d",
                     HandName(h), target + 1, g_open.paramId[target], h == HandRight ? "DPAD_RIGHT" : "DPAD_LEFT",
                     eq.active[h] + 1, target + 1);
                out.hapticAmp[h] = 0.5f; out.hapticSec[h] = 0.05f;
                g_open.hand = -1;
            } else Close(target >= 0 ? "picked the weapon already in hand" : "let go in the middle");
            s.st = St::Idle;
            break;
        }
        }
        out.holdGrip[h] = s.st == St::Pending || s.st == St::Open || s.st == St::Swallow;
    }
    // The switch itself: the game's own button, until its active slot answers.
    WheelCommitTiming timing;
    timing.press = std::max((double)c.gesturePressDuration, 0.10);
    timing.timeout = Clamp(c.wheelSwitchTimeout, 0.3f, 5.f);
    for (int h = 0; h < 2; ++h) {
        WheelCommit& cm = g_commit[h];
        if (cm.target < 0) continue;
        if (!eq.valid || menu || !playerActive || transition) {
            LOGI("[wheel] %s hand: switch to slot %d abandoned (%s)", HandName(h), cm.target + 1, menu ? "a menu opened" : "left the world");
            cm.target = -1;
            continue;
        }
        // Jump: before the first press of a switch more than one press long, the active slot goes to the
        // slot just before the target, so the one press that follows is the game's only switch.
        Jump& jp = g_jump[h];
        if (cm.phase == 0 && cm.presses == 0 && !jp.tried) {
            jp.tried = true;
            bool filledH[kWheelSlots];
            for (int k = 0; k < kWheelSlots; ++k) filledH[k] = WheelSlotFilled(eq.paramId[h][k]);
            if (c.wheelSwitch != "cycle" && WheelCycleSteps(eq.active[h], cm.target, filledH) >= 2) {
                jp.slot = WheelSlotBefore(cm.target, filledH);
                jp.at = now;
                g_jumpResult[h].store(0);
                g_jumpReq[h].store(jp.slot);
                continue;
            }
        }
        if (jp.slot >= 0) {   // the game thread writes it on its next frame; the snapshot shows it the frame after
            const int r = g_jumpResult[h].load();
            if (r == 0 && eq.active[h] != jp.slot && now - jp.at < 0.3) continue;
            if (eq.active[h] == jp.slot)
                LOGI("[wheel] %s hand: jumped to slot %d, one press to slot %d", HandName(h), jp.slot + 1, cm.target + 1);
            else
                LOGI("[wheel] %s hand: jump to slot %d did not take (%s) - cycling instead", HandName(h), jp.slot + 1,
                     r < 0 ? "refused" : "the slot did not change");
            jp.slot = -1;
        }
        const int presses = cm.presses;
        switch (WheelCommitStep(cm, eq.active[h], now, timing)) {
        case WheelStep::Press:
            out.buttons |= h == HandRight ? XINPUT_GAMEPAD_DPAD_RIGHT : XINPUT_GAMEPAD_DPAD_LEFT;
            break;
        case WheelStep::Done:
            g_switches.fetch_add(1, std::memory_order_relaxed);
            LOGI("[wheel] %s hand: slot %d -> %d after %d press%s (%.2f s)", HandName(h), g_commitFrom[h] + 1, eq.active[h] + 1,
                 presses, presses == 1 ? "" : "es", now - g_commitStart[h]);
            break;
        case WheelStep::Failed:
            g_failed.fetch_add(1, std::memory_order_relaxed);
            LOGW("[wheel] %s hand: the game's slot stayed %d after %d press%s - it ignores weapon switches mid-action "
                 "(attacking, rolling, staggered); pick again once it is free",
                 HandName(h), eq.active[h] + 1, presses, presses == 1 ? "" : "es");
            out.hapticAmp[h] = 0.9f; out.hapticSec[h] = 0.2f;
            break;
        default: break;
        }
    }
    if (!c.wheelHaptics) for (int h = 0; h < 2; ++h) out.hapticAmp[h] = 0.f;
    Publish(now, c);
    return out;
}

void WeaponWheelGameFrame(uint8_t* player) {
    if (!Cfg().wheelEnabled) return;
    for (int h = 0; h < 2; ++h) {
        const int slot = g_jumpReq[h].exchange(-1);
        if (slot >= 0) g_jumpResult[h].store(WriteActiveSlot(player, h, slot) ? 1 : -1);
    }
    ReadEquipGuarded(player);
}

bool WeaponWheelOwnsHand(int hand) { return hand >= 0 && g_openHandShared.load(std::memory_order_relaxed) == hand; }

WheelStats WeaponWheelGetStats() {
    WheelStats s;
    { std::lock_guard<std::mutex> g(g_pubMtx); s.openHand = g_pub.visible ? g_open.hand : -1; }
    s.opens = g_opens.exchange(0); s.switches = g_switches.exchange(0); s.failed = g_failed.exchange(0);
    return s;
}

// ---------------------------------------------------------------- the layers (render thread)
namespace {

struct Gpu {
    ID3D12Device* dev = nullptr;
    ID3D12CommandAllocator* alloc[3] = {};
    uint64_t allocFence[3] = {};
    int ring = 0;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;
    uint64_t fenceVal = 0;
    // Two upload buffers per overlay: the copy of the previous frame may still be reading one while
    // new content goes into the other. Every frame copies the current one into a fresh swapchain image.
    struct Up { ID3D12Resource* buf = nullptr; uint8_t* map = nullptr; uint64_t fence = 0; } up[OverlayCount][2];
    int cur[OverlayCount] = {-1, -1};
    uint32_t version[OverlayCount] = {};     // content version staged in up[i][cur[i]]
    DXGI_FORMAT staged[OverlayCount] = {};   // swapchain format it was converted for
} g_gpu;

constexpr int kSidePx[OverlayCount] = {kWheelPx, kCursorPx};
UINT RowPitch(int side) { return (UINT)((side * 8 + 255) & ~255); }   // room for 8 bytes/pixel, 256-aligned

void WaitFence(uint64_t v) {
    if (!v || !g_gpu.fence || g_gpu.fence->GetCompletedValue() >= v) return;
    if (SUCCEEDED(g_gpu.fence->SetEventOnCompletion(v, g_gpu.ev))) WaitForSingleObject(g_gpu.ev, 500);
}

void ReleaseGpu() {
    if (g_gpu.fence) WaitFence(g_gpu.fenceVal);
    for (auto& a : g_gpu.alloc) if (a) { a->Release(); a = nullptr; }
    if (g_gpu.list) { g_gpu.list->Release(); g_gpu.list = nullptr; }
    if (g_gpu.fence) { g_gpu.fence->Release(); g_gpu.fence = nullptr; }
    if (g_gpu.ev) { CloseHandle(g_gpu.ev); g_gpu.ev = nullptr; }
    for (auto& per : g_gpu.up)
        for (auto& u : per) if (u.buf) { u.buf->Release(); u = {}; }
    g_gpu = Gpu{};
}

bool EnsureGpu(ID3D12Device* dev) {
    if (g_gpu.dev == dev && g_gpu.list) return true;
    ReleaseGpu();
    if (!dev) return false;
    g_gpu.dev = dev;
    bool ok = true;
    for (auto& a : g_gpu.alloc) ok = ok && SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)));
    ok = ok && SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_gpu.alloc[0], nullptr, IID_PPV_ARGS(&g_gpu.list)));
    if (ok) g_gpu.list->Close();
    ok = ok && SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_gpu.fence)));
    g_gpu.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ok = ok && g_gpu.ev;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    for (int i = 0; ok && i < OverlayCount; ++i)
        for (auto& u : g_gpu.up[i]) {
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = (UINT64)RowPitch(kSidePx[i]) * kSidePx[i];
            rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ok = SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                        nullptr, IID_PPV_ARGS(&u.buf))) &&
                 SUCCEEDED(u.buf->Map(0, nullptr, (void**)&u.map));
            if (!ok) break;
        }
    if (!ok) { LOGE("[wheel] could not create the upload resources; the wheel is not shown"); ReleaseGpu(); return false; }
    return true;
}

uint16_t Half(float f) {
    uint32_t x; std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int e = (int)((x >> 23) & 0xFF) - 127 + 15;
    if (e <= 0) return (uint16_t)sign;
    if (e >= 31) return (uint16_t)(sign | 0x7C00u);
    return (uint16_t)(sign | ((uint32_t)e << 10) | ((x >> 13) & 0x3FFu));
}

// The CPU image (8-bit sRGB of premultiplied linear) into the upload buffer, in the swapchain's format.
void Stage(int i, const std::vector<uint32_t>& img, DXGI_FORMAT fmt) {
    const int next = g_gpu.cur[i] < 0 ? 0 : (g_gpu.cur[i] + 1) & 1;
    Gpu::Up& u = g_gpu.up[i][next];
    WaitFence(u.fence);
    const int side = kSidePx[i];
    const UINT pitch = RowPitch(side);
    for (int y = 0; y < side; ++y) {
        const uint32_t* src = img.data() + (size_t)y * side;
        uint8_t* dst = u.map + (size_t)y * pitch;
        switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            std::memcpy(dst, src, (size_t)side * 4);
            break;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            for (int x = 0; x < side; ++x) {
                const uint32_t v = src[x];
                ((uint32_t*)dst)[x] = (v & 0xFF00FF00u) | ((v >> 16) & 0xFFu) | ((v & 0xFFu) << 16);
            }
            break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:   // linear light, read as such
            for (int x = 0; x < side; ++x) {
                const uint32_t v = src[x];
                uint16_t* o = (uint16_t*)dst + 4 * x;
                o[0] = Half(g_toLin[v & 255]); o[1] = Half(g_toLin[(v >> 8) & 255]);
                o[2] = Half(g_toLin[(v >> 16) & 255]); o[3] = Half((v >> 24) / 255.f);
            }
            break;
        default: {   // 8-bit UNORM, which the runtimes read as linear (as the eye and HUD blits assume)
            const bool bgra = fmt == DXGI_FORMAT_B8G8R8A8_UNORM;
            for (int x = 0; x < side; ++x) {
                const uint32_t v = src[x];
                const uint32_t r = (uint32_t)std::lround(g_toLin[v & 255] * 255.f), g = (uint32_t)std::lround(g_toLin[(v >> 8) & 255] * 255.f);
                const uint32_t b = (uint32_t)std::lround(g_toLin[(v >> 16) & 255] * 255.f);
                ((uint32_t*)dst)[x] = (bgra ? (b | (r << 16)) : (r | (b << 16))) | (g << 8) | (v & 0xFF000000u);
            }
            break;
        }
        }
    }
    g_gpu.cur[i] = next;
    g_gpu.staged[i] = fmt;
}

void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a; br.Transition.StateAfter = b;
    l->ResourceBarrier(1, &br);
}

} // namespace

void WeaponWheelCompose(IVRBackend* vr, const Dx12Frame& f, VRSubmitLayer& layer) {
    Published p;
    { std::lock_guard<std::mutex> g(g_pubMtx); p = g_pub; }
    // A worker that stopped refreshing (controllers gone, a stall) must not leave the wheel hanging.
    if (!p.visible || GetTickCount64() - p.tick > 300 || !vr || !f.device || !f.queue) return;
    if (!EnsureGpu(f.device)) return;
    if (!vr->EnsureOverlaySwapchain(OverlayWheel, kWheelPx, kWheelPx) ||
        !vr->EnsureOverlaySwapchain(OverlayWheelCursor, kCursorPx, kCursorPx)) return;
    const DXGI_FORMAT fmt[OverlayCount] = {vr->OverlayFormat(OverlayWheel), vr->OverlayFormat(OverlayWheelCursor)};
    // New content is copied out under the lock and staged outside it: staging may wait on the GPU.
    std::vector<uint32_t> wheelImg, cursorImg;
    uint32_t wheelVersion = 0;
    {
        std::lock_guard<std::mutex> g(g_pubMtx);
        if ((g_gpu.cur[OverlayWheel] < 0 || g_gpu.version[OverlayWheel] != g_pub.version || g_gpu.staged[OverlayWheel] != fmt[0]) &&
            g_pubImg.size() == (size_t)kWheelPx * kWheelPx) { wheelImg = g_pubImg; wheelVersion = g_pub.version; }
        if ((g_gpu.cur[OverlayWheelCursor] < 0 || g_gpu.staged[OverlayWheelCursor] != fmt[1]) &&
            g_cursorImg.size() == (size_t)kCursorPx * kCursorPx) cursorImg = g_cursorImg;
    }
    if (!wheelImg.empty()) { Stage(OverlayWheel, wheelImg, fmt[0]); g_gpu.version[OverlayWheel] = wheelVersion; }
    if (!cursorImg.empty()) Stage(OverlayWheelCursor, cursorImg, fmt[1]);
    const bool want[OverlayCount] = {g_gpu.cur[OverlayWheel] >= 0, p.cursor && g_gpu.cur[OverlayWheelCursor] >= 0};
    ID3D12Resource* img[OverlayCount] = {};
    for (int i = 0; i < OverlayCount; ++i)
        if (want[i]) img[i] = vr->AcquireOverlayImage(i);
    const int slot = g_gpu.ring;
    g_gpu.ring = (g_gpu.ring + 1) % 3;
    WaitFence(g_gpu.allocFence[slot]);
    bool recorded = false;
    if ((img[0] || img[1]) && SUCCEEDED(g_gpu.alloc[slot]->Reset()) && SUCCEEDED(g_gpu.list->Reset(g_gpu.alloc[slot], nullptr))) {
        ID3D12GraphicsCommandList* l = g_gpu.list;
        for (int i = 0; i < OverlayCount; ++i) {
            if (!img[i]) continue;
            // OpenXR hands D3D12 colour images over in RENDER_TARGET and wants them back that way.
            Barrier(l, img[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = img[i]; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
            src.pResource = g_gpu.up[i][g_gpu.cur[i]].buf; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = 0;
            src.PlacedFootprint.Footprint.Format = img[i]->GetDesc().Format;   // the image's own (maybe typeless) format: a bit copy
            src.PlacedFootprint.Footprint.Width = (UINT)kSidePx[i];
            src.PlacedFootprint.Footprint.Height = (UINT)kSidePx[i];
            src.PlacedFootprint.Footprint.Depth = 1;
            src.PlacedFootprint.Footprint.RowPitch = RowPitch(kSidePx[i]);
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Barrier(l, img[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        if (SUCCEEDED(l->Close())) {
            ID3D12CommandList* lists[] = {l};
            f.queue->ExecuteCommandLists(1, lists);
            f.queue->Signal(g_gpu.fence, ++g_gpu.fenceVal);
            g_gpu.allocFence[slot] = g_gpu.fenceVal;
            for (int i = 0; i < OverlayCount; ++i) if (img[i]) g_gpu.up[i][g_gpu.cur[i]].fence = g_gpu.fenceVal;
            recorded = true;
        }
    }
    for (int i = 0; i < OverlayCount; ++i) if (img[i]) vr->ReleaseOverlayImage(i);
    if (!recorded) return;
    if (img[OverlayWheel]) {
        VROverlayQuad& q = layer.overlays[OverlayWheel];
        q.valid = true; q.pose = p.wheelPose; q.widthM = q.heightM = p.wheelSize;
    }
    if (img[OverlayWheelCursor]) {
        VROverlayQuad& q = layer.overlays[OverlayWheelCursor];
        q.valid = true; q.pose = p.cursorPose; q.widthM = q.heightM = kCursorM;
    }
}

void WeaponWheelReleaseGpu() { ReleaseGpu(); }

} // namespace ervr
