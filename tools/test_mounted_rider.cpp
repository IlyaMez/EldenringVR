#include "mounted_rider.h"
#include "pose_frame.h"
#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace ervr;
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
void Near(float a, float b, const char* message, float eps = 0.0001f) {
    Check(std::isfinite(a) && std::fabs(a - b) < eps, message);
}
struct Memory {
    std::array<uint8_t, 0x198> player{}, horse{}, oldHorse{};
    std::array<uint8_t, 0xF0> playerModules{}, horseModules{}, oldModules{};
    std::array<uint8_t, 0x164> playerRide{}, horseRide{}, oldRide{};
    std::array<uint8_t, 0x54> playerNode{}, horseNode{}, oldNode{};
    std::array<uint8_t, 0x1CC98> world{};
    std::array<uint8_t, 0x20> set{}, entries{};
    const void* fail = nullptr;
    static constexpr uint64_t PlayerHandle = 0x1234567810000000ULL;
    static constexpr uint64_t HorseHandle = 0x1234567810100000ULL;
    static constexpr uint64_t OldHandle = 0x1234567810200000ULL;
    template<class T> void Put(uint8_t* a, T value) { std::memcpy(a, &value, sizeof(value)); }
    Memory() {
        Put(player.data() + 8, PlayerHandle); Put(horse.data() + 8, HorseHandle); Put(oldHorse.data() + 8, OldHandle);
        Put(horse.data() + 0x64, int32_t{8000}); Put(oldHorse.data() + 0x64, int32_t{8000});
        Put(player.data() + 0x190, playerModules.data()); Put(horse.data() + 0x190, horseModules.data());
        Put(oldHorse.data() + 0x190, oldModules.data());
        Put(playerModules.data() + 0xE8, playerRide.data()); Put(horseModules.data() + 0xE8, horseRide.data());
        Put(oldModules.data() + 0xE8, oldRide.data());
        Put(playerRide.data() + 8, player.data()); Put(horseRide.data() + 8, horse.data()); Put(oldRide.data() + 8, oldHorse.data());
        Put(playerRide.data() + 0x10, playerNode.data()); Put(horseRide.data() + 0x10, horseNode.data());
        Put(oldRide.data() + 0x10, oldNode.data());
        Put(playerNode.data() + 0x10, player.data()); Put(horseNode.data() + 0x10, horse.data()); Put(oldNode.data() + 0x10, oldHorse.data());
        Put(playerNode.data() + 0x18, HorseHandle); Put(horseNode.data() + 0x18, PlayerHandle);
        Put(oldNode.data() + 0x18, uint64_t{0xFFFFFFFFull});
        Put(playerNode.data() + 0x50, uint32_t{5});
        playerRide[0x163] = 1; horseRide[0x33] = 1;
        Put(world.data() + 0x1CC90, set.data()); Put(set.data() + 0x18, entries.data());
        Put(set.data() + 0x10, uint32_t{2});
        Put(entries.data(), oldHorse.data()); Put(entries.data() + 0x10, horse.data());
    }
    bool Read(const void* a, void* out, size_t n) {
        if (a == fail) return false;
        auto copy = [&](const auto& buffer) {
            const auto begin = reinterpret_cast<uintptr_t>(buffer.data()), at = reinterpret_cast<uintptr_t>(a);
            if (n > buffer.size() || at < begin || at - begin > buffer.size() - n) return false;
            std::memcpy(out, a, n); return true;
        };
        return copy(player) || copy(horse) || copy(oldHorse) || copy(playerModules) || copy(horseModules) ||
               copy(oldModules) || copy(playerRide) || copy(horseRide) || copy(oldRide) || copy(playerNode) ||
               copy(horseNode) || copy(oldNode) || copy(world) || copy(set) || copy(entries);
    }
    TorrentLookup Find(bool withWorld = true) {
        return FindMountedTorrent(player.data(), withWorld ? world.data() : nullptr,
            [&](const void* a, void* out, size_t n) { return Read(a, out, n); });
    }
    uint8_t* Resolve(bool withWorld = true) { return Find(withWorld).mount; }
};

