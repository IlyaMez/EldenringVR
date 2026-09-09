#pragma once
#include "vrmath.h"

namespace ervr {

// Express a solved root bone in the renderer's frame without changing its world pose.
// The native model matrix may still be interpolating toward the physics orientation.
inline void RebasePoseRoot(const Vec3& solvePosition, const Quat& solveRotation,
                           const Vec3& renderPosition, const Quat& renderRotation,
                           Vec3& translation, Quat& rotation) {
    const Quat inverseRender = renderRotation.Conj();
    translation = inverseRender.Rotate(solvePosition + solveRotation.Rotate(translation) - renderPosition);
    rotation = (inverseRender * solveRotation * rotation).Normalized();
}

} // namespace ervr
