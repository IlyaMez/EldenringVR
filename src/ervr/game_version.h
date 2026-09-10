#pragma once
#include <cstdint>

namespace ervr {
enum class GameBuild { Unknown, V262, V271 };

constexpr GameBuild IdentifyGameBuild(uint32_t majorMinor, uint32_t patchRevision) {
    if (majorMinor == 0x00020006 && patchRevision == 0x00020000) return GameBuild::V262;
    if (majorMinor == 0x00020007 && patchRevision == 0x00010000) return GameBuild::V271;
    return GameBuild::Unknown;
}

// 2.7.1.0 map verified against the installed executables: normalized instruction
// signatures, matching hook bodies, and RIP references for the camera flag.
// RegisterTask also matches fromsoftware-rs 29fd138 (1.17.1).
constexpr uint32_t GameBuildRva(GameBuild build, uint32_t legacy) {
    if (build == GameBuild::V262) return legacy;
    if (build != GameBuild::V271) return 0;
    switch (legacy) {
    case 0x3bc5e0: return 0x3bc5f0; // camera step
    case 0x3e94d0: return 0x3e96b0; // dummy position
    case 0xb02390: return 0xb03aa0; // character camera
    case 0x3b6e20: return 0x3b6e30; // follow
    case 0x3b7560: return 0x3b7570; // outer follow update
    case 0x3ca950: return 0x3ca960; // character model update
    case 0x716260: return 0x7170b0; // lock target
    case 0x146b5e0: return 0x146d450; // pose evaluation (disabled by default)
    case 0x680d70: return 0x681bc0; // camera finalizer
    case 0x3b15c80: return 0x3b19c80; // camera wall recovery
    case 0x1a679c0: return 0x1a69830; // near dither
    case 0xeb1fe0: return 0xeb3e50; // task registration
    case 0xc7f88b: return 0xc80fcb; // transition null call
    case 0x3f9b2b: return 0x3f9d5b; // transition load
    case 0x1eb9999: return 0x1ebb809; // panic null write
    default: return 0;
    }
}

void GameVersionInit(); // worker startup, before installing exception handlers
GameBuild CurrentGameBuild();
uint32_t GameRva(uint32_t legacy);
} // namespace ervr
