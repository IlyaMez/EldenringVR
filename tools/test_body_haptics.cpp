#include "body_haptics.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace ervr;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}
constexpr float kPi = 3.14159265f;

BodyState Live() { BodyState s; s.live = true; return s; }
BodyEvent Ev(BodyKind kind, int hand = -1, float strength = 1, float azimuth = 0, float height = .4f, bool placed = true) {
    BodyEvent e; e.kind = kind; e.hand = (int8_t)hand; e.strength = strength; e.azimuth = azimuth; e.height = height; e.placed = placed;
    return e;
}
// The strongest vest motor of a frame, -1 for none.
int Strongest(const BodyFrame& f) {
    int best = -1; float most = 0;
    for (unsigned m = 0; m < 32; ++m) if (f.At(BodyDevice::Vest, m) > most) { most = f.At(BodyDevice::Vest, m); best = (int)m; }
    return best;
}
float Peak(const BodyFrame& f, BodyDevice d) {
    float most = 0;
    for (unsigned m = 0; m < BodyMotorCount(d); ++m) most = std::fmax(most, f.At(d, m));
    return most;
}
// One event alone, `t` seconds in.
BodyFrame Alone(const BodyEvent& e, double t, BodyState s = Live()) {
    BodyMixer mx; mx.Set(s); mx.Post(e, 10.0);
    mx.Render(10.0);
    return mx.Render(10.0 + t);
}

