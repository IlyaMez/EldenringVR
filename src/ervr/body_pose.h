#pragma once
#include "vrmath.h"
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

// Height of `bone` above the rig root in the model frame (Y up), for a parent-relative local pose. Walks only
// the bone's parent chain (root bones: parent < 0 or not before them), so it is cheap per frame.
template<class Transform>
float BoneHeight(std::span<const Transform> pose, std::span<const int16_t> parents, int bone) {
    int chain[64]; int n = 0;
    for (int i = bone; i >= 0 && i < (int)pose.size() && i < (int)parents.size() && n < 64;
         i = parents[i] < i ? parents[i] : -1) chain[n++] = i;
    Vec3 p{}; Quat q{};
    for (int k = n - 1; k >= 0; --k) {
        const Transform& t = pose[chain[k]];
        p = p + q.Rotate(Vec3{t.t[0], t.t[1], t.t[2]});
        q = (q * Quat{t.q[0], t.q[1], t.q[2], t.q[3]}).Normalized();
    }
    return p.y;
}

// The VR body is a captured template and the camera rides its head bone, so a template is judged by that head's
// height against the skeleton's own reference pose before it may become the body. Standing and running read
// 0.9-1.0 of the reference, a crouch or kneel about 0.65-0.75, a body on the ground well under half.
enum class BodyStance : uint8_t { Standing, Low, Down };
constexpr float kStanceStandingRatio = 0.82f;
constexpr float kStanceDownRatio = 0.45f;
inline bool StanceReferencePlausible(float head) { return head >= 1.0f && head <= 2.4f; }
inline BodyStance JudgeStance(float head, float reference) {
    if (!StanceReferencePlausible(reference)) return BodyStance::Standing;   // no usable reference: judge nothing
    const float r = head / reference;
    return r >= kStanceStandingRatio ? BodyStance::Standing : r >= kStanceDownRatio ? BodyStance::Low : BodyStance::Down;
}

// Which captured poses may become the template. A pose on the ground (knockdown, the start of the respawn kneel,
// a roll) never does. A first template (skeleton build, the post-load retry) must be standing, since nothing
// else gets the body out of a low one - unless nothing standing has shown up for a long while (`overdue`), so a
// reference pose that reads wrong cannot keep VR posing off for good. A stance re-capture (held still and
// settled) may be low: that is how an in-game crouch reaches the VR body.
enum class TemplateVerdict : uint8_t { Standing, Low, Rejected };
inline TemplateVerdict JudgeTemplate(BodyStance s, bool firstTemplate, bool overdue) {
    if (overdue) return s == BodyStance::Standing ? TemplateVerdict::Standing : TemplateVerdict::Low;
    if (s == BodyStance::Down) return TemplateVerdict::Rejected;
    if (s == BodyStance::Low) return firstTemplate ? TemplateVerdict::Rejected : TemplateVerdict::Low;
    return TemplateVerdict::Standing;
}

// Leaving a low template: the game's own pose has to read standing on fresh frames for `hold` seconds, so one
// frame of a getting-up animation is not enough. Echo frames (our own write read back) neither advance nor reset it.
struct StandUpWatch {
    double since = -1.0;
    bool Update(bool fresh, bool standing, double now, double hold = 0.25) {
        if (!fresh) return false;
        if (!standing) { since = -1.0; return false; }
        if (since < 0.0) since = now;
        return now - since >= hold;
    }
    void Reset() { since = -1.0; }
};

} // namespace ervr
