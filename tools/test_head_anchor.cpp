#include "head_anchor.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
using ervr::HeadAnchorState;
using ervr::Vec3;

void Near(float actual, float expected, const char* behavior) {
    if (!std::isfinite(actual) || std::fabs(actual - expected) > 0.0001f) {
        std::fprintf(stderr, "%s: expected %.6f, got %.6f\n", behavior, expected, actual);
        std::exit(1);
    }
}

void Between(float actual, float low, float high, const char* behavior) {
    if (!(actual > low && actual < high)) {
        std::fprintf(stderr, "%s: expected between %.6f and %.6f, got %.6f\n",
                     behavior, low, high, actual);
        std::exit(1);
    }
}

void TerrainAndStanceStayAttached() {
    HeadAnchorState anchor;
    Vec3 root{4.f, 5.56f, -2.f};
    const Vec3 firstHead = root + Vec3{0.f, 1.56f, 0.f};
    Vec3 eye = anchor.Update(root, firstHead, 1.60f, 1.f, 0.06f, true, 1.0, 10);
    Near(eye.y - root.y, 1.60f, "initial eye height calibration");
    Near(eye.y - firstHead.y, 0.04f, "initial eye-to-head offset");

    // A late model update may change root height after an earlier camera hook
    // has already sampled this frame. Terrain motion must still pass through.
    for (float ground : {5.9f, 6.4f, 5.2f, -6.6f}) {
        root.y = ground;
        Vec3 head = root + Vec3{0.f, 1.56f, 0.f};
        eye = anchor.Update(root, head, 1.60f, 1.f, 0.06f, true, 1.001, 10);
        Near(eye.y - root.y, 1.60f, "terrain movement passes through within a frame");
        Near(eye.y - head.y, 0.04f, "terrain preserves the eye-to-head offset");
    }

    // Reproduce the log's settled template changing from 1.56 m to 1.46 m,
    // then more stance changes in the same frame. None may rebase calibration.
    for (float height : {1.46f, 1.20f, 1.56f, 1.50f}) {
        Vec3 head = root + Vec3{0.f, height, 0.f};
        eye = anchor.Update(root, head, 1.60f, 1.f, 0.06f, true, 1.002, 10);
        Near(eye.y - head.y, 0.04f, "same-frame stance capture preserves attachment");
        Near(anchor.restHeight, 1.56f, "stance capture preserves original calibration");
    }
}

void SmoothingRetainsItsPurpose() {
    HeadAnchorState anchor;
    anchor.Update({}, {0.f, 1.56f, 0.f}, 1.60f, 1.f, 0.06f, true, 2.0, 20);
    const Vec3 first = anchor.Update({}, {0.3f, 1.46f, -0.3f}, 1.60f, 1.f, 0.06f, true, 2.01, 21);
    Between(first.x, 0.f, 0.3f, "horizontal head motion is smoothed");
    Between(first.z, -0.3f, 0.f, "depth head motion is smoothed");
    Near(first.y, 1.50f, "VR stance height is immediate despite smoothing");
    const Vec3 repeated = anchor.Update({}, {0.3f, 1.36f, -0.3f}, 1.60f, 1.f, 0.06f, true, 2.011, 21);
    Near(repeated.x, first.x, "multiple hooks do not advance horizontal smoothing twice");
    Near(repeated.z, first.z, "multiple hooks do not advance depth smoothing twice");
    Near(repeated.y, 1.40f, "late VR stance height bypasses the frame smoothing guard");

    anchor.Reset();
    anchor.Update({}, {0.f, 1.56f, 0.f}, 1.60f, 1.f, 0.06f, false, 3.0, 30);
    const Vec3 animated = anchor.Update({}, {0.f, 1.46f, 0.f}, 1.60f, 1.f, 0.06f, false, 3.01, 31);
    Between(animated.y, 1.50f, 1.60f, "game animation bob remains smoothed");
    const Vec3 moved = anchor.Update({0.f, 0.5f, 0.f}, {0.f, 1.96f, 0.f},
                                     1.60f, 1.f, 0.06f, false, 3.011, 31);
    Near(moved.y - animated.y, 0.5f, "animated camera also follows terrain immediately");
}

