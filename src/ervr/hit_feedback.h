#pragma once
#include "vrmath.h"
#include <cmath>

namespace ervr {

// What a landed hit feels like, in the hands and in the blade (2026-11-21: "hitting enemies is hard to feel").
// Pure: combat.cpp sizes a hit, virtual_pad.cpp plays its impact on the controllers, game_er.cpp holds the blade back.

// How hard a landed hit was, 0..1, from the damage multiplier its contact ran with (1 = a vanilla hit): the slowest
// contact (KineticMinMult) is 0, a full-speed swing 1. KineticDamageScale is taken back out first, so a lowered
// damage setting still feels a clean hit as one.
inline float HitStrength(float hitMult, float minMult, float damageScale) {
    const float m = hitMult / (damageScale > 0.05f ? damageScale : 0.05f);
    const float lo = minMult < 0.95f ? minMult : 0.95f;
    return Clamp((m - lo) / (1.f - lo), 0.f, 1.f);
}

// The impact on the controller: a hard crack, then a thud that dies away. A harder hit cracks harder and thuds
// longer and stronger; a heavier weapon (the game's weight, about 1-30) thuds longer.
struct HitHaptic { float crackSec = 0.f, crackAmp = 0.f, tailSec = 0.f, tailAmp = 0.f; };
inline HitHaptic HitHapticFor(float strength, float weight) {
    const float s = Clamp(strength, 0.f, 1.f), w = Clamp(weight / 20.f, 0.f, 1.f);
    HitHaptic h;
    h.crackSec = 0.025f + 0.015f * s;
    h.crackAmp = 0.5f + 0.5f * s;
    h.tailSec = (0.05f + 0.15f * s) * (0.8f + 0.6f * w);
    h.tailAmp = 0.15f + 0.6f * s;
    return h;
}
// Amplitude t seconds after the hit; negative once the impact is over.
inline float HitHapticAmp(const HitHaptic& h, float t) {
    if (t < 0.f) return 0.f;
    if (t < h.crackSec) return h.crackAmp;
    const float u = (t - h.crackSec) / (h.tailSec > 1e-3f ? h.tailSec : 1e-3f);
    if (u >= 1.f) return -1.f;
    const float k = 1.f - u;
    return h.tailAmp * k * k;   // eases out: no step at the end
}

// The blade's bite: at a landed hit the blade stays where it struck for a moment and then rejoins the hand, the way a
// real one drags in what it cuts. The share of the hand's motion the blade gives up, t seconds after the hit: `hold`
// at first, flat for a moment, back to 0 at `seconds` with no jerk on either side (1 - smoothstep).
inline float BiteHold(float t, float seconds, float hold) {
    if (seconds <= 0.f || hold <= 0.f || t < 0.f || t >= seconds) return 0.f;
    const float u = t / seconds;
    return Clamp(hold, 0.f, 1.f) * (1.f - u * u * (3.f - 2.f * u));
}
// How long and how hard a hit of `strength` bites (HitBiteSeconds / HitBiteHold are a full hit's).
inline void BiteFor(float strength, float fullSeconds, float fullHold, float* seconds, float* hold) {
    const float s = Clamp(strength, 0.f, 1.f);
    *seconds = fullSeconds > 0.f ? fullSeconds * (0.6f + 0.4f * s) : 0.f;
    *hold = fullHold > 0.f ? Clamp(fullHold, 0.f, 1.f) * s : 0.f;
}

// Shortest-path spherical interpolation from a (t = 0) to b (t = 1).
inline Quat QuatSlerp(const Quat& a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.f) { b = Quat{-b.x, -b.y, -b.z, -b.w}; d = -d; }
    float wa = 1.f - t, wb = t;
    if (d < 0.9995f) {
        const float th = std::acos(d), s = std::sin(th);
        wa = std::sin((1.f - t) * th) / s; wb = std::sin(t * th) / s;
    }
    return Quat{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb}.Normalized();
}

} // namespace ervr
