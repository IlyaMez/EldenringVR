#pragma once
#include <cstddef>
#include <cstdint>

namespace ervr {

struct RideState {
    bool valid = false;
    bool active = false;   // mounting, riding or dismounting
    bool seated = false;   // riding: the mount animation is over and the dismount has not begun

    // If the mount cannot be read, leave the rider's pose and physics to the game.
    bool NativeBody() const { return !valid || active; }
};

// ELDEN RING 1.16.2: ChrInsModuleContainer::ride, CSChrRideModule and CSRideNode.
// Layout: fromsoftware-rs eae96df, crates/eldenring/src/cs/chr_ins/module/ride.rs.
// last_mounted (+0x18) is historical and can be null after loading on Torrent;
// use the live flags plus the node state to cover mounting AND dismounting.
template<class Read>
RideState ReadRideState(const uint8_t* player, Read read) {
    if (!player) return {};
    const uint8_t *modules = nullptr, *ride = nullptr, *owner = nullptr, *node = nullptr;
    if (!read(player + 0x190, &modules, sizeof(modules)) || !modules ||
        !read(modules + 0xE8, &ride, sizeof(ride)) || !ride ||
        !read(ride + 0x08, &owner, sizeof(owner)) || owner != player) return {};
    uint8_t flags[2]{};   // is_mounting @0x162, is_mounted @0x163
    if (!read(ride + 0x162, flags, sizeof(flags)) || flags[0] > 1 || flags[1] > 1) return {};
    // Seated: the node's riding state (5), or the mounted flag outside the mount/dismount states (3/7) - no node,
    // a node left idle after loading on Torrent, or a riding state a later patch numbers differently.
    uint32_t state = 0;
    const bool haveNode = read(ride + 0x10, &node, sizeof(node)) && node &&
        read(node + 0x10, &owner, sizeof(owner)) && owner == player && read(node + 0x50, &state, sizeof(state));
    const bool seated = (haveNode && state == 5) || (flags[1] == 1 && !(haveNode && (state == 3 || state == 7)));
    if (flags[0] || flags[1]) return {true, true, seated};
    if (!read(ride + 0x10, &node, sizeof(node))) return {};
    if (!node) return {true, false, false};
    if (!haveNode) return {};
    // Known node states: 0 idle, 3 mounting, 5 riding, 7 dismounting.
    // Preserve native animation for any other active transition as well.
    return {true, state != 0, seated};
}

} // namespace ervr
