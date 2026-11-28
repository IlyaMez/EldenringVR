#pragma once
#include "vrmath.h"

namespace ervr {

// All poses are in game convention. Keep the recenter axes level even if the
// player is looking up or down when they set their seated origin.
inline Pose ThirdPersonHead(const Pose& camera, const Pose& tracking, const Pose& origin, float scale) {
    const Quat worldFromTracking = camera.rot * origin.rot.Conj();
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
