#include "pose_frame.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
using namespace ervr;

void Near(Vec3 actual, Vec3 expected, const char* what) {
    const float error = (actual - expected).Length();
    if (!std::isfinite(error) || error > 0.0001f) {
        std::fprintf(stderr, "%s: error %.6f m\n", what, error);
        std::exit(1);
    }
}

Vec3 RenderBone(Vec3 position, Quat frame, Vec3 rootT, Quat rootQ, Vec3 child) {
    return position + frame.Rotate(rootT + rootQ.Rotate(child));
}

void CheckFrames(float facing, float mismatch) {
    const Quat solveQ = Quat::YawQ(facing);
    // Include tilt as well as yaw: terrain and late interpolation can change both.
    const Quat renderQ = Quat::YawQ(facing + mismatch) * Quat::AxisAngle({1, 0, 0}, 0.12f);
    const Vec3 solvePos{3.f, 8.f, -4.f}, renderPos{3.04f, 7.92f, -4.03f};
    const Vec3 baseT{0.04f, 0.02f, 0.13f}, head{0.03f, 1.56f, 0.08f};
    const Quat baseQ = Quat::YawQ(0.31f);
    const Vec3 neutral = RenderBone(solvePos, solveQ, baseT, baseQ, head);
    for (const Vec3 lean : {Vec3{0.30f, 0, 0}, Vec3{-0.30f, 0, 0},
                            Vec3{0, 0, 0.25f}, Vec3{0, 0, -0.25f}, Vec3{0, -0.35f, 0}}) {
        // This is UpdateArms' order: template + yaw + follow shift in the solve frame,
        // then convert the completed root into the native render frame.
        const Vec3 solvedT = baseT + solveQ.Conj().Rotate(lean);
        for (int pass = 0; pass < 4; ++pass) {
            Vec3 outputT = solvedT; Quat outputQ = baseQ;
            RebasePoseRoot(solvePos, solveQ, renderPos, renderQ, outputT, outputQ);
            Near(RenderBone(renderPos, renderQ, outputT, outputQ, head) - neutral, lean,
                 "rendered head follows the physical lean in the same direction");
            Near((renderQ * outputQ).Rotate({0, 0, -1}), (solveQ * baseQ).Rotate({0, 0, -1}),
                 "rendered body retains its solved facing");
            Near(RenderBone(renderPos, renderQ, outputT, outputQ, {0.45f, 1.25f, 0.30f}),
                 RenderBone(solvePos, solveQ, solvedT, baseQ, {0.45f, 1.25f, 0.30f}),
                 "child hand targets stay in the same world frame as the body");
        }
    }

    // Native root movement must pass through in its original direction, even when
    // the render orientation is half a turn behind the physics orientation.
    const Vec3 step = solveQ.Rotate({0, 0, -0.20f});
    Vec3 t = baseT; Quat q = baseQ;
    RebasePoseRoot(solvePos + step, solveQ, renderPos + step, renderQ, t, q);
    Near(RenderBone(renderPos + step, renderQ, t, q, head) - neutral, step,
         "forward root movement is not inverted");
}
} // namespace

int main() {
    for (float facing : {0.f, 1.5707963f, 3.14159265f, -1.5707963f})
        for (float mismatch : {0.f, 0.2f, -0.4f, 1.5707963f, 3.14159265f})
            CheckFrames(facing, mismatch);
    std::puts("Pose frame regression checks passed.");
}
