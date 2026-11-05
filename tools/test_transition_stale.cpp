// transition_stale.h: a loading/warp signal stuck on while the player walks is ignored; real loads are not.
#include "transition_stale.h"
#include <cstdio>
#include <cstdlib>

using namespace ervr;
static void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}

// 10 Hz samples from t0 for `seconds`, moving `speed` m/s along x, the in-game clock at `igtRate`.
static uint64_t Feed(WalkWindow& w, uint64_t t0, float seconds, float speed, float igtRate, bool pushing,
                     float x0 = 0.f, bool valid = true) {
    uint64_t t = t0;
    const int n = (int)(seconds * 10.f + 0.5f);
    for (int i = 0; i <= n; ++i) {
        t = t0 + (uint64_t)i * 100;
        WalkSample s;
        s.ms = t;
        s.x = x0 + speed * (float)i * 0.1f;
        s.igtMs = (uint32_t)(igtRate * (float)(i * 100));
        s.pushing = pushing;
        s.valid = valid;
        w.Add(s);
    }
    return t;
}

int main() {
    {   // Walking at 2 m/s with the stick pushed and the clock running: walking.
        WalkWindow w;
        const uint64_t t = Feed(w, 1000, 4.f, 2.f, 1.f, true);
        float path = 0, rate = 0;
        Check(w.Walking(t, &path, &rate), "walk at 2 m/s reads as walking");
        Check(path > 5.f && rate > 0.9f, "path and clock rate are reported");
    }
    {   // Same walk, clock stopped (a loading screen): not walking.
        WalkWindow w;
        const uint64_t t = Feed(w, 1000, 4.f, 2.f, 0.f, true);
        Check(!w.Walking(t), "clock stopped: not walking");
    }
    {   // Standing still with the stick pushed against a wall: not walking.
        WalkWindow w;
        const uint64_t t = Feed(w, 1000, 4.f, 0.f, 1.f, true);
        Check(!w.Walking(t), "no movement: not walking");
    }
    {   // Moved by something else, stick released (a cutscene carrying the character): not walking.
        WalkWindow w;
        const uint64_t t = Feed(w, 1000, 4.f, 2.f, 1.f, false);
        Check(!w.Walking(t), "stick released: not walking");
    }
    {   // One big step inside the window (a warp): not walking.
        WalkWindow w;
        uint64_t t = Feed(w, 1000, 2.f, 2.f, 1.f, true);
        t = Feed(w, t + 100, 2.f, 2.f, 1.f, true, 200.f);
        Check(!w.Walking(t), "a 200 m jump is a warp, not walking");
    }
    {   // Only one second of history: not enough.
        WalkWindow w;
        const uint64_t t = Feed(w, 1000, 1.f, 2.f, 1.f, true);
        Check(!w.Walking(t), "1 s of samples is not enough");
    }
    {   // Stale samples (nothing new for a second): not walking.
        WalkWindow w;
        const uint64_t t = Feed(w, 1000, 4.f, 2.f, 1.f, true);
        Check(!w.Walking(t + 1000), "an old window is not current");
    }
    {   // No player (invalid samples) inside the span: not walking.
        WalkWindow w;
        uint64_t t = Feed(w, 1000, 2.f, 2.f, 1.f, true);
        t = Feed(w, t + 100, 0.5f, 0.f, 1.f, false, 0.f, false);
        Check(!w.Walking(t), "player missing: not walking");
    }
    {   // The filter: a real load (set 4 s, nobody walking) stays a load and clears normally.
        StaleSignal s;
        StaleSignal::Event e;
        Check(s.Filter(true, 1000, false, &e) && e == StaleSignal::kNone, "load set");
        Check(s.Filter(true, 5000, false, &e), "load still set at 4 s");
        Check(!s.Filter(false, 5100, false, &e) && e == StaleSignal::kNone, "load clears");
    }
    {   // Set and walking, but not for long: still honoured (a load that just started under a walking player).
        StaleSignal s;
        Check(s.Filter(true, 1000, true), "fresh signal honoured even while walking");
        Check(s.Filter(true, 6900, true), "5.9 s: still honoured");
    }
    {   // Stuck: 6 s set and walking -> stale, stays ignored, and a clear re-arms it for the next real load.
        StaleSignal s;
        StaleSignal::Event e;
        s.Filter(true, 1000, false);
        Check(s.Filter(true, 6000, false), "stuck but nobody walking yet: honoured");
        Check(!s.Filter(true, 7000, true, &e) && e == StaleSignal::kWentStale, "goes stale while walking");
        Check(!s.Filter(true, 60000, false, &e) && e == StaleSignal::kNone, "stays ignored while set");
        Check(!s.Filter(false, 61000, false, &e) && e == StaleSignal::kClearedAfterStale, "clear is reported");
        Check(s.Filter(true, 62000, false), "the next real load is honoured again");
    }
    std::puts("transition_stale: ok");
    return 0;
}
