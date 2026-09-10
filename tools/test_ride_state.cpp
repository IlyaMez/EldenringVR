#include "ride_state.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace ervr;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}

struct GameMemory {
    std::array<uint8_t, 0x198> player{};
    std::array<uint8_t, 0xF0> modules{};
    std::array<uint8_t, 0x164> ride{};
    std::array<uint8_t, 0x54> node{};
    const void* unreadable = nullptr;

    template<class T> void Put(uint8_t* address, T value) { std::memcpy(address, &value, sizeof(value)); }
    GameMemory() {
        Put(player.data() + 0x190, modules.data());
        Put(modules.data() + 0xE8, ride.data());
        Put(ride.data() + 0x08, player.data());
        Put(ride.data() + 0x10, node.data());
        Put(node.data() + 0x10, player.data());
    }
    bool Read(const void* address, void* out, size_t size) {
        if (address == unreadable) return false;
        auto copy = [&](const auto& buffer) {
            const uintptr_t begin = reinterpret_cast<uintptr_t>(buffer.data());
            const uintptr_t at = reinterpret_cast<uintptr_t>(address);
            if (size > buffer.size() || at < begin || at - begin > buffer.size() - size) return false;
            std::memcpy(out, address, size); return true;
        };
        return copy(player) || copy(modules) || copy(ride) || copy(node);
    }
    RideState State() {
        return ReadRideState(player.data(), [&](const void* a, void* out, size_t n) { return Read(a, out, n); });
    }
};

int main() {
    GameMemory m;
    Check(m.State().valid && !m.State().NativeBody(), "on foot uses configured VR pose and facing");
    for (uint32_t state : {3u, 5u, 7u}) {
        m.Put(m.node.data() + 0x50, state);
        Check(m.State().valid && m.State().active && m.State().NativeBody(),
              "mounting, riding and dismounting preserve native rider pose and physics even with cleared flags");
    }
    m.Put(m.node.data() + 0x50, uint32_t{0});
    Check(!m.State().NativeBody(), "finishing dismount restores the walking policy");
    m.Put(m.ride.data() + 0x10, static_cast<uint8_t*>(nullptr));
    for (int flag : {0x162, 0x163}) {
        m.ride[flag] = 1;
        Check(m.State().valid && m.State().active,
              "live mount flags work without a node or last_mounted pointer (loading already on Torrent)");
        m.ride[flag] = 0;
    }
    m.Put(m.ride.data() + 0x18, m.player.data());
    Check(m.State().valid && !m.State().active, "a historical last_mounted pointer does not keep the rider mounted");
    m.ride[0x163] = 2;
    Check(!m.State().valid && m.State().NativeBody(), "invalid boolean leaves the native body alone");
    m.ride[0x163] = 0;
    m.unreadable = m.ride.data() + 0x162;
    Check(!m.State().valid && m.State().NativeBody(), "failed reads cannot start applying a standing pose");
    m.unreadable = nullptr;
    m.Put(m.ride.data() + 0x08, m.node.data());
    Check(!m.State().valid, "a module belonging to a different character is rejected");
    m.Put(m.ride.data() + 0x08, m.player.data());
    m.Put(m.ride.data() + 0x10, m.node.data());
    m.Put(m.node.data() + 0x10, m.ride.data());
    Check(!m.State().valid && m.State().NativeBody(), "a stale ride node cannot release rider protection");
    m.Put(m.node.data() + 0x10, m.player.data());
    m.unreadable = m.node.data() + 0x50;
    Check(!m.State().valid && m.State().NativeBody(), "unreadable dismount state preserves native animation");
    std::puts("Torrent ride-state regression checks passed.");
}
