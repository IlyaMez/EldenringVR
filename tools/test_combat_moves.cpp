#include "combat_moves.h"
#include <cstdio>
#include <cstdlib>

using namespace ervr;
static void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}

static SkillTriggerIn In(bool down, double now, bool two = false, bool oneOk = true, bool effect = true, bool game = false) {
    SkillTriggerIn in;
    in.down = down; in.now = now; in.twoHanded = two; in.oneHandAllowed = oneOk;
    in.effectMode = effect; in.gameMode = game; in.holdSeconds = 0.3f;
    return in;
}

static ThrowParamRow BackstabRow() {
    ThrowParamRow r{};
    r.atkChrId = 0; r.defChrId = 4100; r.padType = 1; r.throwType = kThrowTypeBackstab;
    r.dist = 1.5f; r.diffAngMin = -60.f; r.diffAngMax = 60.f; r.upperYRange = 0.5f; r.lowerYRange = 0.5f; r.diffAngMyToDef = 60.f;
    r.distStart = 2.5f; r.diffAngMinStart = -45.f; r.diffAngMaxStart = 45.f; r.upperYRangeStart = 0.5f; r.lowerYRangeStart = 0.5f;
    r.diffAngMyToDefStart = 30.f;
    return r;
}

int main() {
    // ---- skill trigger ----
    {
        SkillTrigger t;
        SkillTriggerOut o = t.Step(In(true, 0.0));
        Check(o.hold && o.started && o.request == 0, "one-handed: the press holds the skill at once");
        o = t.Step(In(true, 2.0));
        Check(o.hold && !o.started, "one-handed: held as long as the trigger is");
        o = t.Step(In(false, 2.1));
        Check(!o.hold && o.request == 0, "one-handed: letting go releases it, no replay");
    }
    {
        SkillTrigger t;
        SkillTriggerOut o = t.Step(In(true, 0.0, false, false));
        Check(!o.hold, "a hand that may not hold the skill (a staff) gets nothing");
        o = t.Step(In(false, 0.2, false, false));
        Check(!o.hold && o.request == 0, "and nothing on release");
    }
    {
        SkillTrigger t;
        SkillTriggerOut o = t.Step(In(true, 0.0, true));
        Check(!o.hold, "two-handed effect mode: a press is a tap until held long enough");
        o = t.Step(In(false, 0.1, true));
        Check(!o.hold && o.request == 1, "two-handed effect mode: a tap replays the effect");
    }
    {
        SkillTrigger t;
        t.Step(In(true, 0.0, true));
        SkillTriggerOut o = t.Step(In(true, 0.2, true));
        Check(!o.hold, "not yet held at 0.2 s");
        o = t.Step(In(true, 0.31, true));
        Check(o.hold && o.started && o.request == 3, "held past SkillHoldSeconds: the game's own skill, held and recorded");
        o = t.Step(In(true, 0.5, true));
        Check(o.hold && o.request == 0, "the start is reported once");
        o = t.Step(In(false, 1.0, true));
        Check(!o.hold && o.request == 0, "a held press is no tap on release");
    }
    {
        SkillTrigger t;
        SkillTriggerOut o = t.Step(In(true, 0.0, true, true, false, true));
        Check(o.hold && o.started, "two-handed game mode: held from the press");
    }
    {
        SkillTrigger t;
        t.Step(In(true, 0.0));
        t.Reset();
        SkillTriggerOut o = t.Step(In(true, 0.1));
        Check(!o.hold, "after a stand-down a trigger still held does not restart the skill");
        o = t.Step(In(false, 0.2));
        o = t.Step(In(true, 0.3));
        Check(o.hold && o.started, "a fresh press after letting go works again");
    }
    {
        SkillTrigger t;
        t.Reset();
        SkillTriggerOut o = t.Step(In(true, 0.0));
        Check(o.hold, "a reset with nothing pressed does not swallow the next press");
    }

    // ---- stance follow-ups ----
    {
        auto S = [](double now, double held, float tip, float hand) {
            StanceSwingIn in; in.now = now; in.heldFor = held; in.tip = tip; in.hand = hand; return in;
        };
        StanceSwing st; st.Reset();
        // The first build pressed R1 at the live contact's 0.8 m/s; a held blade drifting at 1-2 m/s is no swing.
        Check(st.Step(S(0.00, 0.5, 0.2f, 0.1f)) == StancePress::None, "at rest: nothing");
        Check(st.Step(S(0.02, 0.5, 1.2f, 0.5f)) == StancePress::None && st.Step(S(0.04, 0.5, 2.5f, 1.0f)) == StancePress::None,
              "a drifting blade under SwingOnSpeed presses nothing");
        Check(st.Step(S(0.06, 0.5, 3.5f, 0.3f)) == StancePress::None && st.Step(S(0.14, 0.5, 3.5f, 0.3f)) == StancePress::None,
              "a wrist flick (tip fast, hand slow) presses nothing");
        // A deliberate swing: through the arm window, then one press.
        Check(st.Step(S(0.20, 0.6, 4.0f, 1.5f)) == StancePress::None, "the swing starts: watching its peak");
        Check(st.Step(S(0.23, 0.6, 4.6f, 1.6f)) == StancePress::None, "inside the arm window");
        Check(st.Step(S(0.27, 0.6, 4.4f, 1.6f)) == StancePress::Light, "past the arm window: the light follow-up");
        Check(std::fabs(st.PressPeak() - 4.6f) < 1e-4f, "the press reports its peak");
        // The follow-through and the return to guard are no second press.
        Check(st.Step(S(0.40, 0.8, 5.0f, 2.0f)) == StancePress::None, "follow-through: no press");
        Check(st.Step(S(0.60, 1.0, 1.0f, 0.4f)) == StancePress::None, "slowed, but inside the gap");
        Check(st.Step(S(0.70, 1.1, 4.0f, 1.5f)) == StancePress::None, "the gap has passed but the blade had not rested since: no press");
        Check(st.Step(S(0.80, 1.2, 0.5f, 0.2f)) == StancePress::None, "rested after the gap: ready");
        Check(st.Step(S(0.90, 1.3, 7.0f, 2.5f)) == StancePress::None && st.Step(S(0.97, 1.4, 7.5f, 2.5f)) == StancePress::Heavy,
              "a hard swing: the heavy follow-up");
    }
    {
        StanceSwing st; st.Reset();
        StanceSwingIn in; in.tip = 4.0f; in.hand = 1.5f;
        in.now = 0.0; in.heldFor = 0.0;
        Check(st.Step(in) == StancePress::None, "a blade already swinging when the trigger is pulled must rest first");
        in.now = 0.1; in.heldFor = 0.1; in.tip = 0.3f; in.hand = 0.1f;
        Check(st.Step(in) == StancePress::None, "rested");
        in.now = 0.11; in.heldFor = 0.11; in.tip = 4.0f; in.hand = 1.5f;
        Check(st.Step(in) == StancePress::None, "swung before the stance settled: waits");
        in.now = 0.16; in.heldFor = 0.16;
        Check(st.Step(in) == StancePress::None, "settled: the swing starts now");
        in.now = 0.23; in.heldFor = 0.23;
        Check(st.Step(in) == StancePress::Light, "and presses after its arm window");
    }
    {
        StanceSwing st; st.Reset();
        StanceSwingIn in; in.heldFor = 1.0;
        in.now = 0.0; in.tip = 0.2f; st.Step(in);
        in.now = 0.01; in.tip = 3.5f; in.hand = 1.0f; st.Step(in);
        in.now = 0.03; in.tip = 1.0f;
        Check(st.Step(in) == StancePress::None, "a twitch that dies inside the arm window is no swing");
        in.now = 0.05; in.tip = 3.5f; st.Step(in);
        in.now = 0.12;
        Check(st.Step(in) == StancePress::Light, "and the next real swing still counts");
    }
    // ---- where the swing aims ----
    {
        // A slash from the right (+X) across the front (+Z) to the left, fastest in the middle: it aims ahead.
        StanceSwing st; st.Reset();
        StanceSwingIn in; in.heldFor = 1.0; in.hand = 2.0f;
        in.now = 0.0; in.tip = 0.2f; in.sz = 1.f; st.Step(in);
        int presses = 0;
        for (int i = 0; i <= 30; ++i) {
            const float th = 1.5707963f - 3.14159265f * (float)i / 30.f;   // +90 .. -90 deg
            in.now = 0.01 + i * 0.01; in.tip = 3.2f + 4.f * std::cos(th);
            in.sx = std::sin(th); in.sy = 0.f; in.sz = std::cos(th);
            if (st.Step(in) != StancePress::None) ++presses;
        }
        Check(presses == 1, "one slash, one follow-up");
        Check(!st.TakeFinished() && st.Collecting(), "still swinging: the aim is still being measured");
        in.now = 0.5; in.tip = 0.5f; st.Step(in);
        Check(st.TakeFinished() && !st.TakeFinished(), "the swing slowed: its aim is final, reported once");
        float x, y, z; double at = 0;
        Check(st.Aim(&x, &y, &z, &at) && std::fabs(std::atan2(x, z)) < 0.05f && std::fabs(at - 0.07) < 1e-6,
              "a slash across the front aims ahead (the middle of its arc), not along the blade's travel");
        st.Reset();
        Check(st.Aim(&x, &y, &z, nullptr), "a new hold keeps the last swing's aim (its projectile may leave after you let go)");
    }
    {
        // A slash off to the left: from straight ahead to 150 deg left. Let go of the trigger mid-swing: the aim
        // keeps following the swing until it slows.
        StanceSwing st; st.Reset();
        StanceSwingIn in; in.heldFor = 1.0; in.hand = 2.0f;
        in.now = 0.0; in.tip = 0.2f; in.sz = 1.f; st.Step(in);
        for (int i = 0; i <= 30; ++i) {
            const float th = -2.6179939f * (float)i / 30.f;   // 0 .. -150 deg
            in.now = 0.01 + i * 0.01; in.tip = 4.f; in.sx = std::sin(th); in.sy = 0.f; in.sz = std::cos(th);
            if (i < 10) st.Step(in);
            else { if (i == 10) st.Reset(); st.Follow(in); }
        }
        float x, y, z;
        Check(st.Aim(&x, &y, &z, nullptr) && std::fabs(std::atan2(x, z) * 57.29578f + 75.f) < 3.f,
              "a slash off to the left aims left, measured to the swing's end after the trigger was let go");
    }
    {
        // A forehand that rises slowly past SwingOffSpeed before it is fast enough to count, and slows the same way
        // after: the rise counts too (pre-roll), so the aim is not pulled toward the arc's end (test 2: 16 of 18 left).
        StanceSwing st; st.Reset();
        StanceSwingIn in; in.heldFor = 1.0; in.hand = 2.0f;
        in.now = 0.0; in.tip = 0.2f; in.sz = 1.f; st.Step(in);
        for (int i = 0; i <= 30; ++i) {
            const float th = 1.5707963f - 3.14159265f * (float)i / 30.f;   // +90 .. -90 deg
            in.now = 0.01 + i * 0.01; in.tip = 1.6f + 4.f * std::cos(th);
            in.sx = std::sin(th); in.sy = 0.f; in.sz = std::cos(th);
            st.Step(in);
        }
        in.now = 0.5; in.tip = 0.5f; st.Step(in);
        float x, y, z;
        Check(st.Aim(&x, &y, &z, nullptr) && std::fabs(std::atan2(x, z)) * 57.29578f < 2.f,
              "a forehand rising and falling through SwingOffSpeed still centres on the middle of its arc");
    }
    {
        // A wind-up: the sword drawn back from in front toward your right and behind. Then, without stopping, the
        // forehand. No follow-up while it moves away from your view; one once the strike comes round.
        StanceSwing st; st.Reset();
        StanceSwingIn in; in.heldFor = 1.0; in.hand = 1.5f; in.vx = 0.f; in.vz = 1.f;
        in.now = 0.0; in.tip = 0.2f; in.sz = 1.f; st.Step(in);
        double t = 0.0; int presses = 0;
        for (int d = 20; d <= 150; d += 10) {   // wind-up, 4 m/s
            t += 0.01; in.now = t; in.tip = 4.f; in.sx = std::sin(d * 0.0174533f); in.sz = std::cos(d * 0.0174533f);
            if (st.Step(in) != StancePress::None) ++presses;
        }
        Check(presses == 0, "a wind-up moving away from your view is no follow-up");
        double pressAt = -1.0;
        for (int d = 150; d >= -40; d -= 10) {  // the forehand comes round, 6 m/s
            t += 0.01; in.now = t; in.tip = 6.f; in.sx = std::sin(d * 0.0174533f); in.sz = std::cos(d * 0.0174533f);
            if (st.Step(in) != StancePress::None) { ++presses; if (pressAt < 0) pressAt = t; }
        }
        Check(presses == 1 && pressAt > 0.13 && pressAt < 0.22, "the strike that follows it presses, once, early in its arc");
    }
    {
        StanceSwing st; st.Reset();
        StanceSwingIn in; in.heldFor = 1.0; in.hand = 1.5f; in.vx = 0.f; in.vz = 1.f;
        in.now = 0.0; in.tip = 0.2f; in.sz = 1.f; st.Step(in);
        double t = 0.0;
        for (int d = 10; d <= 140; d += 10) { t += 0.01; in.now = t; in.tip = 4.f; in.sx = std::sin(d * 0.0174533f); in.sz = std::cos(d * 0.0174533f); st.Step(in); }
        t += 0.01; in.now = t; in.tip = 0.5f; st.Step(in);
        float ang = 0.f;
        Check(st.TakeIgnored(&ang) && ang > 100.f && !st.TakeIgnored(nullptr), "a wind-up that comes to rest is reported once as ignored");
        // A slash that stays in front is never refused.
        int presses = 0;
        for (int d = 30; d >= -30; d -= 6) { t += 0.01; in.now = t; in.tip = 5.f; in.sx = std::sin(d * 0.0174533f); in.sz = std::cos(d * 0.0174533f); if (st.Step(in) != StancePress::None) ++presses; }
        Check(presses == 1, "a slash across the front presses");
    }
    {
        float x = std::sin(80.f * 0.0174533f), z = std::cos(80.f * 0.0174533f);
        Check(ClampTowardView(&x, &z, 0.f, 1.f, 45.f) && std::fabs(std::atan2(x, z) * 57.29578f - 45.f) < 0.01f,
              "an aim 80 deg right of the view is turned in to 45");
        x = std::sin(-30.f * 0.0174533f); z = std::cos(-30.f * 0.0174533f);
        Check(!ClampTowardView(&x, &z, 0.f, 1.f, 45.f) && std::fabs(std::atan2(x, z) * 57.29578f + 30.f) < 0.01f, "inside the limit: unchanged");
        x = 0.f; z = -1.f;
        Check(ClampTowardView(&x, &z, 0.f, 1.f, 45.f) && std::fabs(std::fabs(std::atan2(x, z)) * 57.29578f - 45.f) < 0.01f, "straight behind: to the limit");
    }
    {
        StanceSwing st;
        float x, y, z;
        Check(!st.Aim(&x, &y, &z, nullptr), "no swing yet: no aim");
        float dx, dy, dz;
        float yaw = SwingAimDirection(1.f, -0.2f, 0.f, 0.f, 0.1f, 0.7f, &dx, &dy, &dz);
        Check(std::fabs(yaw - 1.5707963f) < 1e-4f && std::fabs(dy - std::sin(0.1f)) < 1e-5f, "the swing's yaw, the view's pitch");
        yaw = SwingAimDirection(0.1f, -1.f, 0.05f, 0.3f, 0.f, 0.7f, &dx, &dy, &dz);
        Check(std::fabs(yaw - 0.3f) < 1e-5f, "a swing straight down has no direction of its own: where you look");
        SwingAimDirection(0.f, 0.f, 1.f, 0.f, 1.2f, 0.7f, &dx, &dy, &dz);
        Check(std::fabs(dy - std::sin(0.7f)) < 1e-5f && std::fabs(dx * dx + dy * dy + dz * dz - 1.f) < 1e-4f, "pitch clamped, unit length");
    }
    Check(SkillHand(true, 0, false) == 0 && SkillHand(true, 1, true) == 1, "two-handed: the two-handed weapon's skill");
    Check(SkillHand(false, 1, false) == 1, "one-handed, a sword (or a seal) in the left: the right weapon's skill");
    Check(SkillHand(false, 1, true) == 0, "one-handed, a shield or bow in the left: its skill");

    // ---- throw geometry ----
    Check(std::fabs(WrapDeg(190.f) + 170.f) < 1e-3f, "WrapDeg wraps past 180");
    const ThrowParamRow bs = BackstabRow();
    Check(ThrowRowKind(bs) == CritKind::Backstab, "throw type 1 is a backstab");
    {
        ThrowParamRow rp = bs; rp.throwType = 0;
        Check(ThrowRowKind(rp) == CritKind::Riposte, "type 0 (any facing) needs the enemy opened up: not a backstab");
        rp.throwType = 20;
        Check(ThrowRowKind(rp) == CritKind::Riposte, "type 20 (face to face) is not a backstab");
    }
    {
        // The game's type-20 window 90..-90 wraps through 180: face to face holds, back to back does not.
        ThrowParamRow ff{};
        ff.throwType = 20; ff.dist = 1.8f; ff.upperYRange = ff.lowerYRange = 0.65f; ff.diffAngMin = 90.f; ff.diffAngMax = -90.f;
        ff.diffAngMyToDef = 60.f;
        ThrowGeom f; f.dz = 1.f; f.dYaw = 3.14159265f;    // the enemy faces the player
        Check(ThrowRowMatches(ff, MeasureThrow(f)) == 1, "a wrapping window holds face to face");
        f.dYaw = 0.f;                                     // the enemy shows its back
        Check(ThrowRowMatches(ff, MeasureThrow(f)) == 0, "a wrapping window does not hold back to back");
    }
    ThrowGeom g;
    g.ax = 0; g.ay = 0; g.az = 0; g.aYaw = 0.f;       // the player looks down +Z
    g.dx = 0; g.dy = 0; g.dz = 1.f; g.dYaw = 0.f;     // the enemy 1 m ahead, facing away
    ThrowMeasure m = MeasureThrow(g);
    Check(std::fabs(m.dist - 1.f) < 1e-4f && std::fabs(m.diffAng) < 1e-3f && m.myToDef < 1e-3f, "measure: behind, same facing");
    Check(ThrowRowMatches(bs, m) == 1, "behind an enemy facing away, in reach: backstab");
    {
        ThrowGeom f = g; f.dYaw = 3.14159265f;           // the enemy turned to face the player
        Check(ThrowRowMatches(bs, MeasureThrow(f)) == 0, "face to face is no backstab");
    }
    {
        ThrowGeom f = g; f.dz = 2.0f;                    // 2 m: outside Dist, inside Dist_start
        Check(ThrowRowMatches(bs, MeasureThrow(f)) == 2, "the backstab-start window reaches further");
        f.dz = 3.0f;
        Check(ThrowRowMatches(bs, MeasureThrow(f)) == 0, "too far for either window");
    }
    {
        ThrowGeom f = g; f.dy = 0.8f;                    // the enemy stands on a ledge
        Check(ThrowRowMatches(bs, MeasureThrow(f)) == 0, "height outside the window");
    }
    {
        ThrowGeom f = g; f.aYaw = 1.5707963f;            // the player looks 90 deg away from the enemy
        f.dYaw = 1.5707963f;                             // (and the enemy faces that way too, so diffAng is 0)
        m = MeasureThrow(f);
        Check(m.myToDef > 89.f && ThrowRowMatches(bs, m) == 0, "not facing the enemy: no throw");
    }
    {
        ThrowGeom f = g; f.dx = 0.5f; f.dz = 1.0f;       // a little to the side, still inside 60 deg
        m = MeasureThrow(f);
        Check(m.myToDef > 26.f && m.myToDef < 27.f && ThrowRowMatches(bs, m) == 1, "off to the side within the cone");
    }
    {
        ThrowParamRow empty{};
        Check(ThrowRowMatches(empty, MeasureThrow(g)) == 0, "a row with no distance never matches");
    }
    // BackstabHolds uses the narrower start window when the row has one (the game's type-1 rows: 1.5 m, +-45 deg).
    Check(BackstabHolds(bs, MeasureThrow(g)), "1 m behind, same facing: a backstab to press for");
    {
        ThrowGeom f = g; f.dYaw = 1.0f;                  // the enemy turned 57 deg: inside the main window, outside start
        m = MeasureThrow(f);
        Check(ThrowRowMatches(bs, m) == 1 && !BackstabHolds(bs, m), "only the start window decides a press");
        f = g; f.dz = 2.0f;                              // 2 m: in the test row's 2.5 m start window, so a press
        Check(BackstabHolds(bs, MeasureThrow(f)), "start window distance");
        ThrowParamRow noStart = bs; noStart.distStart = 0.f;
        Check(!BackstabHolds(noStart, MeasureThrow(f)), "without a start window the main one (1.5 m) decides");
    }
    std::puts("combat moves: ok");
    return 0;
}
