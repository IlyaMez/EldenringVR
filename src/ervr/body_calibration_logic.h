#pragma once
// The T-pose body calibration (Spidy's, made for ERVR). The player stands tall, looks ahead, holds both arms
// straight out to the sides and both triggers for a moment; ERVR measures how high their eyes are and how long
// their arms are. Free of game state, so tools/test_body_calibration.cpp can drive it; body_calibration.cpp runs
// it in the headset and keeps the result in ERVR.ini [Body]. What the measurements change:
//   - the arms. The camera sits on the Tarnished's head and his hands on your controllers whatever size you are,
//     so what does not fit is the arm between them: a long-armed player's reach stretched his arm (ArmStretch),
//     a short-armed one's kept his elbows bent. SolveArm (game_er.cpp) now gives him your arm's length
//     (CalibArmScale), about his shoulder: your straight arm is his straight arm, and ArmStretch only adds to it
//     when a hand goes further.
//   - the belt. [Belt] Drop is for a 1.76 m player; a calibrated one gets it at their own waist (CalibBeltDrop).
// Tracking space throughout, OpenXR axes: +y up from the floor, -z ahead (the stage space ERVR's poses are in).
#include "vrmath.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ervr {

// A standing adult (Spidy's rig, the same proportions): the eyes above the floor, each shoulder joint from the
// point between the eyes along the body's forward, up and left (0 = the left), and the arm from that joint to the
// wrist, metres. A player is this times their eye height over `eye`; the pose checks measure each arm from where
// that puts their shoulder.
struct CalibBody {
    float eye = 1.6969f;
    Vec3 shoulder[2] = {{-0.1107f, -0.2662f, 0.1704f}, {-0.1107f, -0.2662f, -0.1704f}};
    float arm = 0.5586f;
};

// Standing eye height over height (Spidy's HEIGHT): what the panel and the launcher show as your height, since the
// eye height reads as a wrong one ("I'm around 1.88 m and it shows 1.7 something", Spidy, October 27).
constexpr float kCalibEyeShare = 0.936f;
inline float CalibHeight(float eye) { return eye / kCalibEyeShare; }

// What [Body] keeps, millimetres; a value outside these is no calibration.
constexpr int kCalibMinEyeMm = 1000, kCalibMaxEyeMm = 2500, kCalibMinArmMm = 250, kCalibMaxArmMm = 1200;
inline bool CalibValid(int eyeMm, int armMm) {
    return eyeMm >= kCalibMinEyeMm && eyeMm <= kCalibMaxEyeMm && armMm >= kCalibMinArmMm && armMm <= kCalibMaxArmMm;
}

// How much longer the Tarnished's arm gets (bone lengths, about the shoulder) for a player whose arm, shoulder
// joint to wrist, is `playerArm` metres, his own (upper arm + forearm) being `rigArm`. `handScale` is [FirstPerson]
// HandScale, which brings every hand that much closer to the eyes. 1 for an unknown either; kept within these,
// as Spidy keeps his.
constexpr float kCalibMinArmScale = 0.8f, kCalibMaxArmScale = 1.25f;
inline float CalibArmScale(float playerArm, float rigArm, float handScale = 1.f) {
    if (!std::isfinite(playerArm) || !(playerArm > 0.f) || !std::isfinite(rigArm) || !(rigArm > 0.05f)) return 1.f;
    const float hs = std::isfinite(handScale) && handScale > 0.1f ? handScale : 1.f;
    return Clamp(playerArm * hs / rigArm, kCalibMinArmScale, kCalibMaxArmScale);
}
// [Belt] Drop (metres below the eyes) is for a 1.76 m player: a calibrated one's scales with their height, since the
// eyes-to-waist distance does (and not with posture: it is the same seated).
constexpr float kCalibBeltHeight = 1.76f;
inline float CalibBeltDrop(float drop, float eye) {
    if (!std::isfinite(eye) || !(eye > 0.f)) return drop;
    return drop * Clamp(CalibHeight(eye) / kCalibBeltHeight, 0.7f, 1.3f);
}