void TheRiddenHorseIsFound() {
    {
        Memory m;
        Check(m.Resolve() == m.horse.data(), "loading on Torrent resolves the paired horse without last_mounted");
        Check(std::strcmp(m.Find().how, "pair handle") == 0, "the pair handle is the identity used when it names the horse");
        m.Put(m.playerRide.data() + 0x18, m.oldHorse.data());
        Check(m.Resolve() == m.horse.data(), "a stale historical horse is skipped in favor of the paired horse");
        m.Put(m.playerRide.data() + 0x18, m.horse.data());
        Check(m.Resolve(false) == m.horse.data(), "the last mount carrying the pair handle works without the world holder");
    }
    {
        Memory m;   // 1.17.2: the mounting flag still set while riding must not hide the horse
        m.playerRide[0x162] = 1;
        Check(m.Resolve() == m.horse.data(), "riding state 5 resolves the horse whatever the mounting flag says");
    }
    {
        Memory m;   // the rider's pair handle unreadable / moved: Torrent's own link back to the rider stands in
        m.Put(m.playerNode.data() + 0x18, uint64_t{0x55});
        Check(m.Resolve() == m.horse.data(), "a horse paired back to this rider is found when the rider's handle names nothing");
        Check(std::strcmp(m.Find().how, "Torrent's pair link") == 0, "the fallback identity is reported for the log");
        m.Put(m.horseNode.data() + 0x18, uint64_t{1});
        Check(!m.Resolve(), "a horse paired with a different rider is never selected");
    }
    {
        Memory m;   // no ride node at all: the mounted flag and the last mount being ridden identify it
        m.Put(m.playerRide.data() + 0x10, static_cast<uint8_t*>(nullptr));
        m.Put(m.horseNode.data() + 0x18, uint64_t{0});
        m.Put(m.playerRide.data() + 0x18, m.horse.data());
        Check(m.Resolve() == m.horse.data(), "without a node the last mount being ridden is the horse");
        m.horseRide[0x33] = 0;
        Check(!m.Resolve(), "a last mount nobody rides is not the current horse");
        m.horseRide[0x33] = 1; m.playerRide[0x163] = 0;
        Check(!m.Resolve(), "without a node the mounted flag must say riding");
    }
    {
        Memory m;
        for (uint32_t state : {3u, 7u}) {
            m.Put(m.playerNode.data() + 0x50, state);
            const TorrentLookup l = m.Find();
            Check(!l.mount && !l.seated, "mounting/dismounting never force visibility or apply riding turns");
        }
        m.Put(m.playerNode.data() + 0x50, uint32_t{6});
        Check(m.Resolve() == m.horse.data(), "a riding state numbered differently still counts while the mounted flag says riding");
        m.playerRide[0x163] = 0;
        m.Put(m.playerNode.data() + 0x50, uint32_t{0});
        Check(!m.Find().seated && !m.Resolve(), "on foot nothing is resolved");
        m.playerRide[0x163] = 1;
        Check(m.Resolve() == m.horse.data(), "an idle node with the mounted flag set (loading on Torrent) is riding");
        m.Put(m.playerNode.data() + 0x50, uint32_t{5});
        m.Put(m.horse.data() + 0x64, int32_t{8002});
        Check(m.Resolve() == m.horse.data(), "1.17.2's Torrent (npc 8002) is the horse its pair handle names");
        m.Put(m.playerNode.data() + 0x18, uint64_t{0x55});
        Check(m.Resolve() == m.horse.data(), "... and the horse paired back to the rider, any Torrent id");
        m.Put(m.horse.data() + 0x64, int32_t{1000});
        Check(!m.Resolve(), "an unrelated summon is never taken on a weaker identity than the pair handle");
        m.Put(m.playerNode.data() + 0x18, Memory::HorseHandle);
        m.Put(m.horse.data() + 0x64, int32_t{8000});
        m.fail = m.horse.data() + 0x64;
        Check(!m.Resolve(), "an unreadable candidate cannot be taken for the horse");
        m.fail = m.playerRide.data() + 0x08;
        const TorrentLookup l = m.Find();
        Check(!l.mount && !l.rideModule, "failed pair reads cannot expose or turn a stale mount");
    }
    {
        Memory m;   // the diagnostic carries what was read
        m.Put(m.playerRide.data() + 0x18, m.oldHorse.data());
        const TorrentLookup l = m.Find();
        Check(l.rideModule && l.node && l.seated && l.state == 5 && l.mounted == 1, "rider-side reads are reported");
        Check(l.rider == Memory::PlayerHandle && l.peer == Memory::HorseHandle, "handles are reported");
        Check(l.lastMounted == m.oldHorse.data() && l.lastNpc == 8000 && l.lastHandle == Memory::OldHandle, "the last mount is reported");
        Check(l.scanned == 2, "the summon set scan stops at the match");
    }
}

