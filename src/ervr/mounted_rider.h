#pragma once
#include "vrmath.h"
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <mutex>

namespace ervr {

// Layouts: fromsoftware-rs (ChrIns, CSChrRideModule, CSRideNode / CSPairAnimNode), unchanged by 1.17:
//   ChrIns           +0x08 field_ins_handle, +0x64 npc_id (8000 = Torrent), +0x190 modules (+0xE8 = ride)
//   CSChrRideModule  +0x08 owner, +0x10 ride_node, +0x18 last_mounted, +0x33 is_ride_character,
//                    +0x162 is_mounting, +0x163 is_mounted
//   CSRideNode       +0x10 owner, +0x18 counter_party (the paired FieldInsHandle), +0x50 ride_state (5 = riding)
// The old resolver demanded every one of the pair's links at once and resolved nothing on 1.17.2: a whole ride
// on 2026-11-02 without the horse (no seated pose, no turns, no visibility). The horse is now taken on the
// first identity that holds, strongest first, and everything read is kept for the log line that explains a miss.
// Torrent's character ids: 8000, and on 1.17.2 the spectral steed reads 8002 (the 2026-11-02 log: pair handle
// and link back both matching, only the old `== 8000` check failing). The c8000 family, 8000-8099.
inline bool IsTorrentNpc(int32_t npc) { return npc >= 8000 && npc < 8100; }

struct TorrentLookup {
    uint8_t* mount = nullptr;
    const char* how = "";              // which identity matched
    bool rideModule = false;           // the rider's ride module reads as its own
    bool node = false;                 // ... and so does its ride node
    bool seated = false;               // riding state, not the mount/dismount animation
    uint8_t mounting = 0, mounted = 0;
    uint32_t state = 0;                // ride node state (with `node`)
    uint64_t rider = 0, peer = 0;      // the rider's handle and the one its node is paired with
    uint8_t* lastMounted = nullptr;
    int32_t lastNpc = 0;
    uint64_t lastHandle = 0, lastPeer = 0;
    uint8_t lastRidden = 0;
    uint32_t scanned = 0;              // summon-set entries looked at
};

template<class Read>
TorrentLookup FindMountedTorrent(uint8_t* player, uint8_t* world, Read read) {
    TorrentLookup r;
    auto ptr = [&](const uint8_t* a) { uint8_t* p = nullptr; if (!read(a, &p, sizeof(p))) p = nullptr; return p; };
    auto rideOf = [&](uint8_t* chr) -> uint8_t* {
        uint8_t* mods = chr ? ptr(chr + 0x190) : nullptr;
        uint8_t* ride = mods ? ptr(mods + 0xE8) : nullptr;
        return ride && ptr(ride + 8) == chr ? ride : nullptr;
    };
    auto nodePeer = [&](uint8_t* chr, uint8_t* ride, uint64_t& out) {
        uint8_t* n = ptr(ride + 0x10);
        return n && ptr(n + 0x10) == chr && read(n + 0x18, &out, 8);
    };
    uint8_t* ride = rideOf(player); if (!player || !ride) return r;
    r.rideModule = true;
    uint8_t flags[2]{};
    if (read(ride + 0x162, flags, sizeof(flags))) { r.mounting = flags[0]; r.mounted = flags[1]; }
    if (!read(player + 8, &r.rider, 8)) r.rider = 0;
    if (uint8_t* n = ptr(ride + 0x10); n && ptr(n + 0x10) == player && read(n + 0x50, &r.state, 4)) {
        r.node = true;
        if (!read(n + 0x18, &r.peer, 8)) r.peer = 0;
    }
    r.lastMounted = ptr(ride + 0x18);
    if (r.lastMounted) {
        read(r.lastMounted + 0x64, &r.lastNpc, 4);
        read(r.lastMounted + 8, &r.lastHandle, 8);
        if (uint8_t* lr = rideOf(r.lastMounted)) { read(lr + 0x33, &r.lastRidden, 1); nodePeer(r.lastMounted, lr, r.lastPeer); }
    }
    // Riding only: the summon and dismount animations keep the game's own pair (same rule as ReadRideState).
    r.seated = (r.node && r.state == 5) || (r.mounted == 1 && !(r.node && (r.state == 3 || r.state == 7)));
    const auto valid = [](uint64_t h) { return h != 0 && (uint32_t)h != UINT32_MAX; };
    if (!r.seated || !valid(r.rider)) return r;
    const bool peerOk = r.node && valid(r.peer) && r.peer != r.rider;
    // 0 = no, 1 = a weaker identity, 2 = the pair handle itself. The pair handle names the horse outright; the
    // weaker identities also want a Torrent character id.
    auto match = [&](uint8_t* chr, const char*& how) -> int {
        int32_t npc = 0; uint64_t handle = 0;
        if (!chr || chr == player || !read(chr + 0x64, &npc, 4) || !read(chr + 8, &handle, 8)) return 0;
        uint8_t* hr = rideOf(chr); if (!hr) return 0;
        if (peerOk && handle == r.peer) { how = "pair handle"; return 2; }
        if (!IsTorrentNpc(npc)) return 0;
        uint64_t back = 0;
        if (nodePeer(chr, hr, back) && back == r.rider) { how = "Torrent's pair link"; return 1; }
        uint8_t ridden = 0;
        if (chr == r.lastMounted && read(hr + 0x33, &ridden, 1) && ridden == 1) { how = "last mount, being ridden"; return 1; }
        return 0;
    };
    uint8_t* weak = nullptr; const char* weakHow = "";
    auto consider = [&](uint8_t* chr) {
        const char* how = "";
        const int m = match(chr, how);
        if (m == 2) { r.mount = chr; r.how = how; return true; }
        if (m == 1 && !weak) { weak = chr; weakHow = how; }
        return false;
    };
    if (consider(r.lastMounted)) return r;
    // Local Torrent ChrSet holder (ER practice tool, exe 2.6.2; below the 1.17.2 WorldChrMan shift).
    if (uint8_t* set = world ? ptr(world + 0x1CC90) : nullptr) {
        uint32_t capacity = 0;
        uint8_t* entries = ptr(set + 0x18);
        if (entries && read(set + 0x10, &capacity, 4) && capacity <= 1024)
            for (uint32_t i = 0; i < capacity; ++i) {
                ++r.scanned;
                if (consider(ptr(entries + i * 0x10))) return r;
            }
    }
    // A pair handle that names some other horse wins over Torrent's own link only when it is found; when the
    // handle matches nothing (an unreadable or moved field) the horse paired back to this rider stands in.
    r.mount = weak; r.how = weakHow;
    return r;
}

template<class Read>
uint8_t* MountedTorrent(uint8_t* player, uint8_t* world, Read read) {
    return FindMountedTorrent(player, world, read).mount;
}

// Worker input is consumed once on the game thread before the pair's model
// matrices are rebuilt. Clear it when the mount changes, so dismount/death/load
// cannot replay a turn on a later horse. Physical turns arrive through RideHeadFollow.
class MountedTurnQueue {
    mutable std::mutex mutex;
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
    // What the pair's next rebuild will turn by, without taking it (the drawn pose is solved before that rebuild).
    float Peek(const void* current) const {
        std::lock_guard<std::mutex> guard(mutex);
        return current && mount == current ? pending : 0.f;
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

// Seated on Torrent the rider is a captured seated pose (PoseMode=vr), not the gallop and jump animations that
// crouch it forward - the camera rode that head down into the body and Torrent's neck (2026-11-02, camera
// 0.45-0.80 m above the rider's feet). The pose stays local to the rider's own model matrix, so the body and the
// camera share the game's placement on the saddle.
struct MountedSeatFrame {
    const void* rider = nullptr;
    bool seated = false;
    double since = 0;

    void Observe(const void* currentRider, bool currentSeated, double now, bool simulationPass) {
        if (!simulationPass) return;
        if (rider == currentRider && seated == currentSeated) return;
        rider = currentRider; seated = currentSeated; since = now;
    }
    bool Seated(const void* currentRider) const { return seated && currentRider && rider == currentRider; }
    // The mount animation has to be over: a pose is only taken once the rider has sat for half a second.
    bool CanCapture(double now, bool fresh, bool simulationPass) const {
        return seated && fresh && simulationPass && now - since >= 0.5;
    }
};

// A seated stance is a pose the rider HOLDS. Gallop, jump and turn frames keep changing; an idle seat does not.
// A fresh pose becomes a candidate and is taken once it still reads the same `settle` later.
enum class SeatStep : uint8_t { Wait, NewCandidate, Take };
struct SeatSettle {
    double candidateAt = 0;
    bool have = false;
    void Reset() { have = false; candidateAt = 0; }
    // `difference`: the fresh pose against the stored candidate (unused without one). NewCandidate = store this pose.
    SeatStep Step(float difference, float tolerance, double now, double settle) {
        if (!have || difference > tolerance) { have = true; candidateAt = now; return SeatStep::NewCandidate; }
        if (now - candidateAt < settle) return SeatStep::Wait;
        Reset();
        return SeatStep::Take;
    }
};

// The seated body is kept upright: Torrent pitches on slopes and over jumps, and a rider pitched with it swung
// the head - and the camera on it - a quarter metre forward and back. Yaw only, pivoting on the pelvis so the
// hips stay in the saddle. `pelvis` is the pelvis in the rider's model frame (zero = pivot on the root).
inline void UprightSeatRoot(const Vec3& modelPos, const Quat& modelRot, const Vec3& pelvis, Vec3& pos, Quat& yaw) {
    const Vec3 f = modelRot.Rotate({0.f, 0.f, -1.f});
    yaw = (f.x * f.x + f.z * f.z < 1e-6f) ? modelRot : Quat::YawQ(std::atan2(f.x, f.z) + 3.14159265f);
    pos = modelPos + modelRot.Rotate(pelvis) - yaw.Rotate(pelvis);
}

// Gallop bob and saddle sway, taken out of the view: the camera anchor is low-passed in Torrent's own frame
// (physics position, yaw only), so the horse's travel, turns and jumps pass straight through while the rider's
// bounce on top of it is smoothed, never by more than `maxDeviation`. The body is not moved with it.
struct RideCameraStabilizer {
    bool init = false;
    Vec3 local{};       // smoothed anchor, Torrent frame
    double last = 0;
    float deviation = 0;  // |smoothed - live| after the last call, for the log

    void Reset() { init = false; deviation = 0; }
    // `advance`: the one call per simulation frame that may move the smoothing (the final camera write).
    Vec3 Apply(const Vec3& anchor, const Vec3& mountPos, const Quat& mountYaw, double now, bool advance,
               float seconds, float maxDeviation) {
        const Vec3 target = mountYaw.Conj().Rotate(anchor - mountPos);
        if (!init || (target - local).Length() > 2.f) {   // first frame on the horse, a warp or a world shift
            local = target; init = true; last = now; deviation = 0;
            return anchor;
        }
        Vec3 out = local;
        if (advance) {
            const float dt = Clamp((float)(now - last), 0.f, 0.1f); last = now;
            const float k = seconds > 1e-3f ? 1.f - std::exp(-dt / seconds) : 1.f;
            out = local + (target - local) * k;
        }
        Vec3 d = out - target;
        float len = d.Length();
        if (maxDeviation >= 0.f && len > maxDeviation) { out = target + d * (maxDeviation / len); len = maxDeviation; }
        if (advance) { local = out; deviation = len; }
        return mountPos + mountYaw.Rotate(out);
    }
};

// A frame on horseback writes the rider's pose several times - the Havok pose hook, the model-pos pre and post, the
// camera steps - and the game rebuilds the rider's matrix between the pre and the post. Only the POSE HOOK's write
// is drawn (2026-11-12 flat test: the rider lifted 40 cm in each kind of write in turn floated only for the pose
// hook), hung off the matrix the rebuild produces AFTER it. That write used to solve the upright seat in the
// matrix it replaces: at a gallop the rider's matrix turns up to 7.7 degrees a frame with Torrent's pitch, and the
// drawn body swung that much about the saddle - up to ~10 cm at the head, forward and back - while the camera, set
// after the rebuild, did not ("my body pops up in front of me when moving or jumping"). Now the frame's first
// write solves in the matrix the rebuild is about to produce - the last one carried the way the rider's physics
// moved since, which is how the game builds it (exact in position) - and every write of the frame places the
// view at one point of the rider's frame (RideFrameAnchor).
struct RigidPose { Vec3 pos{}; Quat rot{}; };
// `p` moved rigidly the way `from` moved to `to`: to * from^-1 * p.
inline RigidPose CarriedBy(const RigidPose& p, const RigidPose& from, const RigidPose& to) {
    const Quat d = (to.rot * from.rot.Conj()).Normalized();
    return {to.pos + d.Rotate(p.pos - from.pos), (d * p.rot).Normalized()};
}
// The rider's matrix over its physics: its tilt (`res` = physics^-1 * matrix, the ride's pitch eased in on top of
// the physics) changes every frame at a gallop and in a jump, and carrying the last matrix by the physics keeps last
// frame's tilt - up to 3.6 deg off on a 2026-11-12 in-headset gallop, the drawn upper body tipped ~4 cm toward or away
// from the eye for that frame. This continues the tilt's last per-frame change one more frame: the rotation the
// rebuild about to run will produce if the tilt keeps turning as it just did. False (nothing written) when that
// step exceeds `maxStepDeg` - a warp, a mount or a turn, not an eased tilt.
inline bool ExtrapolateTilt(const Quat& physRot, const Quat& resBefore, const Quat& resLast, float maxStepDeg, Quat& out) {
    Quat step = (resBefore.Conj() * resLast).Normalized();
    if (step.w < 0.f) step = Quat{-step.x, -step.y, -step.z, -step.w};
    const float stepDeg = 2.f * std::acos(std::fmin(1.f, step.w)) * 57.29578f;
    if (!(stepDeg <= maxStepDeg)) return false;
    out = (physRot * resLast * step).Normalized();
    return true;
}
// A point expressed in a pose's own frame, and back.
inline Vec3 IntoFrame(const RigidPose& frame, const Vec3& world) { return frame.rot.Conj().Rotate(world - frame.pos); }
inline Vec3 OutOfFrame(const RigidPose& frame, const Vec3& local) { return frame.pos + frame.rot.Rotate(local); }

// Per frame on horseback. The frame's first write (the pose hook - or the model-pos pre-write when the pose hook did
// not run) predicts the rider's new matrix and fixes the view there, in the rider's own frame. Frames are told
// apart by the rider's rebuild count `gen`: set at gen g, a write still at g comes before that frame's rebuild and
// uses the same prediction; a write past it comes after and uses the matrix the rebuild produced. Either way the
// view is the same point of the rider's frame, so the camera and the drawn body ride one matrix.
struct RideFrameAnchor {
    bool valid = false;
    uint32_t gen = 0;   // the rider's rebuild count when it was set (its frame's rebuild still to come)
    RigidPose rider;    // the predicted matrix it was set in
    Vec3 local{};       // the smoothed eye anchor in that frame
    double at = -1.0;
    void Set(uint32_t generation, const RigidPose& predicted, const Vec3& anchorWorld, double now) {
        gen = generation; rider = predicted; local = IntoFrame(predicted, anchorWorld); at = now; valid = true;
    }
    bool Fresh(double now) const { return valid && now >= at && now - at <= 0.1; }
    // A write that may start a frame (`opens`) does so once per rebuild, or when the last frame is long gone.
    bool Opens(uint32_t generation, bool opens, double now) const { return opens && (!Fresh(now) || gen != generation); }
    // The matrix a write at rebuild count `generation` is built on (`real` = the model matrix as it is now).
    RigidPose RiderFor(uint32_t generation, const RigidPose& real) const { return gen == generation ? rider : real; }
    Vec3 Anchor(const RigidPose& rider) const { return OutOfFrame(rider, local); }
    void Reset() { valid = false; at = -1.0; }
};

// Physical turns on horseback: the head yaw, followed the way BodyYaw=head follows it on foot (deadzone, then a
// time constant), turns Torrent and the rider by the followed amount - the same pair turn the right stick
// makes. The view is the headset's own, so nothing turns it a second time.
struct RideHeadFollow {
    bool init = false;
    float followed = 0.f;
    double last = 0;
    uint32_t frame = ~0u;

    void Reset() { init = false; }
    // The yaw to turn the pair by this simulation frame (0 for a repeated call in the same frame).
    float Step(float headYaw, double now, uint32_t simFrame, float deadzone, float seconds) {
        if (!std::isfinite(headYaw)) return 0.f;
        if (!init) { init = true; followed = headYaw; last = now; frame = simFrame; return 0.f; }
        if (simFrame == frame) return 0.f;
        frame = simFrame;
        const float dt = Clamp((float)(now - last), 0.f, 0.25f); last = now;
        float d = WrapPi(headYaw - followed);
        if (std::fabs(d) <= deadzone) return 0.f;
        d -= d > 0.f ? deadzone : -deadzone;
        const float k = seconds > 1e-3f ? 1.f - std::exp(-dt / seconds) : 1.f;
        const float turn = d * k;
        followed = WrapPi(followed + turn);
        return turn;
    }
};

// Full-frame pass 2 re-runs the rider's update at dt 0 and re-places it on Torrent: the second eye's rider read
// up to 0.4 m off the first eye's camera on 2026-11-02, and both eyes are drawn from the first pass's camera.
// So pass 2 draws the rider where pass 1 did: its pose is solved in the pass-1 placement and expressed in the
// model matrix pass 2 renders it with - measured once pass 2 rebuilt it, predicted from the last frame before.
// `f` is the stereo pair the passes belong to (the caller's pair counter; the game's frame counter advances
// between the rider's pass-1 rebuild and pass 2, which left the first version of this holding nothing).
struct RiderPassHold {
    uint32_t frame = ~0u;          // stereo pair of the pass-1 snapshot
    double at = -1.0;              // when it was taken (a pass 1 without a rider rebuild leaves an old one)
    Vec3 pos{}; Quat rot{};        // the rider's model matrix at the end of pass 1
    uint32_t pass2Frame = ~0u;     // pass 2 of this pair has rebuilt the rider already
    Vec3 deltaLocal{};             // pass 2 minus pass 1, rider yaw frame (the prediction)
    bool learned = false;

    void Pass1(const Vec3& p, const Quat& q, uint32_t f, double now = 0.0) { pos = p; rot = q; frame = f; at = now; }
    bool Held(uint32_t f, double now = 0.0) const { return frame == f && now - at < 0.1; }
    // Pass 2's own rebuild: returns its offset from pass 1 (world) and learns it for the next frame.
    Vec3 Pass2(const Vec3& p, const Quat& yaw, uint32_t f) {
        if (frame != f) return {};
        const Vec3 d = p - pos, local = yaw.Conj().Rotate(d);
        deltaLocal = learned ? deltaLocal * 0.4f + local * 0.6f : local;
        learned = true; pass2Frame = f;
        return d;
    }
    // Where pass 2 will render the rider when its rebuild has not run yet this frame.
    Vec3 Render(const Vec3& now, const Quat& yaw, uint32_t f) const {
        return pass2Frame == f ? now : now + yaw.Rotate(deltaLocal);
    }
    void Reset() { frame = pass2Frame = ~0u; at = -1.0; deltaLocal = {}; learned = false; }
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