// What a calibration found.
struct CalibResult {
    float eyeHeight = 0.f;   // metres above the floor, standing tall
    float armLength = 0.f;   // metres, shoulder joint to wrist: the longer (straighter) of the two
    float reach[2] = {0.f, 0.f};   // each arm's, 0 the left
};
// The headset and the controllers, one frame, tracking space.
struct CalibSample {
    bool headTracked = false;
    Vec3 head;                   // the point between the eyes
    Quat headRot;
    bool handTracked[2] = {false, false};
    Vec3 wrist[2];               // where ERVR puts the Tarnished's wrist for each controller (0 the left)
    float trigger[2] = {0.f, 0.f};
    float seconds = 0.f;         // since the previous sample
};
enum class CalibPhase : uint8_t {
    idle,      // nothing shows
    waiting,   // the instructions show; the pose does not count (yet)
    holding,   // the pose counts: the hold is filling
    done,      // measured: the result shows
};
// Why the pose does not count, the first that applies (none: it counts).
enum class CalibHint : uint8_t {
    none,
    tracking,    // the headset or a controller is not tracked
    standUp,     // the eyes are lower than a standing player's
    armsOut,     // an arm is not out to its side
    straight,    // an arm is bent, or the two reach differently far
    lookAhead,   // the head is turned away from the arms' line, or tilted
    triggers,    // a trigger is not held
    still,       // the head or a hand moved
    // The eyes give a height outside 1.20-2.20 m, or controllers too far apart for the eyes: the headset's floor is
    // off, and the measurement would keep it (Spidy's players: "2.50 meters tall", "I am 170cm but the calibration
    // measures me for 105cm").
    floor,
};
struct CalibConfig {
    float holdSeconds = 1.5f;
    float graceSeconds = 0.25f;               // a lapse this short pauses the hold; a longer one starts it over
    float trigger = 0.6f, triggerHeld = 0.35f;   // a trigger counts past `trigger`, then while past `triggerHeld`
    float lowestEyes = 1.f;                   // metres: lower is a seated or kneeling player
    float shortest = 1.2f, tallest = 2.2f;    // a standing player's height, metres
    // An arm points from its shoulder at least this cosine near its side (41 degrees) and reaches at least `bent` of
    // a typical arm at the player's size; the two reach within `mismatch` of the longer.
    float sideways = 0.75f, bent = 0.72f, mismatch = 0.12f;
    // The wrists, arms straight out, at most this many times the eyes' height apart: the floor does not move the
    // span, and arms reach no further than straight (Spidy: a 1.88 m player spans 0.94 of eyes 1.76 m up).
    float widestSpan = 1.15f;
    float headPitch = 0.45f, headYaw = 0.6f;   // radians: the head within these of level and square to the arms
    float handSpeed = 0.35f, headSpeed = 0.3f;   // m/s: faster is not holding still
};

namespace calib_detail {
inline bool Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline Vec3 Level(const Vec3& v) { return {v.x, 0.f, v.z}; }
}

