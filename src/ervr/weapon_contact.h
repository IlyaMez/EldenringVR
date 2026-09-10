#pragma once

namespace ervr {

struct WeaponContactLifetime {
    bool active = false;
    bool first = false;
    bool endRequested = false;

    void Begin() { active = first = true; endRequested = false; }
    void End(bool cancel = false) {
        if (!cancel && active && first) endRequested = true;
        else active = first = endRequested = false;
    }
    void FinishFrame() {
        first = false;
        if (endRequested) { active = false; endRequested = false; }
    }
};

inline bool WeaponContactStarts(bool allowed, bool continuous, float tip, float hand,
                                float onSpeed, float minHandSpeed) {
    // A correctly measured tip arc is a contact even when the wrist stays in
    // place. Weight/arc still determine damage, rather than hiding the hitbox.
    return allowed && tip >= onSpeed && (continuous || hand >= minHandSpeed);
}

inline bool WeaponContactReady(bool continuous, bool renderedMotion, bool kinetic,
                               double elapsed, float armWindow, float weight,
                               float armPerWeight, bool decelerating) {
    if (continuous) return true;
    // Rendered inertia has already paid the startup cost in visible motion.
    const float delay = armWindow + (kinetic && !renderedMotion ? weight * armPerWeight : 0.f);
    return elapsed >= delay || (decelerating && !kinetic);
}

inline bool WeaponContactContinues(bool continuous, float tip, float offSpeed,
                                   double elapsed, float maxSeconds) {
    return tip >= offSpeed && (continuous || elapsed < maxSeconds);
}

} // namespace ervr
