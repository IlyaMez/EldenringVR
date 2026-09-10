#pragma once
#include <span>
#include <string_view>
#include <cstdint>

namespace ervr {

// Only controls saving a new stance. Rendering always keeps the existing VR
// template, including while menus are open and during their exit animations.
// Do not depend on movement-cancel flags: they can be closed at skeleton build
// and throughout idle, which previously prevented ANY template being captured.
struct BodyPoseCaptureGuard {
    static constexpr double MenuExitSeconds = 3.0;
    uintptr_t player = 0;
    double resumeAt = 0;
    bool captureAllowed = false;

    void Update(uintptr_t owner, bool menu, double now, bool simulationPass) {
        if (!simulationPass) return; // both eyes use the simulation's decision
        if (owner != player) { player = owner; resumeAt = 0; }
        if (menu) resumeAt = now + MenuExitSeconds;
        captureAllowed = owner && !menu && now >= resumeAt;
    }
    bool CanCapture(uintptr_t owner) const { return owner && owner == player && captureAllowed; }
};

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
