#include "body_haptics.h"
#include <algorithm>
#include <cmath>
#include <iterator>

namespace ervr {
namespace {
constexpr float kPi = 3.14159265f;
constexpr size_t kMostPlaying = 48;   // patterns playing at once; past it the oldest goes

float Clamp01(float v) { return std::isfinite(v) ? std::clamp(v, 0.f, 1.f) : 0.f; }
// Two patterns on one motor add up without passing full strength.
void Mix(float& motor, float v) { motor = 1 - (1 - motor) * (1 - Clamp01(v)); }
// A pulse: up over `attack`, held for `hold`, down over `release` (seconds); 0 outside it. The devices
// get a frame every 40 ms, so a part shorter than that may fall between two: the patterns hold at
// least that long.
float Envelope(float t, float attack, float hold, float release) {
    if (!(t >= 0)) return 0;
    if (t < attack) return t / attack;
    t -= attack;
    if (t < hold) return 1;
    t -= hold;
    return t < release ? 1 - t / release : 0;
}
// A motor's random number for a step of time; 0..1.
float Noise(unsigned motor, int64_t step) {
    uint32_t x = (uint32_t)step * 0x9e3779b1u ^ (motor + 1) * 0x85ebca77u;
    x ^= x >> 15; x *= 0x2c1b3c6du; x ^= x >> 12; x *= 0x297a2d39u; x ^= x >> 15;
    return (float)(x & 0xffff) / 65535.f;
}
// A flutter: each motor its own, changing twenty times a second; 0..1.
float Flutter(unsigned motor, double now) { return Noise(motor, (int64_t)std::floor(now * 20)); }
// A swell: each motor its own, gliding from one value to the next four times a second; 0..1. The
// wind's: a flutter on every motor at once was a buzz (Spidy, 2026-10-20).
float Swell(unsigned motor, double now) {
    const double x = now * 4, step = std::floor(x);
    const float f = (float)(x - step), s = f * f * (3 - 2 * f);
    const auto i = (int64_t)step;
    return Noise(motor + 64, i) * (1 - s) + Noise(motor + 64, i + 1) * s;
}
float Wrap(float a) {
    a = std::fmod(a + kPi, 2 * kPi);
    return (a < 0 ? a + 2 * kPi : a) - kPi;
}
// The vest motor's side, 0 the wearer's left to 3 the right, and its row.
unsigned SideOf(unsigned motor) {
    const unsigned column = motor % 4;
    return (motor >= 16 ? kBodyBackMirrored : kBodyFrontMirrored) ? 3 - column : column;
}
unsigned RowOf(unsigned motor) { return motor % 16 / 4; }

// Draws patterns into a frame, every motor mixed.
struct Paint {
    BodyFrame& f;
    void Vest(unsigned motor, float v) { Mix(f.At(BodyDevice::Vest, motor), v); }
    void Vest(bool back, unsigned row, unsigned side, float v) { Vest(BodyVestMotor(back, row, side), v); }
    // Every motor of rows `top` to `bottom`, the front's at `front` and the back's at `back`.
    void Rows(unsigned top, unsigned bottom, float front, float back) {
        for (unsigned row = top; row <= bottom; ++row)
            for (unsigned side = 0; side < 4; ++side) { Vest(false, row, side, front); Vest(true, row, side, back); }
    }
    void VestAll(float v) { Rows(0, 3, v, v); }
    // The middle of the chest, and of the back.
    void Chest(float v) { for (unsigned row = 1; row <= 2; ++row) for (unsigned side = 1; side <= 2; ++side) Vest(false, row, side, v); }
    void MidBack(float v) { for (unsigned row = 1; row <= 2; ++row) for (unsigned side = 1; side <= 2; ++side) Vest(true, row, side, v); }
    // The middle of the chest a row higher, under the collarbones.
    void UpperChest(float v) { for (unsigned row = 0; row <= 1; ++row) for (unsigned side = 1; side <= 2; ++side) Vest(false, row, side, v); }
    // The shoulder on a hand's side, front and back, the outer motors most.
    void Shoulder(int hand, float v) {
        const unsigned outer = hand == 1 ? 3 : 0, inner = hand == 1 ? 2 : 1;
        Vest(false, 0, outer, v); Vest(true, 0, outer, v);
        Vest(false, 0, inner, v * .5f); Vest(true, 0, inner, v * .5f);
    }
    void Arm(int hand, float v) { for (unsigned m = 0; m < 3; ++m) Mix(f.At(BodyArm(hand), m), v); }
    void Arms(float v) { Arm(0, v); Arm(1, v); }
    void Visor(unsigned fromLeft, float v) { Mix(f.At(BodyDevice::Head, BodyVisorMotor(fromLeft)), v); }
    void VisorAll(float v) { for (unsigned i = 0; i < 4; ++i) Visor(i, v); }
    // A spot where something comes from `azimuth`, `height` down the body: the motor nearest it at
    // `v`, the ones round it less over `across` radians round the body and `down` of its height, the
    // rest not at all. Whatever the direction, the nearest motor gets the whole `v`. Spidy's spread
    // over half the vest until 2026-10-20 (0.9, 0.45), a side's blow weaker than a frontal one.
    void Toward(float azimuth, float height, float v, float across = .5f, float down = .3f) {
        std::array<float, 32> near{};
        float most = 0;
        for (unsigned m = 0; m < 32; ++m) {
            const float a = Wrap(BodyVestAzimuth(m) - azimuth) / across, h = (BodyVestHeight(m) - height) / down;
            near[m] = std::exp(-(a * a + h * h));
            most = std::max(most, near[m]);
        }
        if (!(most > 0)) return;
        for (unsigned m = 0; m < 32; ++m)
            if (const float n = near[m] / most; n >= .2f) Vest(m, v * n);
    }
};

constexpr const char* kNames[] = {"strike", "block", "deflect", "two_hand", "one_hand", "winded",
                                  "cast", "hit", "heal", "death", "landing", "hello"};
static_assert(std::size(kNames) == kBodyKinds);
constexpr float kDurations[] = {.26f, .25f, .3f, .14f, .1f, .6f, .22f, .3f, .8f, 1.8f, .3f, .55f};
static_assert(std::size(kDurations) == kBodyKinds);

bool Plays(uint32_t groups, BodyGroup g) { return g == BodyGroup::Always || (groups >> (unsigned)g & 1); }

void Pattern(Paint& p, const BodyEvent& e, float t) {
    const float s = e.strength;
    const int hand = e.hand == 1 ? 1 : 0;
    const bool handed = e.hand == 0 || e.hand == 1;
    // The arm (the shoulder) of the event's hand, else both.
    const auto ownArm = [&](float v) { if (handed) p.Arm(hand, v); else p.Arms(v); };
    const auto ownShoulder = [&](float v) { p.Shoulder(hand, v); if (!handed) p.Shoulder(1 - hand, v); };
    switch (e.kind) {
    case BodyKind::Strike: {
        // As hard and as long as the blow; it comes back through the shoulder. Spidy's punch, a
        // weapon's impact a little heavier.
        const float kick = .06f + .08f * s;
        ownArm((.3f + .35f * s) * Envelope(t, .005f, kick, .08f));
        ownShoulder((.12f + .1f * s) * Envelope(t - .02f, .005f, kick, .06f));
        break;
    }
    case BodyKind::Block:
        // The shield arm takes it, then the shoulder, and the chest a little on that side.
        ownArm(s * .45f * Envelope(t, .005f, .06f, .1f));
        ownShoulder(s * .25f * Envelope(t - .02f, .005f, .06f, .1f));
        p.Toward(hand == 1 ? .5f : -.5f, .25f, s * .16f * Envelope(t - .03f, .005f, .05f, .1f));
        break;
    case BodyKind::Deflect:
        // Met moving into it: sharp in the shield arm, the other arm and the upper chest, a ring in the head.
        ownArm(s * .7f * Envelope(t, .005f, .08f, .1f));
        if (handed) p.Arm(1 - hand, s * .2f * Envelope(t, .005f, .06f, .06f));
        p.UpperChest(s * .3f * Envelope(t, .005f, .06f, .08f));
        p.VisorAll(s * .15f * Envelope(t, .005f, .04f, .06f));
        break;
    case BodyKind::TwoHand:
        p.Arms(s * .22f * Envelope(t, .005f, .05f, .08f));
        break;
    case BodyKind::OneHand:
        p.Arms(s * .12f * Envelope(t, .005f, .04f, .05f));
        break;
    case BodyKind::Winded:
        // Out of breath: two soft heaves in the chest.
        p.Chest(s * .12f * (Envelope(t, .05f, .05f, .12f) + .7f * Envelope(t - .3f, .05f, .05f, .12f)));
        break;
    case BodyKind::Cast:
        // It leaves the hand: a push in the arm, then its shoulder.
        ownArm((.2f + .2f * s) * Envelope(t, .01f, .06f, .1f));
        ownShoulder(s * .1f * Envelope(t - .03f, .01f, .05f, .08f));
        break;
    case BodyKind::Hit: {
        // Where the blow came from, as hard as the HP it took; a heavy one rings in the head. From
        // nowhere known: the chest and the back.
        const float v = (.3f + .45f * s) * Envelope(t, .005f, .06f, .18f);
        if (e.placed) p.Toward(e.azimuth, e.height, v);
        else { p.Chest(v); p.MidBack(.7f * v); }
        if (s > .35f) {
            p.VisorAll(.3f * Envelope(t, .005f, .04f, .1f));
            p.Arms(.15f * Envelope(t, .005f, .04f, .08f));
        }
        break;
    }
    case BodyKind::Heal:
        // Warmth spreading out from the middle of the chest, then round the back.
        for (unsigned m = 0; m < 32; ++m) {
            const float r = std::hypot((float)(m % 4) - 1.5f, ((float)RowOf(m) - 1.5f) * 1.2f);
            const float at = (m >= 16 ? .2f : 0.f) + r * .12f;
            p.Vest(m, s * .22f * Envelope(t - at, .05f, .08f, .15f));
        }
        break;
    case BodyKind::Death:
        // Everything at once, then let go of slowly, the head first.
        p.VestAll(.45f * Envelope(t, .01f, .2f, 1.4f));
        p.Arms(.3f * Envelope(t, .01f, .15f, .9f));
        p.VisorAll(.35f * Envelope(t, .01f, .1f, .5f));
        break;
    case BodyKind::Landing: {
        // From the waist up: the waist first and hardest, the shoulders last and least. A short drop
        // plays the waist's row alone; the rows above and the arms come in from 0.4 to 0.6, a fall that
        // hurts. Spidy's every landing went up the whole vest until bHaptics' notes of 2026-10-22, a
        // jump's "slightly stronger than expected".
        const float high = Clamp01((s - .4f) / .2f);
        for (unsigned row = 0; row < 4; ++row) {
            const float near = (float)(row + 1) / 4;
            const float v = s * (.12f + .45f * near) * (row == 3 ? 1 : high) * Envelope(t - (float)(3 - row) * .03f, .005f, .05f, .12f);
            p.Rows(row, row, v, v);
        }
        if (s > .6f) p.VisorAll((s - .6f) * .6f * Envelope(t - .06f, .005f, .03f, .08f));
        p.Arms(high * s * .15f * Envelope(t - .04f, .005f, .04f, .08f));
        break;
    }
    case BodyKind::Hello: {
        const float v = s * .3f * (Envelope(t, .005f, .1f, .05f) + Envelope(t - .25f, .005f, .1f, .05f));
        p.VestAll(v); p.Arms(v); p.VisorAll(v * .7f);
        break;
    }
    }
}
} // namespace

bool BodyFrame::Any(BodyDevice d) const {
    const auto& m = motors[(unsigned)d];
    return std::any_of(m.begin(), m.begin() + BodyMotorCount(d), [](float v) { return v > 0; });
}

float BodyVestAzimuth(unsigned motor) {
    // The inner columns either side of the zip and the spine, the outer ones round toward the flanks.
    const float across = (float)SideOf(motor) - 1.5f;
    const float out = std::fabs(across) > 1 ? .95f : .3f;
    return (across < 0 ? -1.f : 1.f) * (motor < 16 ? out : kPi - out);
}
float BodyVestHeight(unsigned motor) { return (float)RowOf(motor) / 3; }
const char* BodyKindName(BodyKind k) { return (unsigned)k < kBodyKinds ? kNames[(unsigned)k] : "unknown"; }
float BodyKindDuration(BodyKind k) { return (unsigned)k < kBodyKinds ? kDurations[(unsigned)k] : 0.f; }
BodyGroup BodyKindGroup(BodyKind k) {
    switch (k) {
    case BodyKind::Strike: case BodyKind::Block: case BodyKind::Deflect: case BodyKind::TwoHand:
    case BodyKind::OneHand: case BodyKind::Winded: case BodyKind::Cast:
        return BodyGroup::Combat;
    case BodyKind::Hit: case BodyKind::Heal: case BodyKind::Death:
        return BodyGroup::Damage;
    case BodyKind::Landing:
        return BodyGroup::Movement;
    case BodyKind::Hello:
        break;
    }
    return BodyGroup::Always;
}

float BodyFacing::Update(float headYaw, float seconds) {
    seconds = std::isfinite(seconds) ? std::clamp(seconds, 0.f, .25f) : 0.f;
    if (!std::isfinite(headYaw)) return yaw_;
    if (!set_) { yaw_ = Wrap(headYaw); set_ = true; return yaw_; }
    const float off = Wrap(headYaw - yaw_);
    if (std::fabs(off) > kDeadZone) yaw_ = Wrap(yaw_ + std::copysign(std::min(std::fabs(off) - kDeadZone, kSpeed * seconds), off));
    yaw_ = Wrap(yaw_ + Wrap(headYaw - yaw_) * (1 - std::exp(-kDrift * seconds)));
    return yaw_;
}
float BodyHitHeight(float rise, float flat) {
    const float a = std::atan2(rise, std::max(flat, 1.f));
    return std::isfinite(a) ? std::clamp(.4f - a / 1.5f, 0.f, 1.f) : .4f;
}

void BodyMixer::Post(const BodyEvent& e, double now) {
    if ((unsigned)e.kind >= kBodyKinds || !std::isfinite(now)) return;
    if (!(state_.live || e.kind == BodyKind::Hello) || !Plays(state_.groups, BodyKindGroup(e.kind))) return;
    BodyEvent clean = e;
    clean.strength = Clamp01(e.strength);
    clean.placed = e.placed && std::isfinite(e.azimuth) && std::isfinite(e.height);
    clean.azimuth = std::isfinite(e.azimuth) ? Wrap(e.azimuth) : 0.f;
    clean.height = std::isfinite(e.height) ? Clamp01(e.height) : .4f;
    if (clean.hand != 0 && clean.hand != 1) clean.hand = -1;
    if (active_.size() >= kMostPlaying) active_.erase(active_.begin());
    active_.push_back({clean, now});
}
void BodyMixer::Set(const BodyState& s) {
    state_ = s;
    if (!std::isfinite(s.velocity.x) || !std::isfinite(s.velocity.y) || !std::isfinite(s.velocity.z)) state_.velocity = {};
    for (auto& r : state_.rumble) r = Clamp01(r);
    state_.health = std::isfinite(s.health) ? std::clamp(s.health, 0.f, 1.f) : 1.f;
}
void BodyMixer::Hold(const BodyFrame& f, double now, float seconds) {
    if (!state_.live || !std::isfinite(now) || !(seconds > 0)) return;
    if (held_.size() >= kMostPlaying) held_.erase(held_.begin());
    held_.push_back({f, now + seconds});
}
BodyFrame BodyMixer::Render(double now) {
    BodyFrame f;
    if (!std::isfinite(now)) return f;
    const float dt = last_ < 0 ? 0.f : std::clamp((float)(now - last_), 0.f, .25f);
    last_ = std::max(last_, now);
    std::erase_if(active_, [&](const Active& a) { return now - a.start > BodyKindDuration(a.e.kind); });
    std::erase_if(held_, [&](const Held& h) { return now >= h.until; });
    Paint p{f};
    if (!state_.live) {
        // Out of the world everything stops - but the hello that says the devices were found.
        std::erase_if(active_, [](const Active& a) { return a.e.kind != BodyKind::Hello; });
        held_.clear();
        rumbleFelt_ = {};
        windFelt_ = 0;
        for (const auto& a : active_) Pattern(p, a.e, (float)(now - a.start));
        return f;
    }
    // A group switched off stops at once, what it was playing with it.
    const auto on = [&](BodyGroup g) { return Plays(state_.groups, g); };
    std::erase_if(active_, [&](const Active& a) { return !on(BodyKindGroup(a.e.kind)); });
    // The game's own rumble - what ELDEN RING shakes the pad for: blows taken, a boss's stomp, a
    // critical, an explosion - on the vest, the heavy motor round the waist and the light one at the
    // shoulders, front and back. At most a quarter (what goes on all the time stays under a third),
    // smoothed over 50 ms (the game sets it in steps), and ducked to a third while a pattern of its
    // own speaks for the moment: a blow's direction drowned in a whole-vest wash.
    bool spoken = false;
    for (const auto& a : active_)
        spoken = spoken || a.e.kind == BodyKind::Hit || a.e.kind == BodyKind::Landing || a.e.kind == BodyKind::Death ||
                 a.e.kind == BodyKind::Block || a.e.kind == BodyKind::Deflect;
    const float follow = 1 - std::exp(-dt / .05f);
    for (unsigned h = 0; h < 2; ++h)
        rumbleFelt_[h] += ((on(BodyGroup::Rumble) ? .25f * state_.rumble[h] * (spoken ? .33f : 1.f) : 0.f) - rumbleFelt_[h]) * follow;
    if (rumbleFelt_[0] > .005f || rumbleFelt_[1] > .005f)
        for (unsigned m = 0; m < 32; ++m) {
            const float low = (float)RowOf(m) / 3;   // 0 the shoulders, 1 the waist
            const float heavy = rumbleFelt_[0] * (.4f + .6f * low), light = rumbleFelt_[1] * (1 - .6f * low);
            p.Vest(m, (1 - (1 - heavy) * (1 - light)) * (.85f + .15f * Flutter(m, now)));
        }
    // The wind of a fall: from 15 m/s (nothing) to 40 (16%), on the motors that face where the body
    // goes, the nearer they face it the more; falling, the waist most; and on the face. Smoothed over a
    // quarter of a second. Spidy's measures (2026-10-20: from 12 m/s on half the vest was too much).
    const float speed = state_.velocity.Length();
    const bool free = on(BodyGroup::Movement) && state_.airborne;
    windFelt_ += ((free ? .16f * Clamp01((speed - 15) / 25) : 0.f) - windFelt_) * (1 - std::exp(-dt / .25f));
    if (windFelt_ > .005f && speed > 1) {
        const Vec3 way = state_.velocity * (1.f / speed);
        const float rising = std::max(0.f, -way.y);
        for (unsigned m = 0; m < 32; ++m) {
            // The motor's facing: round the body, the shoulders' row tipped up a little and the waist's down.
            const float az = BodyVestAzimuth(m), tip = .25f - .5f * BodyVestHeight(m);
            const Vec3 n{std::sin(az) * std::cos(tip), std::sin(tip), std::cos(az) * std::cos(tip)};
            const float into = std::min(1.f, std::max(0.f, n.Dot(way)) + rising * .6f * BodyVestHeight(m));
            if (into > .1f) p.Vest(m, windFelt_ * into * into * (.7f + .3f * Swell(m, now)));
        }
        if (const float face = std::max(0.f, way.z) * .8f + rising * .3f; face > .1f)
            for (unsigned i = 0; i < 4; ++i) p.Visor(i, windFelt_ * face * face * (.7f + .3f * Swell(40 + i, now)));
    }
    // The heart, high on the chest left of its middle, racing below 30% of the HP. A row lower until
    // bHaptics' notes of 2026-10-22, further from the heart.
    if (state_.health > 0 && state_.health < .3f && on(BodyGroup::Damage)) {
        const float bpm = 80 + (.3f - state_.health) * 200, strength = .18f + (.3f - state_.health) * .6f;
        const float t = (float)std::fmod(now, 60.0 / bpm);
        const float beat = strength * (Envelope(t, .02f, .07f, .05f) + .7f * Envelope(t - .2f, .02f, .06f, .05f));
        p.Vest(false, 0, 1, beat);
        p.Vest(false, 0, 0, .6f * beat);
        p.Vest(false, 1, 1, .5f * beat);
    }
    for (const auto& a : active_) Pattern(p, a.e, (float)(now - a.start));
    for (const auto& h : held_)
        for (unsigned d = 0; d < kBodyDevices; ++d)
            for (unsigned m = 0; m < BodyMotorCount((BodyDevice)d); ++m) Mix(f.motors[d][m], h.frame.motors[d][m]);
    return f;
}

float BodyLandingWatch::Update(float seconds, bool airborne, float downSpeed) {
    seconds = std::isfinite(seconds) ? std::clamp(seconds, 0.f, .1f) : 0.f;
    downSpeed = std::isfinite(downSpeed) ? downSpeed : 0.f;
    float landed = 0;
    if (airborne) {
        airSeconds_ += seconds;
        // The speed it came down at in the fall's last moments: held, and let go of over a fifth of a second.
        fastest_ = std::max(downSpeed, fastest_ * std::exp(-seconds / .2f));
    } else if (airborne_ && airSeconds_ >= .25f && fastest_ > 4) {
        // A hop of a moment is no fall; a short drop lands softly, a long one hard.
        landed = .15f + .85f * Clamp01((fastest_ - 4) / 24);
    }
    if (!airborne) fastest_ = airSeconds_ = 0;
    airborne_ = airborne;
    return landed;
}

}
