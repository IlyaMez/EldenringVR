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

// A spent contact (damage budget gone, the blade inert) ends at the tip's next speed valley or a
// while after it was spent, not only at rest: a long weapon's tip seldom slows to the off speed
// mid-fight, and the next swing must open a new contact. Returns 0 (keep), 1 (valley), 2 (timeout).
inline int SpentContactEnd(bool spent, float tip, float peak, float offSpeed, float valleyFrac,
                           double sinceSpent, float spentSeconds) {
    if (!spent) return 0;
    const float valley = valleyFrac * peak > offSpeed ? valleyFrac * peak : offSpeed;
    if (tip < valley) return 1;
    return sinceSpent >= spentSeconds ? 2 : 0;
}

// After a spent contact ended at a valley, the next contact needs the tip to speed up again from the
// lowest speed it reached since (rearmMin, 0 = no condition): the follow-through of the swing that
// just hit only slows down, so it can never open a second hit.
inline bool RearmReady(float tip, float rearmMin) {
    return rearmMin <= 0.f || tip >= rearmMin * 1.5f + 0.5f;
}

} // namespace ervr