void TurnsStayWithTheMountedPair() {
    int first = 0, second = 0;
    MountedTurnQueue queue;
    queue.Add(1.f);
    Near(queue.Take(nullptr, 1), 0.f, "unmounted input is discarded");
    queue.SetMount(&first);
    queue.Add(Deg2Rad(30.f)); queue.Add(Deg2Rad(15.f));
    const float turn = queue.Take(&first, 2);
    Near(turn, Deg2Rad(45.f), "smooth or snap input is accumulated once for the game thread");
    Near(queue.Take(&first, 2), 0.f, "the other character/model hook cannot apply the same turn twice");
    queue.Add(0.2f);
    Near(queue.Take(&first, 2), 0.f, "input arriving between the pair's model updates waits until both can turn");
    Near(queue.Take(&first, 3, false), 0.f, "the second-eye pass never consumes a pending turn");
    Near(queue.Take(&first, 3), 0.2f, "pending input is retained for the next simulation frame");
    Vec3 pivot{5.f, 3.f, -4.f}, position = pivot + Vec3{0.4f, 1.1f, -0.5f};
    Quat horse = Quat::FromEulerYXZ(0.6f, 0.15f, -0.2f), rider = horse;
    const Vec3 beforeOffset = horse.Conj().Rotate(position - pivot);
    const Vec3 beforeForward = horse.Forward();
    TurnRiderAboutMount(pivot, turn, horse, position, rider);
    const Vec3 afterOffset = horse.Conj().Rotate(position - pivot);
    Near((afterOffset - beforeOffset).Length(), 0.f, "rider stays at the same local saddle position");
    Near(position.y, 4.1f, "turning does not change riding height");
    Near((horse.Forward() - Quat::YawQ(turn).Rotate(beforeForward)).Length(), 0.f,
         "Torrent turns by the right-stick angle while preserving native tilt");
    Near((horse.Forward() - rider.Forward()).Length(), 0.f, "rider and Torrent keep the same facing");
    queue.Add(0.5f); queue.SetMount(nullptr); queue.SetMount(&first);
    Near(queue.Take(&first, 4), 0.f, "dismount and remount do not replay old turn input");
    queue.Add(0.5f); queue.SetMount(&second);
    Near(queue.Take(&second, 5), 0.f, "a replacement horse does not inherit a queued turn");
    queue.Add(std::numeric_limits<float>::quiet_NaN());
    Near(queue.Take(&second, 6), 0.f, "invalid input never reaches physics transforms");
}

void VisibilityPreservesOtherFlags() {
    MountedVisibility hidden{0xA1, 0x12, 0x100000A0, 0.f, 0.2f, 0.f};
    const auto visible = VisibleMountedTorrent(hidden);
    Check(visible.frustum == 0xB1 && visible.render == 0x1A && visible.display == 0x100000A1,
          "restoring Torrent draw state preserves shadow and unrelated character flags");
    Near(visible.opacity, 1.f, "active horse opacity is restored");
    Near(visible.previousOpacity, 1.f, "previous-frame opacity cannot keep the horse invisible");
    Near(visible.baseOpacity, 1.f, "base character opacity is restored as well as material dithering");
}