class BodyCalibration {
public:
    // Shows the instructions and waits for the pose.
    void Start() { phase_ = CalibPhase::waiting; hint_ = CalibHint::tracking; ready_[0] = ready_[1] = false; triggersHeld_ = previous_ = false; Restart(); }
    // Hides them; a result stays.
    void Stop() { phase_ = CalibPhase::idle; ready_[0] = ready_[1] = false; triggersHeld_ = previous_ = false; Restart(); }
    // One frame. True on the frame the pose has been held long enough: Result() then holds the measurements, and
    // the phase stays done until Stop() or Start().
    bool Update(const CalibSample& s, const CalibConfig& c = {}, const CalibBody& b = {}) {
        using namespace calib_detail;
        if (phase_ == CalibPhase::idle || phase_ == CalibPhase::done) return false;
        const float dt = std::isfinite(s.seconds) ? Clamp(s.seconds, 0.f, 0.1f) : 0.f;
        const bool tracked = s.headTracked && s.handTracked[0] && s.handTracked[1] && Finite(s.head) && Finite(s.wrist[0]) && Finite(s.wrist[1]);
        const float eyes = s.head.y;
        CalibHint hint = CalibHint::none;
        bool ready[2] = {false, false};
        float reach[2] = {0.f, 0.f};
        const float lowest = std::min(s.trigger[0], s.trigger[1]);
        triggersHeld_ = std::isfinite(lowest) && lowest >= (triggersHeld_ ? c.triggerHeld : c.trigger);
        const Vec3 up{0.f, 1.f, 0.f};
        if (!tracked) hint = CalibHint::tracking;
        else if (!(eyes >= c.lowestEyes)) hint = CalibHint::standUp;
        else if (!(CalibHeight(eyes) >= c.shortest && CalibHeight(eyes) <= c.tallest)) hint = CalibHint::floor;
        else {
            const Vec3 look = s.headRot.Forward();
            Vec3 ahead = Level(look).Normalized();
            if (ahead.Length() < 0.5f) ahead = {0.f, 0.f, -1.f};
            // The body faces square to the line from the right wrist to the left once the arms are apart; before
            // that, the way the head does.
            const Vec3 across = Level(s.wrist[0] - s.wrist[1]);
            const Vec3 left = across.Length() >= 0.25f ? across.Normalized() : up.Cross(ahead);
            const Vec3 forward = left.Cross(up);
            const float scale = Clamp(eyes / b.eye, 0.7f, 1.3f);
            bool out[2], straight[2];
            for (int i = 0; i < 2; ++i) {
                const Vec3 sh = b.shoulder[i];
                const Vec3 shoulder = s.head + (forward * sh.x + up * sh.y + left * sh.z) * scale;
                const Vec3 arm = s.wrist[i] - shoulder;
                reach[i] = arm.Length();
                const Vec3 side = i == 0 ? left : -left;
                out[i] = reach[i] > 1e-3f && arm.Dot(side) / reach[i] >= c.sideways;
                straight[i] = reach[i] >= c.bent * scale * b.arm;
                ready[i] = out[i] && straight[i];
            }
            const float pitch = std::asin(Clamp(look.y, -1.f, 1.f));
            // Crossed arms put the left wrist on the right: the body would face back.
            if (!out[0] || !out[1] || forward.Dot(ahead) < 0.f) hint = CalibHint::armsOut;
            else if (!straight[0] || !straight[1] || std::fabs(reach[0] - reach[1]) > c.mismatch * std::max(reach[0], reach[1]))
                hint = CalibHint::straight;
            else if ((s.wrist[0] - s.wrist[1]).Length() > c.widestSpan * eyes) hint = CalibHint::floor;
            else if (std::fabs(pitch) > c.headPitch || forward.Dot(ahead) < std::cos(c.headYaw)) hint = CalibHint::lookAhead;
            else if (!triggersHeld_) hint = CalibHint::triggers;
            else if (previous_ && dt > 0.f &&
                     ((s.head - lastHead_).Length() > c.headSpeed * dt || (s.wrist[0] - lastWrist_[0]).Length() > c.handSpeed * dt ||
                      (s.wrist[1] - lastWrist_[1]).Length() > c.handSpeed * dt))
                hint = CalibHint::still;
        }
        previous_ = tracked;
        if (tracked) { lastHead_ = s.head; lastWrist_[0] = s.wrist[0]; lastWrist_[1] = s.wrist[1]; }
        hint_ = hint;
        ready_[0] = ready[0]; ready_[1] = ready[1];
        if (hint == CalibHint::none) {
            phase_ = CalibPhase::holding;
            lapse_ = 0.f;
            held_ += dt;
            ++frames_;
            eyeSum_ += eyes;
            for (int i = 0; i < 2; ++i) reachSum_[i] += reach[i];
            progress_ = Clamp(held_ / std::max(c.holdSeconds, 0.01f), 0.f, 1.f);
            // (Frame times summed in floats fall a hair short of the whole.)
            if (held_ + 1e-4f >= c.holdSeconds && frames_ > 0) {
                result_.eyeHeight = (float)(eyeSum_ / frames_);
                for (int i = 0; i < 2; ++i) result_.reach[i] = (float)(reachSum_[i] / frames_);
                result_.armLength = std::max(result_.reach[0], result_.reach[1]);   // the straighter arm reaches further
                phase_ = CalibPhase::done;
                progress_ = 1.f;
                return true;
            }
        } else if (phase_ == CalibPhase::holding && (lapse_ += dt) > c.graceSeconds) {
            phase_ = CalibPhase::waiting;
            Restart();
        }
        return false;
    }
    CalibPhase Phase() const { return phase_; }
    CalibHint Hint() const { return hint_; }
    float Progress() const { return progress_; }   // how far the hold is, 0 to 1
    bool ArmReady(int i) const { return i >= 0 && i < 2 && ready_[i]; }   // that arm is out to its side and straight
    const CalibResult& Result() const { return result_; }

private:
    void Restart() { held_ = lapse_ = progress_ = 0.f; frames_ = 0; eyeSum_ = 0.0; reachSum_[0] = reachSum_[1] = 0.0; }
    CalibPhase phase_ = CalibPhase::idle;
    CalibHint hint_ = CalibHint::tracking;
    float held_ = 0.f, lapse_ = 0.f, progress_ = 0.f;
    bool ready_[2] = {false, false};
    bool triggersHeld_ = false;
    int frames_ = 0;
    double eyeSum_ = 0.0, reachSum_[2] = {0.0, 0.0};
    bool previous_ = false;   // the previous sample was tracked: the stillness check has something to compare
    Vec3 lastHead_, lastWrist_[2];
    CalibResult result_;
};

