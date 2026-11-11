#pragma once
// Two moves physical combat used to make impossible, decided here without game memory so they can be tested.
//
// 1. The skill trigger. A skill the game holds while its button is held (a stance, a charged skill) needs the
//    button DOWN for as long as the player holds it; the left grip could only tap it (a held grip opens the weapon
//    wheel). The trigger that is the two-handed skill button now holds the game's skill button one-handed too, for
//    as long as it is held. Two-handed with TwoHandSkill=effect, a tap still replays the skill's effect and a hold
//    past SkillHoldSeconds is the game's own skill, held.
//
// 2. Criticals. ELDEN RING's backstabs and ripostes are its "throw" system: on an R1 press the engine looks up
//    ThrowParam rows for the pair (attacker character id, defender character id) and starts the paired animation
//    when one row's distance, height and two angle windows hold. ERVR's swings inject an attack behavior and never
//    press R1, so no throw was ever looked up. ThrowRowMatches is that lookup's geometry (Paramdex THROW_PARAM_ST):
//      Dist            - closer than this [m]
//      upper/lowerYRange - the defender's height relative to the attacker stays inside (-lower, +upper) [m]
//      DiffAngMin/Max  - the angle between the attacker's facing and the defender's facing [deg, -180..180]
//      diffAngMyToDef  - the angle between the attacker's facing and the direction to the defender [deg]
//    plus a second set (*_start) the game uses when a backstab starts. A window whose min is above its max wraps
//    through 180 deg (90..-90 = face to face). The row's throwType says what it is: in the game's data (logged by
//    ERVR at load, 2026-11-11) type 1 rows are the backstabs - facings -90..90 plus a start window (+-45 deg, 1.5 m)
//    - while types 0 (any facing), 20 (face to face) and 24 (side sectors) are the criticals that need the enemy
//    parried or stance-broken first.
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ervr {

// ---- skill trigger ---------------------------------------------------------------------------------------------
struct SkillTriggerIn {
    bool down = false;          // the VR control is held
    bool twoHanded = false;     // the game's arm style is two-handed
    bool oneHandAllowed = false;// one-handed: the control's hand holds a weapon whose skill the button may hold
    bool effectMode = false;    // TwoHandSkill=effect (a tap replays the effect)
    bool gameMode = false;      // TwoHandSkill=game (the game's skill, from the press)
    double now = 0.0;
    float holdSeconds = 0.3f;   // effect mode: held this long, the press is the game's own skill, held
};
struct SkillTriggerOut {
    bool hold = false;          // ERVR holds the game's skill button this tick
    int request = 0;            // 0 none, 1 = replay the effect (a tap), 3 = the held game skill started (record it)
    bool started = false;       // the hold began this tick (for the log)
};

class SkillTrigger {
public:
    SkillTriggerOut Step(const SkillTriggerIn& in) {
        SkillTriggerOut o;
        if (waitRelease_) {   // after a stand-down, a trigger still held is not a new press
            if (!in.down) waitRelease_ = false;
            return o;
        }
        if (in.down && !down_) {
            down_ = true; downAt_ = in.now; held_ = false; tap_ = false;
            two_ = in.twoHanded;
            if (!two_ && in.oneHandAllowed) { held_ = true; o.started = true; }
            else if (two_ && in.gameMode) { held_ = true; o.started = true; }
            else if (two_ && in.effectMode) tap_ = true;   // a tap until it is held long enough
        } else if (in.down && down_) {
            if (tap_ && in.now - downAt_ >= in.holdSeconds) { tap_ = false; held_ = true; o.started = true; o.request = 3; }
        } else if (!in.down && down_) {
            down_ = false;
            if (tap_) o.request = 1;
            tap_ = false; held_ = false;
        }
        o.hold = held_;
        return o;
    }
    // Combat stood down (menu, transition, riding): let go now; the next press - after the trigger has been let
    // go - starts fresh. A no-op when nothing is pressed.
    void Reset() {
        if (down_) waitRelease_ = true;
        down_ = held_ = tap_ = two_ = false;
    }
    bool Holding() const { return held_; }

private:
    bool down_ = false, held_ = false, tap_ = false, two_ = false, waitRelease_ = false;
    double downAt_ = 0.0;
};

// ---- stance follow-ups ------------------------------------------------------------------------------------------
// While the skill trigger holds a stance, a swing of the attacking hand presses the game's light attack (heavy for a
// hard swing): the stance's follow-up. The game's HKS returns to the stance after each follow-up for as long as the
// skill button stays held (DrawStanceRightAttackLight -> DrawStanceRightLoop while ActionDuration(L2) > 0), so one
// hold can follow up several times. A press is a button, not a blade contact, so it needs a deliberate swing:
//   - the gated swing's thresholds - onSpeed at the tip AND minHand at the controller - never the live contact's
//     ContactOnSpeed (0.8 m/s: the first build pressed R1 on any small motion of the held blade, 2026-11-11 test:
//     "very sensitive to movement and triggered attack immediately");
//   - the blade comes to rest (under offSpeed) between presses, and gapSeconds pass: a follow-through or the return
//     to guard is no second press (an R1 that lands in the follow-up's cancel window is a plain attack);
//   - the first press waits settleSeconds after the hold began, and a blade already moving when the trigger was
//     pulled must come to rest first: the game has to be in the stance before the R1 arrives.
// Light vs heavy: the tip's peak over armWindow against heavySpeed.
//
// Where the swing aims (SkillAim=swing): the middle of the arc the blade's tip sweeps around you - the tip's
// horizontal offset from the head (s*), averaged over the whole swing and weighted by the tip's speed, from where it
// rose past offSpeed (a short decaying pre-roll kept while Ready) through the press until it slows under offSpeed
// again (also after the trigger is let go: a quick L2-R1-release still finishes its swing). Symmetric thresholds,
// so a forehand is not pulled toward its end; the tip's position, not the blade's direction, so an upright blade's
// sideways tilt does not decide it (2026-11-11 test 2 averaged the blade direction: blades 40-77 deg up, aims
// scattered 3-109 deg, 16 of 18 to the left). A slash across an enemy in front centres on it; a slash off to the left
// centres left. The stance's projectile flies that way.
//
// Wind-ups and returns are no follow-up (test 3: the presses that flew 41-174 deg off were slow swings centred to the
// right - the sword drawn back before a forehand). A strike moves the tip toward the front of your view or keeps it
// there; a wind-up carries it away. At the press decision, a tip that has moved more than kAwayDeg further from the
// view (v*, its horizontal forward) since the swing started, and is already outside kFrontDeg, does not press: the
// swing keeps being watched, its early part fading (decaySeconds), and presses once it turns into a strike without
// slowing down. No view (v* zero): no check.
enum class StancePress { None = 0, Light = 1, Heavy = 2 };
struct StanceSwingIn {
    double now = 0.0;
    double heldFor = 0.0;       // s since the skill hold began
    float tip = 0.f;            // m/s weapon-tip speed
    float hand = 0.f;           // m/s controller speed
    float sx = 0.f, sy = 0.f, sz = 0.f;   // the aim sample: the tip's offset from the head (any fixed frame)
    float onSpeed = 3.f, offSpeed = 1.5f, minHand = 0.8f, heavySpeed = 6.f;
    float armWindow = 0.06f;
    float settleSeconds = 0.15f, gapSeconds = 0.5f;
    float vx = 0.f, vz = 0.f;   // the view's horizontal forward (the sample's frame); zero = no wind-up check
    float decaySeconds = 0.3f;  // the pre-roll's (and a refused wind-up's) fade
};
constexpr float kAwayDeg = 15.f, kFrontDeg = 30.f;
// The angle between a horizontal sample and the view's forward, degrees (0..180); -1 without either.
inline float AngleFromView(float x, float z, float vx, float vz) {
    const float a = std::sqrt(x * x + z * z), v = std::sqrt(vx * vx + vz * vz);
    if (!(a > 1e-4f) || !(v > 1e-4f)) return -1.f;
    float c = (x * vx + z * vz) / (a * v);
    if (c > 1.f) c = 1.f;
    if (c < -1.f) c = -1.f;
    return std::acos(c) * 57.2957795f;
}
class StanceSwing {
public:
    StancePress Step(const StanceSwingIn& in) {
        switch (st_) {
        case Rest:
            Follow(in);
            if (in.tip < in.offSpeed && in.now - lastPress_ >= in.gapSeconds) { st_ = Ready; accX_ = accY_ = accZ_ = 0.f; }
            break;
        case Ready:
            // The pre-roll: the swing's rise from offSpeed, before it is fast enough to count, fading.
            if (in.tip >= in.offSpeed) { Fade(in); Add(in); }
            else accX_ = accY_ = accZ_ = 0.f;
            if (in.heldFor >= in.settleSeconds && in.tip >= in.onSpeed && in.hand >= in.minHand) {
                st_ = Rising; t_ = in.now; peak_ = in.tip; refused_ = false;
                startAng_ = AngleFromView(in.sx, in.sz, in.vx, in.vz);
            }
            break;
        case Rising:
            if (in.tip > peak_) peak_ = in.tip;
            if (in.tip < in.offSpeed) {   // a twitch over before the arm window, or a refused wind-up that came to rest
                if (refused_) { ignored_ = true; ignoredAng_ = lastAng_; }
                st_ = Ready; accX_ = accY_ = accZ_ = 0.f; break;
            }
            if (refused_) Fade(in);
            Add(in);
            if (in.now - t_ >= in.armWindow) {
                const float ang = AngleFromView(in.sx, in.sz, in.vx, in.vz);
                lastAng_ = ang;
                if (ang >= 0.f && startAng_ >= 0.f && ang > startAng_ + kAwayDeg && ang > kFrontDeg) {
                    refused_ = true; t_ = in.now; startAng_ = ang; peak_ = in.tip;   // moving away: watch the next stretch of it
                    break;
                }
                st_ = Rest; lastPress_ = in.now; pressPeak_ = peak_;
                aimX_ = accX_; aimY_ = accY_; aimZ_ = accZ_; aimAt_ = in.now; collecting_ = true;
                lastT_ = in.now;
                return peak_ >= in.heavySpeed ? StancePress::Heavy : StancePress::Light;
            }
            break;
        }
        lastT_ = in.now;
        return StancePress::None;
    }
    // The pressed swing's aim keeps collecting until the swing slows, held or not (Step calls it in Rest).
    void Follow(const StanceSwingIn& in) {
        if (!collecting_) return;
        // Slowed, or a second has passed (a hand that stopped being tracked mid-swing must not fold a later motion in).
        if (in.tip < in.offSpeed || in.now - aimAt_ > 1.0) { collecting_ = false; finished_ = true; return; }
        Add(in); aimX_ = accX_; aimY_ = accY_; aimZ_ = accZ_;
    }
    // A new hold (or none): the blade must come to rest before the first follow-up. The last swing's aim survives
    // (its projectile can leave after the trigger is let go).
    void Reset() { st_ = Rest; lastPress_ = -1e9; }
    // True once, when a swing refused as a wind-up / return came to rest without pressing; *ang = how far from the
    // view its tip was when last checked. For the log.
    bool TakeIgnored(float* ang) { const bool i = ignored_; ignored_ = false; if (ang) *ang = ignoredAng_; return i; }
    float PressPeak() const { return pressPeak_; }
    bool Collecting() const { return collecting_; }
    // True once, when the pressed swing has slowed (its aim is final): for the log.
    bool TakeFinished() { const bool f = finished_; finished_ = false; return f; }
    // The last pressed swing's aim (unit), and when it was pressed. False before any press or for a zero mean.
    bool Aim(float* x, float* y, float* z, double* at) const {
        const float l = std::sqrt(aimX_ * aimX_ + aimY_ * aimY_ + aimZ_ * aimZ_);
        if (!(l > 1e-4f)) return false;
        *x = aimX_ / l; *y = aimY_ / l; *z = aimZ_ / l; if (at) *at = aimAt_;
        return true;
    }

private:
    void Add(const StanceSwingIn& in) { accX_ += in.sx * in.tip; accY_ += in.sy * in.tip; accZ_ += in.sz * in.tip; }
    void Fade(const StanceSwingIn& in) {
        const float dt = (float)(in.now - lastT_);
        const float keep = dt > 0.f && in.decaySeconds > 0.f ? std::exp(-dt / in.decaySeconds) : 1.f;
        accX_ *= keep; accY_ *= keep; accZ_ *= keep;
    }
    enum { Rest, Ready, Rising } st_ = Rest;
    double t_ = 0.0, lastPress_ = -1e9, aimAt_ = -1e9, lastT_ = 0.0;
    float peak_ = 0.f, pressPeak_ = 0.f, startAng_ = -1.f, lastAng_ = -1.f, ignoredAng_ = 0.f;
    bool refused_ = false, ignored_ = false;
    float accX_ = 0.f, accY_ = 0.f, accZ_ = 0.f, aimX_ = 0.f, aimY_ = 0.f, aimZ_ = 0.f;
    bool collecting_ = false, finished_ = false;
};

// A horizontal direction turned toward the view until it is within maxDeg of it (the length kept). Any frame where
// yaw = atan2(x, z) is a rotation about the vertical. No view or maxDeg <= 0: unchanged. Returns true if it turned.
inline bool ClampTowardView(float* x, float* z, float vx, float vz, float maxDeg) {
    const float a = std::sqrt(*x * *x + *z * *z), v = std::sqrt(vx * vx + vz * vz);
    if (!(a > 1e-4f) || !(v > 1e-4f) || !(maxDeg > 0.f)) return false;
    const float va = std::atan2(vx, vz);
    float d = std::atan2(*x, *z) - va;
    while (d > 3.14159265f) d -= 6.2831853f;
    while (d < -3.14159265f) d += 6.2831853f;
    const float m = maxDeg * 0.0174532925f;
    if (d <= m && d >= -m) return false;
    d = d > 0.f ? m : -m;
    *x = a * std::sin(va + d); *z = a * std::cos(va + d);
    return true;
}

// The direction a swing-aimed projectile flies, in the game's world frame (+Y up, yaw = atan2(x, z)): the swing's
// horizontal direction with the view's pitch - a chop ends pointing down and a rising slash up, but the target is
// where you look, so the height comes from the head (clamped to +-maxPitch). A swing with almost no horizontal
// direction (straight down or up) aims where you look. Returns the yaw used.
inline float SwingAimDirection(float sx, float sy, float sz, float headYaw, float headPitch, float maxPitch,
                               float* dx, float* dy, float* dz) {
    const float h = std::sqrt(sx * sx + sz * sz);
    const float len = std::sqrt(sx * sx + sy * sy + sz * sz);
    const float yaw = (len > 1e-4f && h > 0.25f * len) ? std::atan2(sx, sz) : headYaw;
    float p = headPitch;
    if (p > maxPitch) p = maxPitch;
    if (p < -maxPitch) p = -maxPitch;
    *dx = std::cos(p) * std::sin(yaw); *dy = std::sin(p); *dz = std::cos(p) * std::cos(yaw);
    return yaw;
}

// The hand whose weapon's skill the skill button plays - the game's HKS GetSwordArtInfo: two-handed, the two-handed
// weapon's; one-handed, the left weapon's only for a shield's or a bow's skill (IsShieldArts / IsArrowStanceArts),
// else the right weapon's (a staff or seal in the left carries no skill).
inline int SkillHand(bool twoHanded, int twoHandedHand, bool leftShieldOrBow) {
    if (twoHanded) return twoHandedHand;
    return leftShieldOrBow ? 0 : 1;   // HandLeft = 0, HandRight = 1
}

// ---- criticals (ThrowParam) ------------------------------------------------------------------------------------
// THROW_PARAM_ST, 0x80 bytes (Paramdex ER Defs/ThrowParam.xml; fromsoftware-rs generated.rs field order).
#pragma pack(push, 1)
struct ThrowParamRow {
    int32_t atkChrId, defChrId;
    float dist, diffAngMin, diffAngMax, upperYRange, lowerYRange, diffAngMyToDef;
    int32_t throwTypeId, atkAnimId, defAnimId;
    uint16_t escHp, selfEscCycleTime, sphereCastRadiusRateTop, sphereCastRadiusRateLow;
    uint8_t padType, atkEnableState, throwFollowingType, pad2;
    uint8_t throwType, selfEscCycleCnt, dmyHasChrDirType, bits3b;
    int16_t atkSorbDmyId, defSorbDmyId;
    float distStart, diffAngMinStart, diffAngMaxStart, upperYRangeStart, lowerYRangeStart, diffAngMyToDefStart;
    int32_t judgeRangeBasePosDmyId1, judgeRangeBasePosDmyId2;
    float adsrobModelPosInterpolationTime, throwFollowingEndEasingTime;
    uint8_t pad1[24];
};
#pragma pack(pop)
static_assert(sizeof(ThrowParamRow) == 0x80, "THROW_PARAM_ST layout");

enum class CritKind { None = 0, Backstab = 1, Riposte = 2 };   // Riposte = any throw that needs the enemy opened up first
constexpr uint8_t kThrowTypeBackstab = 1;

inline float WrapDeg(float a) {
    while (a > 180.f) a -= 360.f;
    while (a < -180.f) a += 360.f;
    return a;
}

// Where the two characters stand, in the game's world frame. Yaw: forward = (sin yaw, 0, cos yaw), radians.
struct ThrowGeom {
    float ax = 0, ay = 0, az = 0, aYaw = 0;   // attacker (the player)
    float dx = 0, dy = 0, dz = 0, dYaw = 0;   // defender
};
struct ThrowMeasure {
    float dist = 0, dyUp = 0, diffAng = 0, myToDef = 0;   // dyUp = defender height - attacker height
};

inline ThrowMeasure MeasureThrow(const ThrowGeom& g) {
    ThrowMeasure m;
    const float vx = g.dx - g.ax, vz = g.dz - g.az;
    m.dist = std::sqrt(vx * vx + vz * vz);
    m.dyUp = g.dy - g.ay;
    m.diffAng = WrapDeg((g.aYaw - g.dYaw) * 57.2957795f);
    if (m.dist > 1e-4f) m.myToDef = std::fabs(WrapDeg((std::atan2(vx, vz) - g.aYaw) * 57.2957795f));
    return m;
}

// One of the row's two windows (start = the *_start set).
inline bool ThrowWindowHolds(const ThrowParamRow& r, const ThrowMeasure& m, bool start) {
    const float dist = start ? r.distStart : r.dist;
    const float up = start ? r.upperYRangeStart : r.upperYRange, low = start ? r.lowerYRangeStart : r.lowerYRange;
    const float amin = start ? r.diffAngMinStart : r.diffAngMin, amax = start ? r.diffAngMaxStart : r.diffAngMax;
    const float myToDef = start ? r.diffAngMyToDefStart : r.diffAngMyToDef;
    if (!(dist > 0.f) || m.dist >= dist) return false;
    if (m.dyUp >= up || -m.dyUp >= low) return false;
    if (amax > amin) { if (m.diffAng <= amin || m.diffAng >= amax) return false; }
    else if (amin > amax) { if (m.diffAng < amin && m.diffAng > amax) return false; }   // wraps through 180
    else return false;                                                                  // an empty window
    return m.myToDef <= myToDef;
}

// 1 = the row's own window holds, 2 = its backstab-start window holds, 0 = neither.
inline int ThrowRowMatches(const ThrowParamRow& r, const ThrowMeasure& m) {
    if (ThrowWindowHolds(r, m, false)) return 1;
    if (ThrowWindowHolds(r, m, true)) return 2;
    return 0;
}

// What the row is, by the game's own throw type (see the top of this file).
inline CritKind ThrowRowKind(const ThrowParamRow& r) {
    return r.throwType == kThrowTypeBackstab ? CritKind::Backstab : CritKind::Riposte;
}

// A backstab ERVR may press for: the row's backstab-start window when it has one (the narrower of the two, so a
// prediction errs toward a plain strike, never toward a refused press), else its own window.
inline bool BackstabHolds(const ThrowParamRow& r, const ThrowMeasure& m) {
    if (ThrowRowKind(r) != CritKind::Backstab) return false;
    return r.distStart > 0.f ? ThrowWindowHolds(r, m, true) : ThrowWindowHolds(r, m, false);
}

inline const char* CritKindName(CritKind k) {
    return k == CritKind::Backstab ? "backstab" : k == CritKind::Riposte ? "riposte" : "none";
}

}
