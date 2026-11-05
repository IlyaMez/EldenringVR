#pragma once
// A "the world is loading / warping" signal that stays set while the player walks a running world is stale.
//
// ERVR stands down (no camera, pose or facing writes: the game's own third-person camera; no second pass: a flat
// picture) while the world is torn down or rebuilt, keyed on two game values: the loading-screen byte
// ([[CSEventFlagMan]+0x28]+0x113, eight event flags read as one byte, the byte TGA's Seamless-Coop loading fix
// polls) and GameMan.warp_requested. Reported 2026-11-04/05 by three players, all right after the Grafted
// Scion's scripted death: in the Stranded Graveyard the view went flat, third person, the Tarnished's head hidden
// - ERVR's stand-down with the world plainly running - and stayed so, restarts included. A signal stuck on (a
// flag in that byte that the game sets for good there, or a warp request left standing) fits every part of it.
//
// The world is running when the player walks it: the move stick pushed, the character's position moving in
// walking-size steps, and the in-game clock running. None of that holds in a loading screen (the clock stops:
// "igt x0.00" at the title, x0.25 across a 3 s load in players' logs), a cutscene (the stick moves nothing) or a
// warp (one big step). A signal that has held for StaleAfterMs while that is true is stale: ignored until it next
// reads clear, so the next real load still counts.
#include <cmath>
#include <cstdint>

namespace ervr {

struct WalkSample {
    uint64_t ms = 0;            // wall clock
    float x = 0, y = 0, z = 0;  // the player's world position
    uint32_t igtMs = 0;         // in-game clock
    bool pushing = false;       // the move stick pushed past half way
    bool valid = false;         // a player and a position were read
};

class WalkWindow {
public:
    static constexpr int kSamples = 48;          // ~4.8 s at the 10 Hz the caller samples
    static constexpr uint64_t kSpanMs = 3000;    // judged over the last 3 s
    static constexpr float kMinPath = 2.0f;      // metres walked along the way...
    static constexpr float kMinNet = 1.2f;       // ...and from start to end (not pacing on the spot)
    static constexpr float kMaxStep = 3.0f;      // per sample; more is a warp or a respawn, not walking
    static constexpr float kMinIgtRate = 0.6f;   // in-game clock vs wall clock
    static constexpr float kMinPushing = 0.5f;   // share of samples with the stick pushed

    void Add(const WalkSample& s) {
        ring_[head_] = s;
        head_ = (head_ + 1) % kSamples;
        if (count_ < kSamples) ++count_;
    }
    void Clear() { count_ = 0; head_ = 0; }

    // Walking over the last kSpanMs (newest sample at or before nowMs). Out: path length and clock rate.
    bool Walking(uint64_t nowMs, float* pathOut = nullptr, float* igtRateOut = nullptr) const {
        if (pathOut) *pathOut = 0.f;
        if (igtRateOut) *igtRateOut = 0.f;
        if (count_ < 2) return false;
        const WalkSample* newest = &At(count_ - 1);
        if (!newest->valid || newest->ms > nowMs || nowMs - newest->ms > 500) return false;
        // Walk back to the oldest sample inside the span; every sample in it must be valid.
        int first = count_ - 1;
        while (first > 0) {
            const WalkSample& prev = At(first - 1);
            if (!prev.valid || newest->ms - prev.ms > kSpanMs) break;
            --first;
        }
        const WalkSample& oldest = At(first);
        const uint64_t span = newest->ms - oldest.ms;
        if (span + 500 < kSpanMs) return false;   // not enough history yet
        float path = 0.f;
        int pushing = 0, n = 0;
        for (int i = first; i <= count_ - 1; ++i) {
            const WalkSample& s = At(i);
            ++n;
            if (s.pushing) ++pushing;
            if (i == first) continue;
            const WalkSample& p = At(i - 1);
            const float step = Dist(p, s);
            if (step > kMaxStep) return false;
            path += step;
        }
        const float net = Dist(oldest, *newest);
        const float rate = newest->igtMs >= oldest.igtMs
                               ? (float)(newest->igtMs - oldest.igtMs) / (float)(span ? span : 1) : 0.f;
        if (pathOut) *pathOut = path;
        if (igtRateOut) *igtRateOut = rate;
        return path >= kMinPath && net >= kMinNet && rate >= kMinIgtRate &&
               (float)pushing >= kMinPushing * (float)n;
    }

private:
    // i = 0 is the oldest sample held.
    const WalkSample& At(int i) const { return ring_[(head_ - count_ + i + kSamples) % kSamples]; }
    static float Dist(const WalkSample& a, const WalkSample& b) {
        const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    WalkSample ring_[kSamples]{};
    int head_ = 0, count_ = 0;
};

// One transition signal's stale state.
struct StaleSignal {
    static constexpr uint64_t kStaleAfterMs = 6000;   // a real load or warp request never runs this long walkable
    enum Event { kNone, kWentStale, kClearedAfterStale };
    uint64_t sinceMs = 0;   // when the raw signal last went set (0 = clear)
    bool stale = false;

    // The effective signal: raw, unless it went stale while set.
    bool Filter(bool raw, uint64_t nowMs, bool walking, Event* event = nullptr) {
        if (event) *event = kNone;
        if (!raw) {
            if (stale && event) *event = kClearedAfterStale;
            sinceMs = 0;
            stale = false;
            return false;
        }
        if (!sinceMs) sinceMs = nowMs ? nowMs : 1;
        if (stale) return false;
        if (walking && nowMs - sinceMs >= kStaleAfterMs) {
            stale = true;
            if (event) *event = kWentStale;
            return false;
        }
        return true;
    }
};
}