// The panel's place for a head at the start, tracking space: 1.4 m ahead of it and 12 cm below the eyes, upright,
// its image facing the player (+z toward them, +x to their right).
inline Pose CalibPanelPose(const Vec3& head, const Quat& headRot) {
    Vec3 ahead = calib_detail::Level(headRot.Forward()).Normalized();
    if (ahead.Length() < 0.5f) ahead = {0.f, 0.f, -1.f};
    const Vec3 up{0.f, 1.f, 0.f}, toward = -ahead, right = up.Cross(toward);
    Pose p;
    p.pos = head + ahead * 1.4f + Vec3{0.f, -0.12f, 0.f};
    p.rot = Quat::FromBasis(right, up, toward);
    p.valid = true;
    return p;
}
constexpr float kCalibPanelWidth = 1.2f, kCalibPanelHeight = 0.6f;   // metres

// The panel's line for a hint while the pose is not held.
inline const char* CalibHintText(CalibHint h) {
    switch (h) {
    case CalibHint::tracking:  return "KEEP BOTH CONTROLLERS IN VIEW";
    case CalibHint::standUp:   return "PLEASE STAND UP";
    case CalibHint::armsOut:   return "STRETCH YOUR ARMS OUT TO THE SIDES";
    case CalibHint::straight:  return "STRAIGHTEN BOTH ARMS";
    case CalibHint::lookAhead: return "LOOK STRAIGHT AHEAD";
    case CalibHint::triggers:  return "HOLD BOTH TRIGGERS";
    case CalibHint::floor:     return "CHECK YOUR HEADSET'S FLOOR";
    default:                   return "HOLD STILL";
    }
}
// For the log.
inline const char* CalibHintName(CalibHint h) {
    switch (h) {
    case CalibHint::none:      return "none";
    case CalibHint::tracking:  return "tracking";
    case CalibHint::standUp:   return "stand up";
    case CalibHint::armsOut:   return "arms out";
    case CalibHint::straight:  return "straight";
    case CalibHint::lookAhead: return "look ahead";
    case CalibHint::triggers:  return "triggers";
    case CalibHint::still:     return "still";
    case CalibHint::floor:     return "floor";
    }
    return "?";
}

} // namespace ervr
