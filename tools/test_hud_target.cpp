#include "hud_target.h"
#include <cstdio>
#include <cstdlib>

using namespace ervr;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}

constexpr uint32_t kW = 3712, kH = 2088;
constexpr uint32_t kR11G11B10 = 26, kR8G8B8A8 = 28, kR10G10B10A2 = 24, kR16G16B16A16F = 10;

HudRun Target(uint64_t res, int draws, uint32_t fmt, uint32_t w = kW, uint32_t h = kH) {
    HudRun r;
    r.res = res; r.draws = draws; r.fmt = fmt; r.w = w; r.h = h;
    return r;
}
HudRun BackBuffer(uint64_t res, int draws) {
    HudRun r = Target(res, draws, kR10G10B10A2);
    r.backBuffer = true;
    return r;
}

int main() {
    Check(HudUiFormat(kR8G8B8A8), "R8G8B8A8 can hold the UI");
    Check(!HudUiFormat(kR10G10B10A2), "an offscreen R10G10B10A2 target is a scene buffer (normals), not the UI");
    Check(!HudUiFormat(kR11G11B10), "R11G11B10 float is a scene buffer");
    Check(!HudUiFormat(kR16G16B16A16F), "R16G16B16A16 float is a scene buffer");

    // In play: scene HDR passes, a half-res bloom chain, the tonemap + ~55 HUD batches on
    // the R8G8B8A8 target, then the one copy to the back buffer.
    {
        const HudRun runs[] = {Target(0xa1, 6, kR11G11B10), Target(0xb1, 12, kR11G11B10, kW / 2, kH / 2),
                               Target(0xaf5810, 2, kR11G11B10), Target(0x111aa0, 56, kR8G8B8A8),
                               BackBuffer(0xbb, 1)};
        int rejected = -2;
        Check(HudPickUiTarget(runs, 5, 2, kW, kH, &rejected) == 3, "in play: the UI target");
        Check(rejected == -1, "in play: nothing passed over before the UI target");
    }

    // The 2026-11-01 boss cutscene: the UI target is down to its tonemap draw. The scene
    // buffer with 2 draws used to be learned as the UI; now nothing is.
    {
        const HudRun runs[] = {Target(0xa1, 6, kR11G11B10), Target(0xaf5810, 2, kR11G11B10),
                               Target(0x111aa0, 1, kR8G8B8A8), BackBuffer(0xbb, 1)};
        int rejected = -2;
        Check(HudPickUiTarget(runs, 4, 2, kW, kH, &rejected) == -1, "cutscene: no UI target");
        Check(rejected == 1, "cutscene: the 2-draw scene buffer was passed over");
    }

    // The 2026-11-07 boss phase-2 cinematic (co-op, 1.17.2): the UI target took no draw at all, and the last
    // full-size multi-draw run was an offscreen R10G10B10A2 buffer (3 draws) - learned as the UI, it put a normals
    // pass on the HUD layer until the death's load. Not a UI target now; the back buffer still is.
    {
        const HudRun runs[] = {Target(0xa1, 6, kR11G11B10), Target(0x700e00, 3, kR10G10B10A2),
                               BackBuffer(0xbb, 1)};
        int rejected = -2;
        Check(HudPickUiTarget(runs, 3, 2, kW, kH, &rejected) == -1, "cinematic: the 10-bit scene buffer is not the UI");
        Check(rejected == 1, "cinematic: the 10-bit scene buffer was passed over");
        const HudRun title[] = {Target(0xa1, 4, kR11G11B10), BackBuffer(0xbb, 3)};
        Check(HudPickUiTarget(title, 2, 2, kW, kH) == 1, "a 10-bit back buffer is still taken");
    }

    // 1.17.2 title and loading screens: the UI on the same R8G8B8A8 target.
    {
        const HudRun runs[] = {Target(0x111aa0, 8, kR8G8B8A8), BackBuffer(0xbb, 1)};
        Check(HudPickUiTarget(runs, 2, 2, kW, kH) == 0, "title: the UI target");
    }

    // 1.16.2 title: the UI straight onto the back buffer (output blit + two UI batches).
    {
        const HudRun runs[] = {Target(0xa1, 4, kR11G11B10), BackBuffer(0xbb, 3)};
        Check(HudPickUiTarget(runs, 2, 2, kW, kH) == 1, "1.16.2 title: the back buffer");
    }

    // FirstDraw raised: a target needs FirstDraw + 1 draws to count.
    {
        const HudRun runs[] = {Target(0x111aa0, 3, kR8G8B8A8), BackBuffer(0xbb, 1)};
        Check(HudPickUiTarget(runs, 2, 4, kW, kH) == -1, "FirstDraw 3: 3 draws is no UI");
        Check(HudPickUiTarget(runs, 2, 3, kW, kH) == 0, "FirstDraw 2: 3 draws is UI");
    }

    // Off 16:9 the game renders every target a few px short of the back buffer.
    {
        const HudRun runs[] = {Target(0x111aa0, 56, kR8G8B8A8, 3628, 2040), BackBuffer(0xbb, 1)};
        Check(HudPickUiTarget(runs, 2, 2, 3632, 2040) == -1, "off 16:9: no full-size target");
    }

    // A buffer that is not a 2D texture never qualifies.
    {
        HudRun buf = Target(0xc1, 9, kR8G8B8A8);
        buf.texture2d = false;
        const HudRun runs[] = {Target(0x111aa0, 20, kR8G8B8A8), buf};
        Check(HudPickUiTarget(runs, 2, 2, kW, kH) == 0, "a buffer is skipped");
    }

    Check(HudUiAbsentNotMoved(true, 1), "learned target took its tonemap draw: UI absent, kept");
    Check(!HudUiAbsentNotMoved(true, 0), "learned target not drawn to at all: it moved");
    Check(!HudUiAbsentNotMoved(false, 1), "back buffer always takes the copy: relearn as before");

    std::puts("hud_target: ok");
    return 0;
}
