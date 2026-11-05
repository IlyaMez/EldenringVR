#pragma once
#include "vrmath.h"

namespace ervr {

// Closest distance between segments p1-q1 and p2-q2 (Ericson, Real-Time Collision Detection 5.1.9): how near a
// blade came to a character's body line.
inline float SegmentDistance(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2) {
    const Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = d1.Dot(d1), e = d2.Dot(d2), f = d2.Dot(r);
    float s = 0.f, t = 0.f;
    if (a <= 1e-8f && e <= 1e-8f) return r.Length();
    if (a <= 1e-8f) t = Clamp(f / e, 0.f, 1.f);
    else {
        const float c = d1.Dot(r);
        if (e <= 1e-8f) s = Clamp(-c / a, 0.f, 1.f);
        else {
            const float b = d1.Dot(d2), denom = a * e - b * b;
            s = denom > 1e-8f ? Clamp((b * f - c * e) / denom, 0.f, 1.f) : 0.f;
            t = (b * s + f) / e;
            if (t < 0.f) { t = 0.f; s = Clamp(-c / a, 0.f, 1.f); }
            else if (t > 1.f) { t = 1.f; s = Clamp((b - c) / a, 0.f, 1.f); }
        }
    }
    return ((p1 + d1 * s) - (p2 + d2 * t)).Length();
}

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

// A live (unspent) continuous contact is one swing too. It used to last as long as the tip moved at the off
// speed (0.25 m/s), with no time limit: a blade carried while walking, rested on a wall or waved about stayed a
// live hitbox for whole seconds (0.7.0 player log: a strike live 296-300 of every 300 frames, "swings 0,
// injected 271") - the "constant attacking". It now ends once the tip has stayed below the swing's valley
// (ContactValley x its peak, at least the off speed) for dwellSeconds - the follow-through slowed - or after
// maxSeconds however the blade moves. Returns 0 (keep), 3 (slowed), 4 (too long).
inline int LiveContactEnd(bool continuous, bool spent, double belowValleyFor, float dwellSeconds, double elapsed,
                          float maxSeconds) {
    if (!continuous || spent) return 0;
    if (belowValleyFor >= dwellSeconds) return 3;
    if (maxSeconds > 0.f && elapsed >= maxSeconds) return 4;
    return 0;
}

// After a contact that landed a hit the next one opens only `cooldown` seconds after it ended: one hit per
// swing, nothing sawn out of the same motion. A contact that missed re-arms at once, so a windup that slows at
// the top still opens for its downswing.
inline bool ContactCooldownDone(bool landed, double sinceEnd, float cooldown) {
    return !landed || sinceEnd >= cooldown;
}

// The one-frame stamina drop that is ERVR's own on-hit mark (stamina "off": the strike's behavior cost is forced
// to `mark` and refunded). A frame's regen can shave up to 2 off it; a roll, a jump or a blocked hit costs
// something else, and that cost is the player's - it used to be read as a hit and refunded (free rolls while a
// strike was live).
inline bool StaminaMarkSized(int drop, int mark) {
    return mark <= 1 ? drop >= 1 : drop >= mark - 2 && drop <= mark;
}

} // namespace ervr
