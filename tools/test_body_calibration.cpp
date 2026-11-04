// The T-pose body calibration (body_calibration_logic.h): what counts as the pose and what each failed check asks
// for, the hold and its lapses, the measurements, the scales they give the arms and the belt, the panel's place;
// and the stroke font the panel is written in (overlay_text.h).
#include "body_calibration_logic.h"
#include "overlay_text.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace ervr;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}
bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

// A player `height` m tall in a T-pose at the origin, looking down -z: each wrist `arm` m straight out from where
// CalibBody puts that shoulder, `lift` m above it.
CalibSample TPose(float height, float arm, float lift = 0.f) {
    const CalibBody b;
    CalibSample s;
    const float eye = height * kCalibEyeShare, scale = eye / b.eye;
    s.headTracked = true;
    s.head = {0.f, eye, 0.f};
    const Vec3 forward{0.f, 0.f, -1.f}, up{0.f, 1.f, 0.f}, left{-1.f, 0.f, 0.f};
    for (int i = 0; i < 2; ++i) {
        const Vec3 sh = b.shoulder[i];
        const Vec3 shoulder = s.head + (forward * sh.x + up * sh.y + left * sh.z) * scale;
        s.wrist[i] = shoulder + (i == 0 ? left : -left) * arm + Vec3{0.f, lift, 0.f};
        s.handTracked[i] = true;
        s.trigger[i] = 1.f;
    }
    s.seconds = 1.f / 90.f;
    return s;
}
// Runs `s` for `seconds`; true when the calibration finished on the way.
bool Hold(BodyCalibration& c, const CalibSample& s, float seconds) {
    bool done = false;
    for (float t = 0.f; t < seconds; t += s.seconds) done |= c.Update(s);
    return done;
}
CalibHint HintFor(const CalibSample& s) {
    BodyCalibration c;
    c.Start();
    c.Update(s); c.Update(s);   // twice: the stillness check needs a previous frame
    return c.Hint();
}

