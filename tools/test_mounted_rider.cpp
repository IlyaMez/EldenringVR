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
void Near(float a, float b, const char* message) {
    Check(std::isfinite(a) && std::fabs(a - b) < 0.0001f, message);
}
struct Memory {
    std::array<uint8_t, 0x198> player{}, horse{}, oldHorse{};
    std::array<uint8_t, 0xF0> playerModules{}, horseModules{};
    std::array<uint8_t, 0x164> playerRide{}, horseRide{};
    std::array<uint8_t, 0x54> playerNode{}, horseNode{};
    std::array<uint8_t, 0x1CC98> world{};
    std::array<uint8_t, 0x20> set{}, entries{};
    const void* fail = nullptr;
    static constexpr uint64_t PlayerHandle = 0x1234567810000000ULL;
    static constexpr uint64_t HorseHandle = 0x1234567810100000ULL;
    template<class T> void Put(uint8_t* a, T value) { std::memcpy(a, &value, sizeof(value)); }
    Memory() {
        Put(player.data() + 8, PlayerHandle); Put(horse.data() + 8, HorseHandle);
        Put(horse.data() + 0x64, int32_t{8000}); Put(oldHorse.data() + 0x64, int32_t{8000});
        Put(player.data() + 0x190, playerModules.data()); Put(horse.data() + 0x190, horseModules.data());
        Put(playerModules.data() + 0xE8, playerRide.data()); Put(horseModules.data() + 0xE8, horseRide.data());
        Put(playerRide.data() + 8, player.data()); Put(horseRide.data() + 8, horse.data());
        Put(playerRide.data() + 0x10, playerNode.data()); Put(horseRide.data() + 0x10, horseNode.data());
        Put(playerNode.data() + 0x10, player.data()); Put(horseNode.data() + 0x10, horse.data());
        Put(playerNode.data() + 0x18, HorseHandle); Put(horseNode.data() + 0x18, PlayerHandle);
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
               copy(playerRide) || copy(horseRide) || copy(playerNode) || copy(horseNode) ||
               copy(world) || copy(set) || copy(entries);
    }
    uint8_t* Resolve(bool withWorld = true) {
        return MountedTorrent(player.data(), withWorld ? world.data() : nullptr,
            [&](const void* a, void* out, size_t n) { return Read(a, out, n); });
    }
};

void OnlyCurrentLivingPairIsSelected() {
    Memory m;
    Check(m.Resolve() == m.horse.data(), "loading on Torrent resolves the current pair without last_mounted");
    m.Put(m.playerRide.data() + 0x18, m.oldHorse.data());
    Check(m.Resolve() == m.horse.data(), "a stale historical horse is skipped in favor of the paired horse");
    m.Put(m.playerRide.data() + 0x18, m.horse.data());
    Check(m.Resolve(false) == m.horse.data(), "validated last_mounted works without the world holder");
    for (uint32_t state : {0u, 3u, 7u}) {
        m.Put(m.playerNode.data() + 0x50, state);
        Check(!m.Resolve(), "mounting/dismounting never force visibility or apply riding turns");
    }
    m.Put(m.playerNode.data() + 0x50, uint32_t{5});
    m.playerRide[0x162] = 1;
    Check(!m.Resolve(), "mounting flag prevents overriding the summon animation");
    m.playerRide[0x162] = 0; m.playerRide[0x163] = 0;
    Check(!m.Resolve(), "mounted flag must agree with the live pair state");
    m.playerRide[0x163] = 1;
    m.Put(m.horseNode.data() + 0x18, uint64_t{1});
    Check(!m.Resolve(), "a horse paired with a different rider is never selected");
    m.Put(m.horseNode.data() + 0x18, Memory::PlayerHandle);
    m.Put(m.horse.data() + 0x64, int32_t{1000});
    Check(!m.Resolve(), "an unrelated summon is never selected");
    m.Put(m.horse.data() + 0x64, int32_t{8000});
    m.fail = m.playerNode.data() + 0x18;
    Check(!m.Resolve(), "failed pair reads cannot expose or turn a stale mount");
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

void SeatedBodySharesCameraFrame() {
    int rider, horse, otherHorse;
    MountedSeatFrame seat;
    seat.Observe(&rider, &horse, 10, true);
    Check(!seat.CanCapture(10.2, true, true), "mount animation must settle before saving a seated pose");
    Check(!seat.CanCapture(11, false, true), "our previous IK output is not a fresh mounted pose");
    Check(!seat.CanCapture(11, true, false), "second eye cannot capture a different seated pose");
    Check(seat.CanCapture(11, true, true), "settled native seated pose can be captured");
    const Vec3 mountP{2, 3, 4}, riderP{2.1f, 4, 4.2f}, headLocal{0, 0.85f, -0.05f};
    const Quat mountQ = Quat::YawQ(0.6f), riderQ = mountQ * Quat::AxisAngle({1, 0, 0}, 0.04f);
    seat.Capture(mountP, mountQ, riderP, riderQ, riderP + riderQ.Rotate(headLocal));
    Near((seat.headLocal - headLocal).Length(), 0, "seat capture preserves head height without adding standing eye height");
    for (float yaw : {0.f, 0.7f, -1.5f, 3.14159265f}) {
        const Vec3 moved{7, 5, -3}; const Quat turn = Quat::YawQ(yaw);
        Vec3 stableP; Quat stableQ; seat.Root(moved, turn * mountQ, stableP, stableQ);
        const Vec3 expected = moved + turn.Rotate(riderP - mountP);
        Near((stableP - expected).Length(), 0, "horse movement and turns carry the same saddle offset");
        const Vec3 cameraHead = stableP + stableQ.Rotate(headLocal);
        // Simulate the renderer's later gallop bob/pitch, then rebase the fixed
        // seated pose into it. The head must still land at the camera anchor.
        for (float bob : {-0.2f, 0.f, 0.2f}) {
            const Vec3 renderP = stableP + Vec3{0.08f, bob, -0.12f};
            const Quat renderQ = stableQ * Quat::AxisAngle({1, 0, 0}, bob);
            Vec3 t{}; Quat q{};
            RebasePoseRoot(stableP, stableQ, renderP, renderQ, t, q);
            const Vec3 drawnHead = renderP + renderQ.Rotate(t + q.Rotate(headLocal));
            Near((drawnHead - cameraHead).Length(), 0, "late rider animation cannot move the body away from the camera");
        }
    }
    seat.Observe(&rider, nullptr, 12, false);
    Check(seat.ready, "second eye preserves the current calibration");
    seat.Observe(&rider, nullptr, 12, true);
    Check(!seat.ready && !seat.Matches(&rider, &horse), "dismount releases seated pose");
    seat.Observe(&rider, &otherHorse, 13, true);
    Check(!seat.ready && !seat.Matches(&rider, &horse), "a replacement mount requires a new seat capture");
}

int main() {
    OnlyCurrentLivingPairIsSelected(); TurnsStayWithTheMountedPair(); VisibilityPreservesOtherFlags();
    SeatedBodySharesCameraFrame();
    std::puts("Mounted rider resolution, turning and visibility checks passed.");
}