void SeatedPoseIsASettledSeat() {
    int rider, other;
    MountedSeatFrame seat;
    seat.Observe(&rider, true, 10, true);
    Check(seat.Seated(&rider) && !seat.Seated(&other), "the seat belongs to the rider that sat down");
    Check(!seat.CanCapture(10.2, true, true), "mount animation must settle before saving a seated pose");
    Check(!seat.CanCapture(11, false, true), "our previous IK output is not a fresh mounted pose");
    Check(!seat.CanCapture(11, true, false), "second eye cannot capture a different seated pose");
    Check(seat.CanCapture(11, true, true), "settled native seated pose can be captured");
    seat.Observe(&rider, false, 12, false);
    Check(seat.Seated(&rider), "the second eye cannot end the seat");
    seat.Observe(&rider, false, 12, true);
    Check(!seat.Seated(&rider) && !seat.CanCapture(13, true, true), "dismounting releases the seat");
    seat.Observe(&rider, true, 14, true);
    Check(!seat.CanCapture(14.3, true, true) && seat.CanCapture(14.6, true, true), "a new ride waits for its own mount animation");

    SeatSettle settle;
    Check(settle.Step(0.f, 0.1f, 1.0, 0.3) == SeatStep::NewCandidate, "the first fresh pose is only a candidate");
    Check(settle.Step(0.05f, 0.1f, 1.2, 0.3) == SeatStep::Wait, "a held pose waits out the settle time");
    Check(settle.Step(0.5f, 0.1f, 1.25, 0.3) == SeatStep::NewCandidate, "a gallop frame that moved replaces the candidate");
    Check(settle.Step(0.02f, 0.1f, 1.5, 0.3) == SeatStep::Wait, "still waiting after the restart");
    Check(settle.Step(0.02f, 0.1f, 1.56, 0.3) == SeatStep::Take, "a seat held for the settle time is taken");
    Check(settle.Step(0.f, 0.1f, 1.6, 0.3) == SeatStep::NewCandidate, "after a take the next pose starts over");
}

void SeatedBodyStaysUpright() {
    const Vec3 pelvis{0.f, 0.95f, 0.05f}, head{0.f, 1.7f, -0.02f};
    for (float yaw : {0.f, 0.9f, -2.4f}) {
        for (float pitch : {0.f, 0.35f, -0.3f}) {
            const Vec3 modelP{3.f, 10.f, -7.f};
            const Quat modelQ = (Quat::YawQ(yaw) * Quat::AxisAngle({1, 0, 0}, pitch)).Normalized();
            Vec3 pos; Quat up;
            UprightSeatRoot(modelP, modelQ, pelvis, pos, up);
            Near(((pos + up.Rotate(pelvis)) - (modelP + modelQ.Rotate(pelvis))).Length(), 0.f, "the hips stay in the saddle");
            const Vec3 f = up.Rotate({0, 0, -1});
            Near(f.y, 0.f, "no pitch reaches the seated body");
            Near(WrapPi(std::atan2(f.x, f.z) + 3.14159265f - yaw), 0.f, "the seated body keeps the horse's heading", 0.001f);
            const Vec3 headW = pos + up.Rotate(head), pelvisW = pos + up.Rotate(pelvis);
            Near(headW.y - pelvisW.y, head.y - pelvis.y, "the head stays upright over the hips however Torrent pitches", 0.001f);
        }
    }
}

