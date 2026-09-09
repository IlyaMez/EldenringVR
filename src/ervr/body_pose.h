#pragma once
#include <span>
#include <string_view>
#include <cstdint>

namespace ervr {

inline bool IsFingerBone(std::string_view name) {
    return name.starts_with("L_Finger") || name.starts_with("R_Finger");
}

// Freeze the body, but retain the current animation's finger joints. Equipment
// changes and one-/two-hand transitions must not wait for a stance recapture.
template<class Transform>
void ApplyBodyPoseTemplate(std::span<Transform> live, std::span<const Transform> stance,
                           std::span<const uint8_t> fingers) {
    for (size_t i = 0; i < live.size(); ++i)
        if (!fingers[i]) live[i] = stance[i];
}

} // namespace ervr
