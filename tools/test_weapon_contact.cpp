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
    std::puts("Weapon contact regression checks passed.");
}