void CameraRidesTheHorseNotTheBounce() {
    RideCameraStabilizer stab;
    const Quat yaw = Quat::YawQ(0.7f);
    Vec3 horse{0, 0, 0};
    const Vec3 seat{0.1f, 2.3f, -0.2f};
    Vec3 out = stab.Apply(horse + yaw.Rotate(seat), horse, yaw, 0.0, true, 0.2f, 0.12f);
    Near((out - (horse + yaw.Rotate(seat))).Length(), 0.f, "the first frame on the horse sits exactly on the anchor");
    float maxDev = 0.f;
    double t = 0;
    for (int i = 1; i <= 300; ++i) {   // gallop: 14 m/s forward, the saddle bobbing 6 cm at 2.2 Hz
        t = i / 60.0;
        horse = horse + yaw.Rotate(Vec3{0, 0, 14.f / 60.f});
        const Vec3 anchor = horse + yaw.Rotate(seat + Vec3{0, 0.06f * std::sin(6.2831853f * 2.2f * (float)t), 0});
        out = stab.Apply(anchor, horse, yaw, t, true, 0.2f, 0.12f);
        const Vec3 local = yaw.Conj().Rotate(out - horse);
        if (i > 120) maxDev = std::fmax(maxDev, std::fabs(local.y - seat.y));
        Check((out - anchor).Length() <= 0.1201f, "the view never strays further than the bound from the rider");
    }
    Check(maxDev < 0.035f, "most of the gallop bob is taken out of the view");
    Near(yaw.Conj().Rotate(out - horse).z, seat.z, "the horse's travel passes straight through", 0.002f);
    const Quat turned = Quat::YawQ(1.6f);
    out = stab.Apply(horse + turned.Rotate(seat), horse, turned, t + 1.0 / 60.0, true, 0.2f, 0.12f);
    Check((out - (horse + turned.Rotate(seat))).Length() < 0.04f, "a turn of the horse swings the view with it at once");
    const Vec3 before = stab.local;
    stab.Apply(horse + turned.Rotate(seat + Vec3{0, 0.1f, 0}), horse, turned, t + 2.0 / 60.0, false, 0.2f, 0.12f);
    Near((stab.local - before).Length(), 0.f, "only the final camera write of a frame moves the smoothing");
    const Vec3 far{500, 0, 0};
    out = stab.Apply(far + turned.Rotate(seat), far, turned, t + 3.0 / 60.0, true, 0.2f, 0.12f);
    Check((out - (far + turned.Rotate(seat))).Length() < 0.04f, "a world shift moving horse and rider together changes nothing");
    const Vec3 reseat = far + turned.Rotate(seat + Vec3{0, 0, 3.f});
    out = stab.Apply(reseat, far, turned, t + 4.0 / 60.0, true, 0.2f, 0.12f);
    Near((out - reseat).Length(), 0.f, "a rider placed somewhere else on the horse restarts on the new anchor");
}

void PhysicalTurnsFollowTheHead() {
    RideHeadFollow follow;
    Near(follow.Step(0.3f, 0.0, 1, 0.f, 0.f), 0.f, "the first sample only sets the reference");
    Near(follow.Step(0.5f, 0.016, 2, 0.f, 0.f), 0.2f, "with no smoothing the pair turns with the head 1:1");
    Near(follow.Step(0.9f, 0.017, 2, 0.f, 0.f), 0.f, "a second call in the same frame turns nothing");
    float total = 0.f;
    for (uint32_t f = 3; f < 200; ++f) total += follow.Step(1.5f, 0.016 * f, f, 0.f, 0.25f);
    Near(total, 1.0f, "a held physical turn is followed all the way", 0.002f);
    RideHeadFollow dz;
    dz.Step(0.f, 0.0, 1, Deg2Rad(20.f), 0.f);
    Near(dz.Step(Deg2Rad(15.f), 0.016, 2, Deg2Rad(20.f), 0.f), 0.f, "a glance inside the deadzone leaves the horse alone");
    Near(dz.Step(Deg2Rad(30.f), 0.032, 3, Deg2Rad(20.f), 0.f), Deg2Rad(10.f), "only the part past the deadzone turns the pair");
    RideHeadFollow look;   // free look: 45 degrees either side leave the horse alone, the excess turns it
    const float fl = Deg2Rad(45.f);
    look.Step(0.f, 0.0, 1, fl, 0.f);
    float looked = 0.f;
    for (float yaw : {20.f, 44.f, -30.f, -45.f, 10.f}) looked += look.Step(Deg2Rad(yaw), 0.0, (uint32_t)(yaw + 100.f), fl, 0.f);
    Near(looked, 0.f, "glancing around within the free look never turns the horse");
    Near(look.followed, 0.f, "... nor moves the forward direction");
    Near(look.Step(Deg2Rad(90.f), 0.1, 300, fl, 0.f), Deg2Rad(45.f), "turning 90 degrees turns the horse by the 45 past the free look");
    Near(look.Step(Deg2Rad(50.f), 0.2, 301, fl, 0.f), 0.f, "looking back inside the new free look keeps the new heading");
    Near(look.followed, Deg2Rad(45.f), "forward moved with the turn");
    RideHeadFollow wrap;
    wrap.Step(3.1f, 0.0, 1, 0.f, 0.f);
    Near(wrap.Step(-3.1f, 0.016, 2, 0.f, 0.f), 6.2831853f - 6.2f, "turning through the back takes the short way", 0.001f);
    Near(wrap.Step(std::numeric_limits<float>::quiet_NaN(), 0.03, 3, 0.f, 0.f), 0.f, "lost tracking turns nothing");
}

