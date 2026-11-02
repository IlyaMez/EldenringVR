#include "belt_logic.h"
#include <cstdio>
#include <cstdlib>

using namespace ervr;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}
bool Near(const Vec3& a, const Vec3& b, float eps = 1e-4f) { return (a - b).Length() < eps; }

int main() {
    // A pouch slot holds a Goods id; empty reads 0xFFFFFFFF.
    Check(BeltItemValid(0x40000000u | 1234u) && !BeltItemValid(0xFFFFFFFFu) && !BeltItemValid(1234u) && !BeltItemValid(0x40000000u),
          "only a Goods id is an item");
    Check(BeltGoodsId(0x40000000u | 1234u) == 1234u, "the Goods param row is the id without its category");

    // Directions: four different ones, any case or spacing; anything else leaves the old ones.
    {
        uint8_t d[kBeltSlots] = {9, 9, 9, 9};
        Check(BeltParseDirections("up,right,left,down", d) && d[0] == BeltUp && d[1] == BeltRight && d[2] == BeltLeft && d[3] == BeltDown,
              "the game's own order parses");
        Check(BeltParseDirections(" Down , LEFT right;up", d) && d[0] == BeltDown && d[1] == BeltLeft && d[2] == BeltRight && d[3] == BeltUp,
              "case and separators do not matter");
        uint8_t keep[kBeltSlots] = {BeltUp, BeltRight, BeltLeft, BeltDown};
        Check(!BeltParseDirections("up,up,left,down", keep) && !BeltParseDirections("up,right,left", keep) &&
              !BeltParseDirections("up,right,left,down,up", keep) && !BeltParseDirections("north,right,left,down", keep),
              "a repeat, a missing or extra slot, or an unknown word is refused");
        Check(keep[0] == BeltUp && keep[1] == BeltRight && keep[2] == BeltLeft && keep[3] == BeltDown, "a refused list changes nothing");
    }
    {
        float a[kBeltSlots] = {0, 0, 0, 0};
        Check(BeltParseAngles("-30,30,-80,80", a) && a[0] == -30.f && a[1] == 30.f && a[2] == -80.f && a[3] == 80.f, "angles parse");
        float keep[kBeltSlots] = {1, 2, 3, 4};
        Check(!BeltParseAngles("a,b,c,d", keep) && !BeltParseAngles("1,2,3", keep) && !BeltParseAngles("1,2,3,400", keep) &&
              keep[0] == 1.f && keep[3] == 4.f, "a bad angle list changes nothing");
    }

    // The pouch shortcut: Event Action first and alone for the lead, the direction only ever under it, Event
    // Action still down for the tail after the direction lets go.
    {
        BeltChordTiming t;
        BeltChord c;
        c.Start(2, 10.0);
        Check(c.Active() && c.slot == 2, "a started chord is active");
        double firstDir = -1.0, lastDir = -1.0, lastMod = -1.0;
        for (int i = 0; i < 200 && c.Active(); ++i) {
            const double now = 10.0 + i / 90.0;
            const uint8_t b = BeltChordStep(c, now, t);
            Check(!(b & BeltChordDir) || (b & BeltChordMod), "the direction is never pressed without Event Action");
            if (b & BeltChordDir) { if (firstDir < 0.0) firstDir = now; lastDir = now; }
            if (b & BeltChordMod) lastMod = now;
        }
        Check(!c.Active(), "the chord ends");
        Check(firstDir - 10.0 >= t.lead - 1e-9, "Event Action is held alone for the whole lead");
        Check(lastDir - firstDir >= t.press - 0.02, "the direction is held for the press");
        Check(lastMod - lastDir >= t.tail - 0.02, "Event Action outlasts the direction by the tail");
        Check(BeltChordStep(c, 20.0, t) == 0, "an ended chord presses nothing");
    }

    // Where the belt hangs: under the neck, which the eyes pivot around.
    {
        Pose head; head.pos = {0.f, 1.6f, 0.f}; head.valid = true;
        const Vec3 neck = BeltNeck(head);
        Check(Near(neck, {0.f, 1.5f, 0.09f}), "the neck is below and behind the eyes of an upright head");
        Check(Near(BeltSlotPos(neck, 0.f, 0.f, 0.19f, 0.6f), {0.f, 1.0f, -0.10f}), "straight ahead: in front of the spine, Drop below the eyes");
        Check(Near(BeltSlotPos(neck, 0.f, Deg2Rad(90.f), 0.19f, 0.6f), {0.19f, 1.0f, 0.09f}), "+90 degrees is the right hip");
        Check(Near(BeltSlotPos(neck, Deg2Rad(90.f), 0.f, 0.19f, 0.6f), {-0.19f, 1.0f, 0.09f}), "turned left, ahead is -X");
        // Looking down at the belt pitches the head about the neck: the neck, and so the belt, stays put.
        Pose down = head;
        down.rot = Quat::AxisAngle({1.f, 0.f, 0.f}, Deg2Rad(-60.f));
        down.pos = neck + down.rot.Rotate({0.f, kEyeAboveNeck, -kEyeAheadOfNeck});
        Check(Near(BeltNeck(down), neck), "a head pitched down keeps the same neck point");
        Check(std::fabs(WrapPi(down.rot.Yaw())) < 1e-4f, "pitching down does not read as a turn");
        Check(std::fabs(BeltYawForward(0.7f).Dot(BeltYawRight(0.7f))) < 1e-6f &&
              std::fabs(Quat::YawQ(0.7f).Yaw() - 0.7f) < 1e-5f && Near(Quat::YawQ(0.7f).Forward(), BeltYawForward(0.7f)),
              "the belt's yaw is the head's Quat::Yaw convention");
    }

    // The belt turns with the body: still for a glance, dragged by a big turn, settles under a steady head, frozen
    // while reached for.
    {
        const float dz = Deg2Rad(45.f);
        BeltYaw y;
        double now = 0.0;
        y.Update(0.f, now, 0.0, dz, false);
        for (int i = 0; i < 160; ++i) y.Update(0.3f, now += 1.0 / 90.0, 1.0 / 90.0, dz, false);
        Check(std::fabs(y.yaw) < 1e-4f, "a glance of well over a second inside the dead zone leaves the belt");
        for (int i = 0; i < 450; ++i) y.Update(0.3f, now += 1.0 / 90.0, 1.0 / 90.0, dz, false);
        Check(std::fabs(y.yaw - 0.3f) < 0.01f, "a heading held for seconds brings the belt under the head");
        y.Update(1.6f, now += 1.0 / 90.0, 1.0 / 90.0, dz, false);
        Check(std::fabs(y.yaw - (1.6f - dz)) < 1e-4f, "a big turn drags the belt to the edge of the dead zone");
        const float before = y.yaw;
        for (int i = 0; i < 400; ++i) y.Update(-1.0f, now += 1.0 / 90.0, 1.0 / 90.0, dz, true);
        Check(y.yaw == before, "a frozen belt does not move at all");
        y.Update(3.0f, now += 1.0 / 90.0, 1.0 / 90.0, dz, false);
        y.Update(-3.0f, now += 1.0 / 90.0, 1.0 / 90.0, dz, false);
        Check(std::fabs(WrapPi(-3.0f - y.yaw)) <= dz + 1e-4f, "the drag goes the short way round past +-180 degrees");
    }

    // Taking an item: the nearest filled slot in reach that the other hand does not hold.
    {
        const Vec3 pos[kBeltSlots] = {{-0.1f, 1.f, -0.15f}, {0.1f, 1.f, -0.15f}, {-0.19f, 1.f, 0.f}, {0.19f, 1.f, 0.f}};
        bool filled[kBeltSlots] = {true, true, true, true}, taken[kBeltSlots] = {};
        Check(BeltNearest({0.12f, 1.02f, -0.14f}, pos, filled, taken, 0.11f) == 1, "the hand at slot 2 takes slot 2");
        Check(BeltNearest({0.f, 1.3f, 0.f}, pos, filled, taken, 0.11f) == -1, "a hand out of reach takes nothing");
        filled[1] = false;
        Check(BeltNearest({0.12f, 1.02f, -0.14f}, pos, filled, taken, 0.11f) == -1, "an empty slot is never taken");
        filled[1] = true; taken[1] = true;
        Check(BeltNearest({0.12f, 1.02f, -0.14f}, pos, filled, taken, 0.11f) == -1, "nor one the other hand holds");
        taken[1] = false;
        Check(BeltNearest({0.f, 1.f, -0.15f}, pos, filled, taken, 0.11f) >= 0, "between two items the hand gets one of them");
    }

    // What the game made of a use.
    {
        BeltGameState before;
        for (int k = 0; k < kBeltSlots; ++k) { before.pouch[k] = 0x40000000u | (100u + k); before.quantity[k] = 5; }
        before.quickSlot = 2; before.weaponL = 0; before.weaponR = 1; before.spellSlot = 0; before.queuedItem = 0xFFFFFFFFu;
        BeltGameState now = before;
        Check(BeltJudge(before, now, 1, true, false).outcome == BeltOutcome::Pending, "nothing yet: still watching");
        Check(BeltJudge(before, now, 1, false, true).outcome == BeltOutcome::Unconfirmed, "nothing by the end: unconfirmed");
        now.quantity[1] = 4;
        Check(BeltJudge(before, now, 1, true, false).outcome == BeltOutcome::Used, "its count dropped: used");
        now = before; now.pouch[1] = 0xFFFFFFFFu;
        Check(BeltJudge(before, now, 1, true, false).outcome == BeltOutcome::Used, "the last one used empties the slot: used");
        now = before; now.queuedItem = before.pouch[1];
        Check(BeltJudge(before, now, 1, true, false).outcome == BeltOutcome::Used, "the game queued it: used");
        BeltGameState again = before; again.queuedItem = before.pouch[1];
        Check(BeltJudge(again, again, 1, false, true).outcome == BeltOutcome::Unconfirmed,
              "an item already queued before the press proves nothing by staying queued");
        now = before; now.quantity[2] = 4;
        const BeltVerdict other = BeltJudge(before, now, 1, true, false);
        Check(other.outcome == BeltOutcome::OtherSlot && other.otherSlot == 2, "another slot's item went: the directions are off");
        now = before; now.quickSlot = 3;
        const BeltVerdict sw = BeltJudge(before, now, 1, true, false);
        Check(sw.outcome == BeltOutcome::Switched && sw.switched, "a moved quick item: the D-pad arrived as a plain switch");
        Check(BeltJudge(before, now, 1, false, false).outcome == BeltOutcome::Pending, "a switch long after the press is not ours");
        now.quantity[1] = 4;
        Check(BeltJudge(before, now, 1, true, false).outcome == BeltOutcome::Used, "a use wins over anything else");
        BeltGameState empty = before; empty.pouch[3] = 0xFFFFFFFFu; empty.quantity[3] = -1;
        now = empty;
        Check(BeltJudge(empty, now, 1, false, true).outcome == BeltOutcome::Unconfirmed, "an empty slot never reads as used");
    }
    {
        uint8_t d[kBeltSlots] = {BeltUp, BeltRight, BeltLeft, BeltDown};
        BeltSwapDirections(d, 1, 2);   // right was sent for slot 2 and the game used slot 3
        Check(d[0] == BeltUp && d[1] == BeltLeft && d[2] == BeltRight && d[3] == BeltDown, "the two directions trade places");
        BeltSwapDirections(d, 1, 7);
        Check(d[1] == BeltLeft, "an out-of-range slot changes nothing");
    }
    std::puts("Belt regression checks passed.");
}
