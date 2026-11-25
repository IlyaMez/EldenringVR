// A landed hit, felt (hit_feedback.h): how hard it was, the impact the controller plays, the blade's bite.
#include "hit_feedback.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
using namespace ervr;

void Check(bool ok, const char* behavior) {
    if (!ok) { std::fprintf(stderr, "FAILED: %s\n", behavior); std::exit(1); }
}
bool Near(float a, float b, float eps = 1e-4f) { return std::isfinite(a) && std::fabs(a - b) <= eps; }
float AngleBetween(const Quat& a, const Quat& b) {
    const float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.f * std::acos(d > 1.f ? 1.f : d);
}

void Strength() {
    Check(Near(HitStrength(0.25f, 0.25f, 1.f), 0.f), "the slowest contact is the softest hit");
    Check(Near(HitStrength(1.f, 0.25f, 1.f), 1.f), "a vanilla hit is a full one");
    Check(Near(HitStrength(1.2f, 0.25f, 1.f), 1.f), "a hit past vanilla stays full");
    Check(Near(HitStrength(0.625f, 0.25f, 1.f), 0.5f), "half way between the floor and a vanilla hit is half");
    Check(Near(HitStrength(0.5f, 0.25f, 0.5f), 1.f), "a lowered damage setting still feels a clean hit as one");
    Check(Near(HitStrength(0.f, 0.25f, 1.f), 0.f), "nothing below the floor");
    Check(std::isfinite(HitStrength(1.f, 1.f, 0.f)), "degenerate settings stay finite");
}

void Impact() {
    const HitHaptic soft = HitHapticFor(0.1f, 5.f), hard = HitHapticFor(1.f, 5.f), heavy = HitHapticFor(1.f, 25.f);
    Check(hard.crackAmp > soft.crackAmp && hard.tailAmp > soft.tailAmp, "a harder hit cracks and thuds harder");
    Check(hard.tailSec > soft.tailSec, "a harder hit thuds longer");
    Check(heavy.tailSec > hard.tailSec, "a heavier weapon thuds longer");
    Check(soft.crackAmp >= 0.5f, "the softest hit still cracks noticeably");
    Check(hard.crackAmp <= 1.f && heavy.tailAmp <= 1.f, "amplitudes stay in range");
    Check(Near(HitHapticAmp(hard, 0.f), hard.crackAmp), "the impact starts with the crack");
    float prev = HitHapticAmp(hard, hard.crackSec);
    Check(prev <= hard.crackAmp, "the thud starts no harder than the crack");
    for (float t = hard.crackSec; t < hard.crackSec + hard.tailSec; t += 0.002f) {
        const float a = HitHapticAmp(hard, t);
        Check(a >= 0.f && a <= prev + 1e-6f, "the thud dies away");
        prev = a;
    }
    Check(prev < 0.02f, "the thud ends near silent - no step at the end");
    Check(HitHapticAmp(hard, hard.crackSec + hard.tailSec + 0.001f) < 0.f, "then the impact is over");
    Check(hard.crackSec + hard.tailSec < 0.4f, "a full hit with the heaviest weapon is still short");
}

void Bite() {
    Check(Near(BiteHold(0.f, 0.12f, 0.7f), 0.7f), "at the hit the blade gives up the full hold");
    Check(Near(BiteHold(0.12f, 0.12f, 0.7f), 0.f) && Near(BiteHold(0.5f, 0.12f, 0.7f), 0.f), "then it is back in the hand");
    Check(BiteHold(0.01f, 0.12f, 0.7f) > 0.68f, "it holds a moment before letting go");
    Check(BiteHold(0.115f, 0.12f, 0.7f) < 0.01f, "and arrives without a jerk");
    float prev = 1.f;
    for (float t = 0.f; t < 0.12f; t += 0.001f) {
        const float h = BiteHold(t, 0.12f, 0.7f);
        Check(h <= prev + 1e-6f, "the bite only ever lets go");
        prev = h;
    }
    Check(Near(BiteHold(0.f, 0.f, 0.7f), 0.f) && Near(BiteHold(0.f, 0.12f, 0.f), 0.f), "0 seconds or 0 hold = no bite");
    Check(Near(BiteHold(-0.01f, 0.12f, 0.7f), 0.f), "nothing before the hit");
    float fullS = 0.f, fullH = 0.f, softS = 0.f, softH = 0.f;
    BiteFor(1.f, 0.12f, 0.7f, &fullS, &fullH);
    BiteFor(0.1f, 0.12f, 0.7f, &softS, &softH);
    Check(Near(fullS, 0.12f) && Near(fullH, 0.7f), "a full hit bites as set");
    Check(softS < fullS && softH < 0.1f, "a soft hit barely bites");
    BiteFor(1.f, 0.f, 0.7f, &fullS, &fullH);
    Check(fullS == 0.f, "HitBiteSeconds=0 turns the bite off");
}

void Slerp() {
    const Quat a = Quat::AxisAngle({0, 1, 0}, 0.f), b = Quat::AxisAngle({0, 1, 0}, 1.f);
    Check(AngleBetween(QuatSlerp(a, b, 0.f), a) < 1e-3f, "t=0 is the first");
    Check(AngleBetween(QuatSlerp(a, b, 1.f), b) < 1e-3f, "t=1 is the second");
    Check(Near(AngleBetween(QuatSlerp(a, b, 0.25f), a), 0.25f, 1e-3f), "a quarter of the way is a quarter of the angle");
    const Quat negB{-b.x, -b.y, -b.z, -b.w};
    Check(Near(AngleBetween(QuatSlerp(a, negB, 0.5f), a), 0.5f, 1e-3f), "the short way round, whatever the sign");
    const Quat c = Quat::AxisAngle({1, 0, 0}, 1e-5f);
    Check(AngleBetween(QuatSlerp(a, c, 0.5f), a) < 1e-3f, "nearly equal orientations stay finite");
}
}  // namespace

int main() {
    Strength();
    Impact();
    Bite();
    Slerp();
    std::puts("hit feedback checks passed.");
    return 0;
}