void SecondEyeDrawsTheRiderWhereTheFirstDid() {
    RiderPassHold hold;
    const Quat yaw = Quat::YawQ(-0.8f);
    const Vec3 pass1{10, 2, 5}, pass2 = pass1 + yaw.Rotate(Vec3{0.02f, -0.05f, 0.28f});
    const Vec3 headLocal{0.f, 0.75f, 0.05f};
    Check(!hold.Held(7), "nothing is held before pass 1 finished");
    hold.Pass1(pass1, yaw, 7);
    Check(hold.Held(7) && !hold.Held(8), "the hold belongs to its own frame");
    Near((hold.Render(pass1, yaw, 7) - pass1).Length(), 0.f, "with nothing learned yet the current matrix is the guess");
    const Vec3 d = hold.Pass2(pass2, yaw, 7);
    Near((d - (pass2 - pass1)).Length(), 0.f, "pass 2's re-placement is measured");
    Near((hold.Render(pass2, yaw, 7) - pass2).Length(), 0.f, "after pass 2's rebuild its real matrix is used");
    // The body solved in the pass-1 placement and rebased into the pass-2 matrix lands where pass 1 drew it.
    Vec3 t{}; Quat q{};
    RebasePoseRoot(pass1, yaw, pass2, yaw, t, q);
    Near(((pass2 + yaw.Rotate(t + q.Rotate(headLocal))) - (pass1 + yaw.Rotate(headLocal))).Length(), 0.f,
         "the second eye draws the rider exactly where the camera was placed");
    // Next frame: before pass 2 rebuilds, the learned re-placement is the prediction.
    const Vec3 next1 = pass1 + yaw.Rotate(Vec3{0, 0, 0.28f});
    hold.Pass1(next1, yaw, 8);
    const Vec3 guess = hold.Render(next1, yaw, 8);
    Check((guess - (next1 + (pass2 - pass1))).Length() < 0.001f, "a steady gallop's re-placement is predicted from the last frame");
    Near((hold.Pass2(pass2, yaw, 6) - Vec3{}).Length(), 0.f, "a stale pass 2 measures nothing");
    hold.Pass1(next1, yaw, 9, 5.0);
    Check(hold.Held(9, 5.05), "pass 2 right after its pass 1 holds that pass's placement");
    Check(!hold.Held(9, 5.3), "a pass 1 that rebuilt no rider leaves no hold for a later pass 2");
    hold.Reset();
    Check(!hold.Held(8), "a reset drops the hold");
}

int main() {
    TheRiddenHorseIsFound(); TurnsStayWithTheMountedPair(); VisibilityPreservesOtherFlags();
    SeatedPoseIsASettledSeat(); SeatedBodyStaysUpright(); CameraRidesTheHorseNotTheBounce();
    PhysicalTurnsFollowTheHead(); SecondEyeDrawsTheRiderWhereTheFirstDid();
    std::puts("Mounted rider resolution, seat, camera, turning and second-eye checks passed.");
}
