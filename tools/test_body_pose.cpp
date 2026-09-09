#include "body_pose.h"
#include <array>
#include <cstdio>
#include <cstdlib>

using namespace ervr;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}
int main() {
    constexpr std::array names{"R_Hand", "R_Finger0", "R_Finger02", "L_Finger1", "L_Finger12", "R_Weapon", "Spine", "L_Thigh"};
    std::array<uint8_t, names.size()> fingers{};
    for (size_t i = 0; i < names.size(); ++i) fingers[i] = IsFingerBone(names[i]);
    Check(!fingers[0] && fingers[1] && fingers[2] && fingers[3] && fingers[4] && !fingers[5],
          "finger selection must cover both hands and joint depths without taking over the wrist or weapon");
    std::array<int, names.size()> stance{};
    for (int animationGrip : {1, 2, 3, 2, 1}) {
        auto live = stance;
        live.fill(animationGrip);
        ApplyBodyPoseTemplate<int>(live, stance, fingers);
        for (size_t i = 0; i < live.size(); ++i)
            Check(live[i] == (fingers[i] ? animationGrip : 0),
                  "current equipment grip must survive a frozen open-hand stance while the body stays fixed");
        const auto previous = live;
        ApplyBodyPoseTemplate<int>(live, stance, fingers);
        Check(live == previous, "repeated pose hooks must preserve the current grip");
    }
    std::puts("Body/finger pose regression checks passed.");
}