void ConfigurationAndSkeletonReset() {
    HeadAnchorState anchor;
    anchor.Update({}, {0.f, 1.56f, 0.f}, 1.60f, 0.f, 0.06f, true, 4.0, 40);
    const Vec3 noBob = anchor.Update({}, {0.f, 1.20f, 0.f}, 1.60f, 0.f, 0.06f, true, 4.01, 41);
    Near(noBob.y, 1.60f, "CameraBob zero retains configured eye height");

    anchor.Reset();
    const Vec3 newSkeleton = anchor.Update({0.f, 8.f, 0.f}, {0.f, 9.7f, 0.f},
                                           1.60f, 1.f, 0.06f, true, 4.02, 41);
    Near(newSkeleton.y, 9.60f, "skeleton reset establishes a new eye calibration");
    Near(anchor.restHeight, 1.70f, "skeleton reset discards the previous rest height");
}

void RidingTracksTheSeatedHead() {
    HeadAnchorState anchor;
    anchor.Update({}, {0.f, 1.56f, 0.f}, 1.60f, 1.f, 0.06f, true, 5.0, 50);
    // The model root is already on the saddle. Its seated head is lower relative
    // to that root than a standing head; adding standing EyeHeight is too tall.
    Vec3 root{4.f, 2.f, -2.f};
    for (float bob : {0.f, 0.5f, 1.f}) {
        for (float height : {0.82f, 0.76f, 0.92f}) {
            root = root + Vec3{0.3f, 0.1f, -0.2f};
            const Vec3 head = root + Vec3{0.25f, height, -0.15f};
            const Vec3 eye = anchor.Update(root, head, 1.60f, bob, 0.06f, false, 5.001, 50, true);
            Near(eye.x, head.x, "rider head stays attached through late horizontal updates");
            Near(eye.z, head.z, "rider head stays attached through late depth updates");
            Near(eye.y - head.y, 0.04f, "saddle height is counted once regardless of CameraBob");
            Near(anchor.restHeight, 1.56f, "riding preserves standing calibration");
        }
    }
    const Vec3 footRoot{1.f, 0.3f, 2.f};
    const Vec3 footHead = footRoot + Vec3{0.f, 1.56f, 0.f};
    const Vec3 eye = anchor.Update(footRoot, footHead, 1.60f, 1.f, 0.06f, true, 5.002, 50);
    Near(eye.x, footHead.x, "dismount discards the saddle's horizontal offset in the same frame");
    Near(eye.z, footHead.z, "dismount discards the saddle's depth offset in the same frame");
    Near(eye.y - footRoot.y, 1.60f, "dismount restores standing height immediately");
}

void LoadingOnTorrentDoesNotCalibrateFromTheSaddle() {
    HeadAnchorState anchor;
    const Vec3 root{1.f, 3.f, 2.f}, head = root + Vec3{0.2f, 0.8f, -0.1f};
    for (uint32_t frame : {60u, 60u, 61u, 62u}) {
        const Vec3 eye = anchor.Update(root, head, 1.60f, 0.f, 0.06f, false, 6.0, frame, true);
        Near(eye.y, head.y, "loading mounted starts at the live head without a standing offset");
        Near(anchor.restHeight, -1.f, "mounted startup leaves standing calibration unset");
    }
    const Vec3 standing = anchor.Update({}, {0.f, 1.56f, 0.f}, 1.60f, 1.f, 0.06f, true, 6.1, 63);
    Near(standing.y, 1.60f, "first dismount establishes normal eye height");
    Near(anchor.restHeight, 1.56f, "first dismount calibrates from the standing head");
    anchor.Update(root, head, 1.60f, 1.f, 0.06f, false, 6.2, 64, true);
    anchor.Reset();
    const Vec3 reset = anchor.Update(root, head, 1.60f, 1.f, 0.06f, false, 6.3, 64, true);
    Near(reset.y, head.y, "mounted skeleton rebuild discards old calibration");
}
} // namespace

int main() {
    TerrainAndStanceStayAttached();
    SmoothingRetainsItsPurpose();
    ConfigurationAndSkeletonReset();
    RidingTracksTheSeatedHead();
    LoadingOnTorrentDoesNotCalibrateFromTheSaddle();
    std::puts("Head anchor regression checks passed.");
}