int main() {
    // ---- the vest's layout ----------------------------------------------------------------------
    Check(BodyVestMotor(false, 0, 0) == 0 && BodyVestMotor(true, 3, 3) == 31 && BodyVestMotor(true, 0, 0) == 16, "vest motor index: front 0-15, back 16-31, row by row");
    Check(BodyVestAzimuth(BodyVestMotor(false, 1, 0)) < 0 && BodyVestAzimuth(BodyVestMotor(false, 1, 3)) > 0, "the front's column 0 is on the wearer's left");
    Check(std::fabs(BodyVestAzimuth(BodyVestMotor(true, 1, 1))) > kPi / 2 && BodyVestAzimuth(BodyVestMotor(true, 1, 3)) > 0, "the back faces behind, its column 3 on the wearer's right");
    Check(BodyVestHeight(BodyVestMotor(false, 0, 2)) == 0.f && BodyVestHeight(BodyVestMotor(true, 3, 2)) == 1.f, "rows run from the shoulders (0) to the waist (1)");

    // ---- blows taken land on the side they came from -----------------------------------------------
    const int ahead = Strongest(Alone(Ev(BodyKind::Hit, -1, .3f, 0.f), .03));
    const int right = Strongest(Alone(Ev(BodyKind::Hit, -1, .3f, kPi / 2), .03));
    const int left = Strongest(Alone(Ev(BodyKind::Hit, -1, .3f, -kPi / 2), .03));
    const int behind = Strongest(Alone(Ev(BodyKind::Hit, -1, .3f, kPi), .03));
    Check(ahead >= 0 && ahead < 16, "a blow from ahead lands on the front");
    Check(behind >= 16, "a blow from behind lands on the back");
    Check(right >= 0 && BodyVestAzimuth(right) > .5f, "a blow from the right lands on the right flank");
    Check(left >= 0 && BodyVestAzimuth(left) < -.5f, "a blow from the left lands on the left flank");
    {
        const BodyFrame f = Alone(Ev(BodyKind::Hit, -1, .3f, kPi / 2), .03);
        int on = 0; for (unsigned m = 0; m < 32; ++m) on += f.At(BodyDevice::Vest, m) > 0;
        Check(on > 0 && on <= 10, "a blow is a spot of a few motors, not half the vest");
    }
    {
        const BodyFrame high = Alone(Ev(BodyKind::Hit, -1, .3f, 0.f, BodyHitHeight(4.f, 2.f)), .03);
        const BodyFrame low = Alone(Ev(BodyKind::Hit, -1, .3f, 0.f, BodyHitHeight(-3.f, 2.f)), .03);
        Check(BodyVestHeight(Strongest(high)) < BodyVestHeight(Strongest(low)), "a blow from above lands higher than one from below");
        Check(BodyHitHeight(0.f, 3.f) == .4f && BodyHitHeight(5.f, 1.f) < .4f && BodyHitHeight(-5.f, 1.f) > .4f, "hit height: level at the chest");
    }
    {
        const BodyFrame f = Alone(Ev(BodyKind::Hit, -1, .3f, 0.f, .4f, false), .03);
        Check(f.At(BodyDevice::Vest, BodyVestMotor(false, 1, 1)) > 0 && f.At(BodyDevice::Vest, BodyVestMotor(true, 1, 1)) > 0, "an unplaced blow plays the chest and the back");
        Check(Peak(f, BodyDevice::Head) == 0.f, "a light blow does not ring in the head");
        Check(Peak(Alone(Ev(BodyKind::Hit, -1, 1.f, 0.f), .02), BodyDevice::Head) > 0.f, "a heavy blow rings in the head");
    }

    // ---- blows given: in the arm that gave them --------------------------------------------------------
    {
        const BodyFrame f = Alone(Ev(BodyKind::Strike, 1, .8f), .03);
        Check(Peak(f, BodyDevice::RightArm) > .3f && Peak(f, BodyDevice::LeftArm) == 0.f, "a right-hand strike plays the right sleeve alone");
        Check(f.At(BodyDevice::Vest, BodyVestMotor(false, 0, 3)) > 0 && f.At(BodyDevice::Vest, BodyVestMotor(false, 0, 0)) == 0.f, "... and comes back through the right shoulder");
        const BodyFrame both = Alone(Ev(BodyKind::Strike, -1, .8f), .03);
        Check(Peak(both, BodyDevice::RightArm) > 0 && Peak(both, BodyDevice::LeftArm) > 0, "a two-handed strike plays both sleeves");
        Check(Peak(Alone(Ev(BodyKind::Strike, 1, 1.f), .03), BodyDevice::RightArm) > Peak(Alone(Ev(BodyKind::Strike, 1, .1f), .03), BodyDevice::RightArm), "a better swing lands harder");
        Check(Peak(Alone(Ev(BodyKind::Block, 0), .03), BodyDevice::LeftArm) > 0 && Peak(Alone(Ev(BodyKind::Deflect, 0), .03), BodyDevice::LeftArm) > Peak(Alone(Ev(BodyKind::Block, 0, .5f), .03), BodyDevice::LeftArm), "a deflect is sharper than a block in the shield arm");
        Check(Peak(Alone(Ev(BodyKind::Cast, 0), .04), BodyDevice::LeftArm) > 0 && Peak(Alone(Ev(BodyKind::Cast, 0), .04), BodyDevice::RightArm) == 0.f, "a cast leaves the hand that cast it");
    }

    // ---- what plays when ----------------------------------------------------------------------------
    {
        BodyMixer mx; BodyState off; mx.Set(off);
        mx.Post(Ev(BodyKind::Hit), 1.0);
        Check(mx.Playing() == 0 && Strongest(mx.Render(1.03)) < 0, "out of the world nothing plays");
        mx.Post(Ev(BodyKind::Hello), 1.1);
        Check(Strongest(mx.Render(1.13)) >= 0, "... but the hello that says the devices were found");
    }
    {
        BodyMixer mx; mx.Set(Live());
        for (unsigned k = 0; k < kBodyKinds; ++k) mx.Post(Ev((BodyKind)k, (int)(k % 3) - 1), 1.0);
        bool finite = true; float most = 0;
        for (double t = 1.0; t < 3.5; t += .01) {
            const BodyFrame f = mx.Render(t);
            for (unsigned d = 0; d < kBodyDevices; ++d)
                for (unsigned m = 0; m < BodyMotorCount((BodyDevice)d); ++m) { const float v = f.motors[d][m]; finite = finite && std::isfinite(v) && v >= 0; most = std::fmax(most, v); }
        }
        Check(finite && most <= 1.f, "every pattern stays within 0..1, all of them at once");
        Check(mx.Playing() == 0, "every pattern ends");
        Check(BodyKindDuration(BodyKind::Death) > BodyKindDuration(BodyKind::Strike), "death lasts longer than a blow");
    }
    {
        BodyState s = Live(); s.groups = kBodyAllGroups & ~(1u << (unsigned)BodyGroup::Combat);
        Check(Strongest(Alone(Ev(BodyKind::Strike, 1), .03, s)) < 0 && Peak(Alone(Ev(BodyKind::Strike, 1), .03, s), BodyDevice::RightArm) == 0.f, "Combat=false silences strikes");
        Check(Strongest(Alone(Ev(BodyKind::Hit), .03, s)) >= 0, "... and leaves blows taken");
        BodyMixer mx; mx.Set(Live()); mx.Post(Ev(BodyKind::Death), 1.0);
        Check(Strongest(mx.Render(1.1)) >= 0, "death plays");
        BodyState noDamage = Live(); noDamage.groups &= ~(1u << (unsigned)BodyGroup::Damage);
        mx.Set(noDamage);
        Check(Strongest(mx.Render(1.2)) < 0 && mx.Playing() == 0, "a group switched off stops what it was playing at once");
    }

    // ---- what goes on: rumble, the heart, the wind ---------------------------------------------------
    {
        BodyMixer mx; BodyState s = Live(); s.rumble = {1.f, 0.f}; mx.Set(s);
        BodyFrame f; for (double t = 1.0; t < 1.5; t += .04) f = mx.Render(t);
        const float waist = f.At(BodyDevice::Vest, BodyVestMotor(false, 3, 1)), shoulders = f.At(BodyDevice::Vest, BodyVestMotor(false, 0, 1));
        Check(waist > 0 && waist <= .26f && waist > shoulders, "the heavy rumble motor plays round the waist, at most a quarter");
        s.rumble = {0.f, 1.f}; mx.Set(s);
        for (double t = 1.5; t < 2.0; t += .04) f = mx.Render(t);
        Check(f.At(BodyDevice::Vest, BodyVestMotor(true, 0, 1)) > f.At(BodyDevice::Vest, BodyVestMotor(true, 3, 1)), "the light one at the shoulders");
        mx.Post(Ev(BodyKind::Hit, -1, .3f, kPi / 2), 2.0);
        for (double t = 2.0; t < 2.15; t += .04) f = mx.Render(t);
        Check(f.At(BodyDevice::Vest, BodyVestMotor(false, 0, 0)) < .25f * .5f, "rumble ducks under a blow's own pattern");
        s.groups &= ~(1u << (unsigned)BodyGroup::Rumble); s.rumble = {1.f, 1.f}; mx.Set(s);
        for (double t = 2.5; t < 3.0; t += .04) f = mx.Render(t);
        Check(Strongest(f) < 0, "Rumble=false: no rumble");
    }
    {
        BodyMixer mx; BodyState s = Live(); s.health = .15f; mx.Set(s);
        float beat = 0; for (double t = 1.0; t < 2.5; t += .01) beat = std::fmax(beat, mx.Render(t).At(BodyDevice::Vest, BodyVestMotor(false, 0, 1)));
        Check(beat > .1f && beat < .45f, "the heart races at low HP, high on the chest's left");
        s.health = .5f; mx.Set(s);
        float none = 0; for (double t = 3.0; t < 4.5; t += .01) none = std::fmax(none, Peak(mx.Render(t), BodyDevice::Vest));
        Check(none == 0.f, "no heart above 30% HP");
        s.health = 0.f; mx.Set(s);
        for (double t = 5.0; t < 6.5; t += .01) none = std::fmax(none, Peak(mx.Render(t), BodyDevice::Vest));
        Check(none == 0.f, "no heart once dead");
    }
    {
        BodyMixer mx; BodyState s = Live(); s.airborne = true; s.velocity = {0, -30, 0}; mx.Set(s);
        BodyFrame f; for (double t = 1.0; t < 2.5; t += .04) f = mx.Render(t);
        Check(f.At(BodyDevice::Vest, BodyVestMotor(false, 3, 1)) > f.At(BodyDevice::Vest, BodyVestMotor(false, 0, 1)) && Peak(f, BodyDevice::Vest) < .2f, "a fall's wind blows up from the waist, faintly");
        s.velocity = {0, -10, 0}; mx.Set(s);
        for (double t = 2.5; t < 4.5; t += .04) f = mx.Render(t);
        Check(Strongest(f) < 0, "no wind under 15 m/s");
    }

    // ---- landings and the torso -------------------------------------------------------------------------
    {
        BodyLandingWatch w; float landed = 0;
        for (int i = 0; i < 30; ++i) landed = std::fmax(landed, w.Update(1.f / 60, true, 12.f));
        landed = std::fmax(landed, w.Update(1.f / 60, false, 0.f));
        Check(landed > .15f && landed < 1.f, "a half-second fall at 12 m/s lands in between");
        BodyLandingWatch hop; float h = 0;
        for (int i = 0; i < 6; ++i) h = std::fmax(h, hop.Update(1.f / 60, true, 6.f));
        h = std::fmax(h, hop.Update(1.f / 60, false, 0.f));
        Check(h == 0.f, "a hop of a moment is no fall");
        const BodyFrame soft = Alone(Ev(BodyKind::Landing, -1, .3f), .02), hard = Alone(Ev(BodyKind::Landing, -1, 1.f), .12);
        Check(soft.At(BodyDevice::Vest, BodyVestMotor(false, 0, 1)) == 0.f && soft.At(BodyDevice::Vest, BodyVestMotor(false, 3, 1)) > 0, "a soft landing plays the waist's row alone");
        Check(hard.At(BodyDevice::Vest, BodyVestMotor(false, 0, 1)) > 0, "a hard one goes up to the shoulders");
    }
    {
        BodyFacing f;
        Check(std::fabs(f.Update(.3f, 0.f) - .3f) < 1e-5f, "the torso starts where the head looks");
        float y = f.Update(.6f, .016f);
        Check(std::fabs(y - .3f) < .01f, "a look within the dead zone leaves the torso (but for a slow drift)");
        for (int i = 0; i < 120; ++i) y = f.Update(2.f, .016f);
        Check(y > 1.3f, "a look past the shoulder turns the torso after it");
        BodyFacing w; w.Update(3.f, 0.f);
        for (int i = 0; i < 120; ++i) y = w.Update(-3.f, .016f);
        Check(std::fabs(WrapPi(y + 3.f)) < .7f, "the torso turns the short way round across +-pi");
    }
    std::puts("Body haptics regression checks passed.");
}
