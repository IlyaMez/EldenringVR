#include "game_version.h"
using namespace ervr;
// File-version words are not interchangeable with the user-facing patch number.
static_assert(IdentifyGameBuild(0x00020006, 0x00020000) == GameBuild::V262);
static_assert(IdentifyGameBuild(0x00020007, 0x00010000) == GameBuild::V271);
static_assert(IdentifyGameBuild(0x00020007, 0x00020000) == GameBuild::V272);
static_assert(IdentifyGameBuild(0x00020007, 0) == GameBuild::Unknown);
static_assert(IdentifyGameBuild(0x00020007, 0x00010001) == GameBuild::Unknown);
static_assert(IdentifyGameBuild(0x00020007, 0x00020001) == GameBuild::Unknown);   // JP build
static_assert(GameBuildRva(GameBuild::Unknown, 0x3bc5e0) == 0);
static_assert(GameBuildRva(GameBuild::V262, 0x123456) == 0x123456);
static_assert(GameBuildRva(GameBuild::V271, 0x123456) == 0);
static_assert(GameBuildRva(GameBuild::V272, 0x123456) == 0);
static_assert(GameBuildRva(GameBuild::V271, 0x716260) == 0x7170b0);
static_assert(GameBuildRva(GameBuild::V272, 0x716260) == 0x717560);
static_assert(GameBuildRva(GameBuild::V272, 0x3bc5e0) == 0x3bc5f0);
static_assert(IsGame117(GameBuild::V271) && IsGame117(GameBuild::V272) && !IsGame117(GameBuild::V262));
static_assert(GameBuildWorldChrManOffset(GameBuild::V272, 0x1ECE0) == 0x1ECF0);
static_assert(GameBuildWorldChrManOffset(GameBuild::V272, 0x1F1D8) == 0x1F1E8);
static_assert(GameBuildWorldChrManOffset(GameBuild::V272, 0x1E508) == 0x1E508);
static_assert(GameBuildWorldChrManOffset(GameBuild::V271, 0x1ECE0) == 0x1ECE0);
static_assert(GameBuildWorldChrManOffset(GameBuild::Unknown, 0x1ECE0) == 0x1ECE0);
int main() {}