int main() {
    // A 1.76 m player, arms 0.60 m: measured as such after 1.5 s held.
    {
        BodyCalibration c;
        Check(c.Phase() == CalibPhase::idle && !c.Update(TPose(1.76f, 0.6f)), "nothing is measured before Start");
        c.Start();
        Check(c.Phase() == CalibPhase::waiting, "Start shows the instructions");
        const CalibSample s = TPose(1.76f, 0.6f);
        Check(!Hold(c, s, 1.0f) && c.Phase() == CalibPhase::holding && c.Hint() == CalibHint::none, "a second of the pose is not enough");
        Check(c.Progress() > 0.6f && c.Progress() < 0.7f && c.ArmReady(0) && c.ArmReady(1), "the hold fills, both arms in place");
        Check(Hold(c, s, 0.6f) && c.Phase() == CalibPhase::done && Near(c.Progress(), 1.f), "1.5 s held measures");
        Check(Near(c.Result().eyeHeight, 1.76f * kCalibEyeShare) && Near(CalibHeight(c.Result().eyeHeight), 1.76f), "the eye height and the height");
        Check(Near(c.Result().armLength, 0.6f) && Near(c.Result().reach[0], 0.6f) && Near(c.Result().reach[1], 0.6f), "the arm length");
        Check(!c.Update(s) && c.Phase() == CalibPhase::done, "done stays done");
        c.Stop();
        Check(c.Phase() == CalibPhase::idle && Near(c.Result().armLength, 0.6f), "Stop hides the panel; the result stays");
    }
    // The longer arm is the arm length: the shorter is the bent one.
    {
        BodyCalibration c;
        c.Start();
        CalibSample s = TPose(1.88f, 0.64f);
        s.wrist[1] = s.wrist[1] + Vec3{-0.03f, 0.f, 0.f};   // the right wrist 3 cm in: a slightly bent right arm
        Check(Hold(c, s, 1.6f), "a slightly bent arm still counts");
        Check(Near(c.Result().armLength, 0.64f) && c.Result().reach[1] < c.Result().reach[0], "the straighter arm's reach is kept");
        Check(Near(CalibHeight(c.Result().eyeHeight), 1.88f), "a tall player's height");
    }
    // Each check, the first that fails, and the line the panel shows for it.
    {
        CalibSample s = TPose(1.76f, 0.6f);
        Check(HintFor(s) == CalibHint::none, "the pose counts");
        s.handTracked[1] = false;
        Check(HintFor(s) == CalibHint::tracking, "an untracked controller");
        s = TPose(1.76f, 0.6f); s.head.y = 0.9f;
        Check(HintFor(s) == CalibHint::standUp, "eyes under a metre: sitting or kneeling");
        Check(HintFor(TPose(2.4f, 0.7f)) == CalibHint::floor && HintFor(TPose(1.15f, 0.5f)) == CalibHint::floor,
              "a height outside 1.20-2.20 m is the headset's floor");
        Check(HintFor(TPose(1.76f, 0.6f, -0.7f)) == CalibHint::armsOut, "arms hanging down are not out to the sides");
        {
            CalibSample crossed = TPose(1.76f, 0.6f);
            const Vec3 l = crossed.wrist[0];
            crossed.wrist[0] = crossed.wrist[1]; crossed.wrist[1] = l;
            Check(HintFor(crossed) == CalibHint::armsOut, "crossed arms are not out to the sides");
        }
        Check(HintFor(TPose(1.76f, 0.3f)) == CalibHint::straight, "bent arms");
        {
            CalibSample uneven = TPose(1.76f, 0.6f);
            uneven.wrist[1] = uneven.wrist[1] + Vec3{-0.15f, 0.f, 0.f};
            Check(HintFor(uneven) == CalibHint::straight, "one arm much shorter than the other");
        }
        // Arms this long for the eyes put the controllers further apart than arms can: the floor is too high.
        Check(HintFor(TPose(1.5f, 0.75f)) == CalibHint::floor, "a span too wide for the eyes");
        s = TPose(1.76f, 0.6f); s.headRot = Quat::AxisAngle({1.f, 0.f, 0.f}, -0.7f);   // looking down 40 degrees
        Check(HintFor(s) == CalibHint::lookAhead, "a head tilted down");
        s = TPose(1.76f, 0.6f); s.headRot = Quat::YawQ(0.9f);
        Check(HintFor(s) == CalibHint::lookAhead, "a head turned away from the arms");
        s = TPose(1.76f, 0.6f); s.trigger[0] = 0.2f;
        Check(HintFor(s) == CalibHint::triggers, "a trigger let go");
        {
            BodyCalibration c;
            c.Start();
            CalibSample a = TPose(1.76f, 0.6f);
            c.Update(a);
            a.wrist[0] = a.wrist[0] + Vec3{0.f, 0.02f, 0.f};   // 2 cm in a frame: 1.8 m/s
            c.Update(a);
            Check(c.Hint() == CalibHint::still, "a hand moving");
        }
        for (CalibHint h : {CalibHint::tracking, CalibHint::standUp, CalibHint::armsOut, CalibHint::straight, CalibHint::lookAhead,
                            CalibHint::triggers, CalibHint::still, CalibHint::floor})
            Check(CalibHintText(h) && *CalibHintText(h) && TextWidth(CalibHintText(h), 1.f) > 0.f, "every hint has a line to show");
    }
    // The triggers: past 0.6 to count, then held past 0.35.
    {
        BodyCalibration c;
        c.Start();
        CalibSample s = TPose(1.76f, 0.6f);
        s.trigger[0] = s.trigger[1] = 0.5f;
        c.Update(s);
        Check(c.Hint() == CalibHint::triggers, "a half-pressed trigger does not start the hold");
        s.trigger[0] = s.trigger[1] = 0.7f; c.Update(s);
        s.trigger[0] = s.trigger[1] = 0.4f; c.Update(s);
        Check(c.Hint() == CalibHint::none, "a trigger eased back after counting still counts");
    }
    // A short lapse pauses the hold; a longer one starts it over.
    {
        BodyCalibration c;
        c.Start();
        CalibSample s = TPose(1.76f, 0.6f), off = s;
        off.trigger[1] = 0.f;
        Hold(c, s, 0.75f);
        const float half = c.Progress();
        Hold(c, off, 0.15f);
        Check(c.Phase() == CalibPhase::holding && Near(c.Progress(), half), "a 0.15 s lapse pauses the hold");
        Check(Hold(c, s, 0.8f), "and the hold goes on from there");
        c.Start();
        Hold(c, s, 0.75f);
        Hold(c, off, 0.4f);
        Check(c.Phase() == CalibPhase::waiting && c.Progress() == 0.f, "a 0.4 s lapse starts it over");
        Check(!Hold(c, s, 1.0f), "from the beginning");
    }

    // What the measurements change.
    Check(Near(CalibArmScale(0.62f, 0.55f), 0.62f / 0.55f) && Near(CalibArmScale(0.5f, 0.55f), 0.5f / 0.55f), "the arm takes your length");
    Check(Near(CalibArmScale(0.9f, 0.55f), kCalibMaxArmScale) && Near(CalibArmScale(0.3f, 0.55f), kCalibMinArmScale), "within 0.8-1.25");
    Check(Near(CalibArmScale(0.62f, 0.55f, 0.9f), 0.62f * 0.9f / 0.55f), "HandScale brings the hands closer: the arm with them");
    Check(CalibArmScale(0.f, 0.55f) == 1.f && CalibArmScale(0.6f, 0.f) == 1.f && CalibArmScale(NAN, 0.55f) == 1.f, "unknown: unchanged");
    Check(Near(CalibBeltDrop(0.6f, 1.76f * kCalibEyeShare), 0.6f) && Near(CalibBeltDrop(0.6f, 1.88f * kCalibEyeShare), 0.6f * 1.88f / 1.76f) &&
          Near(CalibBeltDrop(0.6f, 0.f), 0.6f), "the belt's drop follows the height from a 1.76 m player's");
    Check(CalibValid(1650, 600) && !CalibValid(0, 600) && !CalibValid(1650, 0) && !CalibValid(3000, 600) && !CalibValid(1650, 2000),
          "what [Body] keeps");

    // The panel: 1.4 m ahead and 12 cm below the eyes, upright, its image facing the player.
    {
        const Pose p = CalibPanelPose({0.f, 1.6f, 0.f}, Quat{});
        Check(Near(p.pos.x, 0.f) && Near(p.pos.y, 1.48f) && Near(p.pos.z, -1.4f), "ahead of a player looking down -z");
        const Vec3 n = p.rot.Rotate({0.f, 0.f, 1.f}), r = p.rot.Rotate({1.f, 0.f, 0.f});
        Check(Near(n.z, 1.f) && Near(r.x, 1.f), "facing them, its right to their right");
        const Pose q = CalibPanelPose({0.f, 1.6f, 0.f}, Quat::YawQ(1.5707963f) * Quat::AxisAngle({1.f, 0.f, 0.f}, -0.5f));
        const Vec3 qn = q.rot.Rotate({0.f, 0.f, 1.f}), qu = q.rot.Rotate({0.f, 1.f, 0.f});
        Check(Near(q.pos.x, -1.4f) && Near(q.pos.z, 0.f) && Near(qn.x, 1.f) && Near(qu.y, 1.f),
              "a player turned left and looking down gets it upright where they face");
    }

    // The panel's font.
    {
        Check(Near(TextWidth("A", 12.f), 8.f) && Near(TextWidth("AB", 12.f), 19.f) && TextWidth("", 12.f) == 0.f, "glyph and spacing widths");
        Check(Near(TextWidth("a b", 12.f), TextWidth("A B", 12.f)), "lower case is drawn in capitals");
        Check(Near(TextFit("STRETCH YOUR ARMS OUT TO THE SIDES", 30.f, 100.f) * TextWidth("STRETCH YOUR ARMS OUT TO THE SIDES", 1.f), 100.f, 0.01f),
              "a line too long shrinks to the width");
        Check(TextFit("HI", 30.f, 1000.f) == 30.f, "a short line keeps its height");
        Canvas cv;
        cv.Reset(64, 32);
        CanvasText(cv, "I", 32.f, 28.f, 24.f, Px{1.f, 1.f, 1.f, 1.f});
        float ink = 0.f, outside = 0.f;
        for (int y = 0; y < cv.h; ++y)
            for (int x = 0; x < cv.w; ++x) (y < 2 ? outside : ink) += cv.px[(size_t)y * cv.w + x].a;
        Check(ink > 40.f && outside < 0.5f, "a glyph draws within its cap height above the baseline");
    }

    std::puts("Body calibration checks passed.");
}
