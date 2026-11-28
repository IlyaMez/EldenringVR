#pragma once
#include "vrmath.h"

namespace ervr {

// All poses are in game convention. The stock camera supplies position and
// heading. Only the tracked head can pitch or roll the view.
inline Pose ThirdPersonHead(const Pose& camera, const Pose& tracking, const Pose& origin, float scale) {
    const Vec3 forward = camera.rot.Forward();
    float yaw = std::atan2(-forward.x, -forward.z);
    if (forward.x * forward.x + forward.z * forward.z < 1e-6f) {
        // A nearly vertical camera has no reliable horizontal forward vector.
        const Vec3 right = camera.rot.Rotate({1.f, 0.f, 0.f});
        yaw = std::atan2(-right.z, right.x);
    }
    const Quat worldFromTracking = Quat::YawQ(yaw) * origin.rot.Conj();
    Pose head;
    head.pos = camera.pos + worldFromTracking.Rotate((tracking.pos - origin.pos) * scale);
    head.rot = (worldFromTracking * tracking.rot).Normalized();
    head.valid = true;
    return head;
}

inline Pose ThirdPersonEye(const Pose& head, const Vec3& offset, const Quat& rotation) {
    Pose eye;
    eye.pos = head.pos + head.rot.Rotate(offset);
    eye.rot = (head.rot * rotation).Normalized();
    eye.valid = true;
    return eye;
}

} // namespace ervr
