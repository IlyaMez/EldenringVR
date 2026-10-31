#include "game_res.h"
#include <cstdio>
#include <cstdlib>

using namespace ervr;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}

// The game renders this 1:1: exactly 16:9, so its 16:9 render-size clamp is a no-op.
bool Exact169(uint32_t w, uint32_t h) {
    return (uint64_t)w * 9 == (uint64_t)h * 16 && w % 8 == 0 && h % 8 == 0;
}

void Auto(uint32_t eyeW, uint32_t eyeH, double scale, uint32_t& w, uint32_t& h) {
    Auto169Target(eyeW, eyeH, scale, w, h);
    SnapGameRes(w, h);
}

bool Is(uint32_t w, uint32_t h, uint32_t ew, uint32_t eh) { return w == ew && h == eh; }

int main() {
    uint32_t w = 0, h = 0;

    // Quest 3 through Virtual Desktop (2688x2880 per eye): the default lands on 3200x1800,
    // the size the old resolution-table snap delivered, and 1.0 on 3712x2088.
    Auto(2688, 2880, 0.86, w, h);
    Check(Is(w, h, 3200, 1800), "Quest 3 at 0.86 is 3200x1800");
    Auto(2688, 2880, 1.0, w, h);
    Check(Is(w, h, 3712, 2088), "Quest 3 at 1.0 is 3712x2088");

    // Quest Pro through Virtual Desktop (3072x3264): rounding each side to 8 asked 3632x2040,
    // which the game rendered at 3628x2040 inside black bars and the HUD lost the UI.
    Auto(3072, 3264, 0.86, w, h);
    Check(Is(w, h, 3584, 2016), "Quest Pro at 0.86 is 3584x2016");

    // Every auto size is exact 16:9 on whole multiples of 8, within half a grid step of the
    // unrounded height, and inside the 8192 limit.
    for (uint32_t ew = 1200; ew <= 4400; ew += 52)
        for (uint32_t eh = 1200; eh <= 4400; eh += 76)
            for (double s = 0.25; s <= 2.0001; s += 0.05) {
                uint32_t rw = 0, rh = 0;
                Auto169Target(ew, eh, s, rw, rh);
                Auto(ew, eh, s, w, h);
                Check(Exact169(w, h), "auto size is exact 16:9 on multiples of 8");
                Check(w <= 8192 && h >= 504, "auto size stays inside 896x504 .. 8192x4608");
                if (rh >= 504 && rh <= 4608)
                    Check(h + 36 >= rh && h <= rh + 36, "auto size is the nearest grid point");
            }

    // Exact 16:9 requests already on the grid are kept, built-in sizes included.
    const uint32_t kept[][2] = {{1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}, {3584, 2016}};
    for (const auto& k : kept) {
        w = k[0]; h = k[1];
        SnapGameRes(w, h);
        Check(Is(w, h, k[0], k[1]), "a grid 16:9 size is kept");
    }
    // A near-16:9 request is pulled onto the grid.
    w = 3632; h = 2040;
    SnapGameRes(w, h);
    Check(Is(w, h, 3584, 2016), "3632x2040 becomes 3584x2016");
    w = 3192; h = 1792;
    SnapGameRes(w, h);
    Check(Is(w, h, 3200, 1800), "3192x1792 becomes 3200x1800");

    // A request clearly off 16:9 is deliberate (the letterbox patch handles it): shape kept.
    w = 2000; h = 2000;
    SnapGameRes(w, h);
    Check(Is(w, h, 2000, 2000), "a square request keeps its shape");
    w = 3003; h = 2002;
    SnapGameRes(w, h);
    Check(Is(w, h, 3000, 2000), "a 3:2 request rounds to multiples of 8");

    // The fitted path's ceiling: never taller than the desktop.
    w = 1365; h = 768;
    SnapGameRes(w, h, 768);
    Check(Is(w, h, 1280, 720), "a 1366x768 desktop fits 1280x720");
    w = 1920; h = 1080;
    SnapGameRes(w, h, 1080);
    Check(Is(w, h, 1920, 1080), "a 1080p desktop keeps 1920x1080");

    // Huge requests clamp on the grid, not off it.
    w = 16000; h = 9000;
    SnapGameRes(w, h);
    Check(Is(w, h, 8192, 4608), "a huge request clamps to 8192x4608");

    std::puts("game_res ok");
    return 0;
}
