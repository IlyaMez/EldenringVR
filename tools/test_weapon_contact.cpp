#include "weapon_contact.h"
#include <cstdio>
#include <cstdlib>

using namespace ervr;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}
int main() {
    Check(WeaponContactStarts(true, true, 1.2f, 0.f, 0.8f, 0.8f),
          "a blade arc with a stationary wrist must register continuous contact");
    Check(!WeaponContactStarts(true, true, 0.f, 0.f, 0.8f, 0.8f), "resting weapons must not attack");
    Check(!WeaponContactStarts(false, true, 8.f, 2.f, 0.8f, 0.8f), "tracking/menu gates must block contacts");
    Check(WeaponContactStarts(true, false, 4.f, 1.f, 3.f, 0.8f),
          "a hand that reaches minimum speed after the tip threshold must still arm");
    Check(WeaponContactReady(true, true, true, 0, 0.06f, 30.f, 0.012f, false),
          "a visible heavy-weapon contact must not wait through another startup window");
    Check(WeaponContactReady(false, true, true, 0.061, 0.06f, 30.f, 0.012f, false),
          "gated rendered inertia must not charge startup twice");
    Check(!WeaponContactReady(false, false, true, 0.061, 0.06f, 30.f, 0.012f, false),
          "the legacy virtual-only model must preserve configured commitment timing");
    Check(WeaponContactContinues(true, 3.f, 0.25f, 0.9, 0.6f),
          "a long windup must not time out the continuous hitbox before it connects");
    Check(!WeaponContactContinues(false, 3.f, 1.5f, 0.9, 0.6f),
          "the gated model must still enforce its maximum swing time");
    Check(!WeaponContactContinues(true, 0.f, 0.25f, 0.1, 0.6f),
          "a stopped blade must close even a newly opened continuous contact");
    Check(SpentContactEnd(false, 0.3f, 9.f, 0.25f, 0.4f, 5.0, 1.f) == 0,
          "a live contact never ends through the spent-contact rule");
    Check(SpentContactEnd(true, 0.3f, 9.f, 0.25f, 0.4f, 0.1, 1.f) == 1,
          "a spent long-weapon contact must end at a reversal that never reaches the off speed");
    Check(SpentContactEnd(true, 6.f, 9.f, 0.25f, 0.4f, 0.1, 1.f) == 0,
          "a spent contact still mid-stroke keeps waiting for its valley");
    Check(SpentContactEnd(true, 6.f, 9.f, 0.25f, 0.4f, 1.2, 1.f) == 2,
          "a spent contact that never slows (windmilling) must still re-arm");
    Check(SpentContactEnd(true, 0.2f, 0.4f, 0.25f, 0.4f, 0.0, 1.f) == 1,
          "a slow spent contact falls back to the off speed as its valley");
    Check(RearmReady(0.9f, 0.f), "without a valley end the usual contact start applies");
    Check(!RearmReady(2.5f, 3.1f), "a decelerating follow-through must not open a second hit");
    Check(RearmReady(1.0f, 0.3f), "the next swing out of a reversal must arm");
    WeaponContactLifetime contact;
    contact.Begin(); contact.End();
    Check(contact.active && contact.first, "a contact shorter than a game frame must survive until delivery");
    contact.FinishFrame();
    Check(!contact.active && !contact.first, "a delivered short contact must retire exactly once");
    contact.Begin(); contact.End(true);
    Check(!contact.active && !contact.first, "menus/tracking loss must cancel even an undelivered contact");
    contact.Begin(); contact.FinishFrame();
    Check(contact.active && !contact.first, "a continuing contact must retain its identity across frames");
    contact.End();
    Check(!contact.active, "a delivered contact can end immediately");
    const Vec3 feet{2.f, 0.f, 0.f}, top{2.f, 1.7f, 0.f};
    Check(std::fabs(SegmentDistance({0, 1, -1}, {0, 1, 1}, feet, top) - 2.f) < 1e-4f,
          "a blade passing beside a body line is as far as the gap between them");
    Check(SegmentDistance({1, 1, 0}, {3, 1, 0}, feet, top) < 1e-4f, "a blade through the body line touches it");
    Check(std::fabs(SegmentDistance({2, 3, 0}, {2, 4, 0}, feet, top) - 1.3f) < 1e-4f,
          "a blade above the head is measured to the top of the body line");
    Check(std::fabs(SegmentDistance({0, 0.5f, 0}, {0, 0.5f, 0}, feet, top) - 2.f) < 1e-4f, "a zero-length blade is a point");
    std::puts("Weapon contact regression checks passed.");
}
