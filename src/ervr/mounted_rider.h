#pragma once
#include "vrmath.h"
#include <cstdint>
#include <cstddef>
#include <mutex>

namespace ervr {

// These are the 1.16.2 layouts from fromsoftware-rs eae96df (ChrIns,
// CSChrRideModule/CSRideNode). Resolve the current paired character, never just
// the historical last_mounted pointer: it is absent when loading on Torrent.
template<class Read>
uint8_t* MountedTorrent(uint8_t* player, uint8_t* world, Read read) {
    auto ptr = [&](const uint8_t* a) { uint8_t* p = nullptr; read(a, &p, sizeof(p)); return p; };
    auto rideOf = [&](uint8_t* chr) {
        uint8_t* mods = ptr(chr + 0x190);
        uint8_t* ride = mods ? ptr(mods + 0xE8) : nullptr;
        return ride && ptr(ride + 8) == chr ? ride : nullptr;
    };
    if (!player) return nullptr;
    uint8_t* ride = rideOf(player); if (!ride) return nullptr;
    uint8_t flags[2]{};
    if (!read(ride + 0x162, flags, sizeof(flags)) || flags[0] != 0 || flags[1] != 1) return nullptr;
    uint8_t* node = ptr(ride + 0x10); if (!node || ptr(node + 0x10) != player) return nullptr;
    uint32_t state = 0; uint64_t peer = 0, rider = 0;
    if (!read(node + 0x50, &state, 4) || state != 5 ||
        !read(node + 0x18, &peer, 8) || !read(player + 8, &rider, 8) ||
        (uint32_t)peer == UINT32_MAX || peer == 0 || peer == rider) return nullptr;
    auto matches = [&](uint8_t* chr) {
        if (!chr || chr == player) return false;
        int32_t model = 0; uint64_t handle = 0, back = 0;
        if (!read(chr + 0x64, &model, 4) || model != 8000 ||
            !read(chr + 8, &handle, 8) || handle != peer) return false;
        uint8_t* otherRide = rideOf(chr); if (!otherRide) return false;
        uint8_t ridden = 0;
        if (!read(otherRide + 0x33, &ridden, 1) || ridden != 1) return false;
        uint8_t* otherNode = ptr(otherRide + 0x10);
        return otherNode && ptr(otherNode + 0x10) == chr &&
               read(otherNode + 0x18, &back, 8) && back == rider;
    };
    if (uint8_t* last = ptr(ride + 0x18); matches(last)) return last;
    if (!world) return nullptr;
    // Local Torrent ChrSet holder (ER practice tool, exe 2.6.2). The current
    // pair handles above still have to match; a different summon is never used.
    if (uint8_t* set = ptr(world + 0x1CC90)) {
        uint32_t capacity = 0;
        uint8_t* entries = ptr(set + 0x18);
        if (entries && read(set + 0x10, &capacity, 4) && capacity <= 1024)
            for (uint32_t i = 0; i < capacity; ++i)
                if (uint8_t* chr = ptr(entries + i * 0x10); matches(chr)) return chr;
    }
    return nullptr;
}

// Worker input is consumed once on the game thread before the pair's model
// matrices are rebuilt. Clear it when the mount changes, so dismount/death/load
// cannot replay a turn on a later horse. No head-tracking yaw enters this queue.
class MountedTurnQueue {
    std::mutex mutex;
    const void* mount = nullptr;
    float pending = 0.f;
    uint32_t lastFrame = 0;
    bool haveFrame = false;
public:
    void SetMount(const void* current) {
        std::lock_guard<std::mutex> guard(mutex);
        if (mount != current) { mount = current; pending = 0.f; haveFrame = false; }
    }
    void Add(float radians) {
        std::lock_guard<std::mutex> guard(mutex);
        if (mount && std::isfinite(radians)) pending = WrapPi(pending + radians);
    }
    float Take(const void* current, uint32_t frame, bool simulationPass = true) {
        std::lock_guard<std::mutex> guard(mutex);
        if (!simulationPass) return 0.f;
        if (!current || mount != current) { mount = current; pending = 0.f; haveFrame = false; return 0.f; }
        if (haveFrame && lastFrame == frame) return 0.f;
        lastFrame = frame; haveFrame = true;
        const float turn = pending; pending = 0.f; return turn;
    }
};

// A settled rider pose lives in Torrent's physics frame. The animated rider
// model matrix must not move the camera or reintroduce gallop sway after IK.
// Keep this calibration separate from the standing pose and eye height.
struct MountedSeatFrame {
    const void* rider = nullptr;
    const void* mount = nullptr;
    double since = 0;
    bool ready = false;
    Vec3 offset{}, headLocal{};
    Quat rotation{};

    void Observe(const void* currentRider, const void* currentMount, double now, bool simulationPass) {
        if (!simulationPass) return;
        if (rider == currentRider && mount == currentMount) return;
        rider = currentRider; mount = currentMount; since = now; ready = false;
    }
    bool Matches(const void* currentRider, const void* currentMount) const {
        return currentMount && rider == currentRider && mount == currentMount;
    }
    bool CanCapture(double now, bool fresh, bool simulationPass) const {
        return mount && !ready && fresh && simulationPass && now - since >= 0.5;
    }
    void Capture(Vec3 mountPos, Quat mountYaw, Vec3 riderPos, Quat riderQ, Vec3 headWorld) {
        offset = mountYaw.Conj().Rotate(riderPos - mountPos);
        rotation = (mountYaw.Conj() * riderQ).Normalized();
        headLocal = riderQ.Conj().Rotate(headWorld - riderPos);
        ready = true;
    }
    void Root(Vec3 mountPos, Quat mountYaw, Vec3& riderPos, Quat& riderQ) const {
        riderPos = mountPos + mountYaw.Rotate(offset);
        riderQ = (mountYaw * rotation).Normalized();
    }
};

// Preserve the native pitch/roll and saddle offset when turning the pair.
// A rotation of the rider alone leaves its legs and torso across the horse.
inline void TurnRiderAboutMount(Vec3 mountPosition, float radians, Quat& mountRotation,
                                Vec3& riderPosition, Quat& riderRotation) {
    const Quat turn = Quat::YawQ(radians);
    riderPosition = mountPosition + turn.Rotate(riderPosition - mountPosition);
    mountRotation = (turn * mountRotation).Normalized();
    riderRotation = (turn * riderRotation).Normalized();
}

struct MountedVisibility {
    uint8_t frustum = 0, render = 0;
    uint32_t display = 0;
    float opacity = 1.f, previousOpacity = 1.f, baseOpacity = 1.f;
};
inline MountedVisibility VisibleMountedTorrent(MountedVisibility value) {
    value.frustum |= 0x10; // ChrInsFlags1c4::is_onscreen
    value.render |= 0x08;  // ChrInsFlags1c5::enable_render
    value.display |= 1;    // model display visibility (preserve shadow/other flags)
    value.opacity = value.previousOpacity = value.baseOpacity = 1.f;
    return value;
}

} // namespace ervr
