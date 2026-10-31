#pragma once
#include <cstddef>
#include <cstdint>

namespace ervr {
enum class GameBuild { Unknown, V262, V271, V272 };

constexpr GameBuild IdentifyGameBuild(uint32_t majorMinor, uint32_t patchRevision) {
    if (majorMinor == 0x00020006 && patchRevision == 0x00020000) return GameBuild::V262;
    if (majorMinor == 0x00020007 && patchRevision == 0x00010000) return GameBuild::V271;
    if (majorMinor == 0x00020007 && patchRevision == 0x00020000) return GameBuild::V272;
    return GameBuild::Unknown;
}

// Layouts shared by every 1.17 patch (exe 2.7.x).
constexpr bool IsGame117(GameBuild build) { return build == GameBuild::V271 || build == GameBuild::V272; }

// One row per site, keyed by its 2.6.2.0 RVA.
// 2.7.1.0 map verified against the installed executables: normalized instruction
// signatures, matching hook bodies, and RIP references for the camera flag.
// RegisterTask also matches fromsoftware-rs 29fd138 (1.17.1).
// 2.7.2.0 (1.17.2) verified the same way from 2.7.1.0 (tools/verify_game_version_map.py);
// every site erfps2 4af562f or fromsoftware-rs b726336 publish agrees.
struct GameRvaRow { uint32_t v262, v271, v272; };
inline constexpr GameRvaRow kGameRvaMap[] = {
    {0x3bc5e0, 0x3bc5f0, 0x3bc5f0},    // camera step
    {0x3e94d0, 0x3e96b0, 0x3e96b0},    // dummy position
    {0xb02390, 0xb03aa0, 0xb03f60},    // character camera
    {0x3b6e20, 0x3b6e30, 0x3b6e30},    // follow
    {0x3b7560, 0x3b7570, 0x3b7570},    // outer follow update
    {0x3ca950, 0x3ca960, 0x3ca960},    // character model update
    {0x716260, 0x7170b0, 0x717560},    // lock target
    {0x146b5e0, 0x146d450, 0x146eb40}, // pose evaluation (disabled by default)
    {0x680d70, 0x681bc0, 0x682070},    // camera finalizer
    {0x3b15c80, 0x3b19c80, 0x3b1cc80}, // camera wall recovery
    {0x1a679c0, 0x1a69830, 0x1a6af20}, // near dither
    {0xeb1fe0, 0xeb3e50, 0xeb5540},    // task registration
    {0xc7f88b, 0xc80fcb, 0xc8148b},    // transition null call
    {0x3f9b2b, 0x3f9d5b, 0x3f9deb},    // transition load
    {0x1eb9999, 0x1ebb809, 0x1ebcef9}, // panic null write
};

constexpr uint32_t GameBuildRva(GameBuild build, uint32_t legacy) {
    if (build == GameBuild::V262) return legacy;
    for (const GameRvaRow& row : kGameRvaMap)
        if (row.v262 == legacy) return build == GameBuild::V271 ? row.v271 : build == GameBuild::V272 ? row.v272 : 0;
    return 0;
}

// 1.17.2 moved every WorldChrMan field from +0x1E608 on by 0x10 (fromsoftware-rs 5baf0dd).
// Confirmed in the exe: each 2.7.1.0 instruction using chr_cam (+0x1ECE0) or the by-distance
// list (+0x1F1D0..) matches a 2.7.2.0 one 0x10 further; main_player (+0x1E508) did not move.
constexpr size_t GameBuildWorldChrManOffset(GameBuild build, size_t legacy) {
    return build == GameBuild::V272 && legacy >= 0x1E608 ? legacy + 0x10 : legacy;
}

void GameVersionInit(); // worker startup, before installing exception handlers
GameBuild CurrentGameBuild();
uint32_t GameRva(uint32_t legacy);
size_t GameWorldChrManOffset(size_t legacy);
} // namespace ervr
