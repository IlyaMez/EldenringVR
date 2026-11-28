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
    const float yaw = Deg2Rad(60.f);
    const Quat level = Quat::YawQ(yaw);
    stock.rot = Quat::FromEulerYXZ(yaw, Deg2Rad(25.f), Deg2Rad(12.f));
    Pose seated;
    seated.pos = {0.3f, 1.15f, -0.5f};
    seated.rot = Quat::YawQ(Deg2Rad(45.f));
    const Pose neutral = ThirdPersonHead(stock, seated, seated, 1.f);
    Near(neutral.pos, stock.pos, "seated height and room offset cancel");
    Near(neutral.rot.Forward(), level.Forward(), "level head keeps stock yaw");
    Near(neutral.rot.Up(), {0.f, 1.f, 0.f}, "stock roll leaves the horizon level");

    Pose orbited = stock;
    orbited.pos = {6.f, 4.f, 5.f};
    const float orbitYaw = Deg2Rad(-30.f);
    orbited.rot = Quat::FromEulerYXZ(orbitYaw, Deg2Rad(-50.f), Deg2Rad(-8.f));
    const Pose orbitView = ThirdPersonHead(orbited, seated, seated, 1.f);
    Near(orbitView.pos, orbited.pos, "stock orbit keeps all three position axes");
    Near(orbitView.rot.Forward(), Quat::YawQ(orbitYaw).Forward(), "stock orbit turns the view horizontally");
    Near(orbitView.rot.Up(), neutral.rot.Up(), "changing the stock pitch and roll leaves world up fixed");

    Pose vertical = stock;
    vertical.rot = Quat::FromEulerYXZ(yaw, Deg2Rad(90.f), 0.f);
    const Pose verticalView = ThirdPersonHead(vertical, seated, seated, 1.f);
    Near(verticalView.rot.Forward(), neutral.rot.Forward(), "vertical stock camera keeps its heading");

    Pose moved = seated;
    moved.pos = moved.pos + seated.rot.Rotate({0.2f, 0.1f, 0.f});
    const Quat turn = Quat::FromEulerYXZ(Deg2Rad(30.f), Deg2Rad(-20.f), Deg2Rad(15.f));
    moved.rot = seated.rot * turn;
    const Pose head = ThirdPersonHead(stock, moved, seated, 1.f);
    Near(head.pos, stock.pos + level.Rotate({0.2f, 0.1f, 0.f}), "head lean uses level tracking axes");
    Near(head.rot.Forward(), (level * turn).Forward(), "head turn and pitch follow tracking");
    Near(head.rot.Up(), (level * turn).Up(), "head tilt follows tracking");
    const Pose left = ThirdPersonEye(head, {-0.032f, 0.f, 0.f}, Quat::YawQ(-0.04f));
    const Pose right = ThirdPersonEye(head, {0.032f, 0.f, 0.f}, Quat::YawQ(0.04f));
    Near((left.pos + right.pos) * 0.5f, head.pos, "stereo pair shares head center");
    Near(right.pos - left.pos, head.rot.Rotate({0.064f, 0.f, 0.f}), "canted eyes keep head-frame separation");
    Near(right.rot.Forward(), (head.rot * Quat::YawQ(0.04f)).Forward(), "right-eye cant");
}
