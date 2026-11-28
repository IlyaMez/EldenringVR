#include "third_person_camera.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace ervr;

static void Near(const Vec3& actual, const Vec3& expected, const char* what) {
    if ((actual - expected).Length() < 0.0001f) return;
    std::fprintf(stderr, "%s: got (%f, %f, %f), expected (%f, %f, %f)\n", what,
                 actual.x, actual.y, actual.z, expected.x, expected.y, expected.z);
    std::exit(1);
}

int main() {
    Pose stock;
    stock.pos = {10.f, 2.f, 3.f};
    stock.rot = Quat::FromEulerYXZ(Deg2Rad(60.f), Deg2Rad(25.f), 0.f);
    Pose seated;
    seated.pos = {0.3f, 1.15f, -0.5f};
    seated.rot = Quat::YawQ(Deg2Rad(45.f));
    const Pose neutral = ThirdPersonHead(stock, seated, seated, 1.f);
    Near(neutral.pos, stock.pos, "seated height and room offset cancel");
    Near(neutral.rot.Forward(), stock.rot.Forward(), "neutral head preserves stock camera pitch and yaw");

    Pose moved = seated;
    moved.pos = moved.pos + seated.rot.Rotate({0.2f, 0.1f, 0.f});
    const Quat turn = Quat::YawQ(Deg2Rad(30.f));
    moved.rot = seated.rot * turn;
    const Pose head = ThirdPersonHead(stock, moved, seated, 1.f);
    Near(head.pos, stock.pos + stock.rot.Rotate({0.2f, 0.1f, 0.f}), "translation relative to seated origin");
    Near(head.rot.Forward(), (stock.rot * turn).Forward(), "head turn relative to game camera");
    const Pose left = ThirdPersonEye(head, {-0.032f, 0.f, 0.f}, Quat::YawQ(-0.04f));
    const Pose right = ThirdPersonEye(head, {0.032f, 0.f, 0.f}, Quat::YawQ(0.04f));
    Near((left.pos + right.pos) * 0.5f, head.pos, "stereo pair shares head center");
    Near(right.pos - left.pos, head.rot.Rotate({0.064f, 0.f, 0.f}), "canted eyes keep head-frame separation");
    Near(right.rot.Forward(), (head.rot * Quat::YawQ(0.04f)).Forward(), "right-eye cant");
}
