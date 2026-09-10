#include "game_version.h"
using namespace ervr;
// File-version words are not interchangeable with the user-facing patch number.
static_assert(IdentifyGameBuild(0x00020006, 0x00020000) == GameBuild::V262);
static_assert(IdentifyGameBuild(0x00020007, 0x00010000) == GameBuild::V271);
static_assert(IdentifyGameBuild(0x00020007, 0) == GameBuild::Unknown);
static_assert(IdentifyGameBuild(0x00020007, 0x00010001) == GameBuild::Unknown);
static_assert(GameBuildRva(GameBuild::Unknown, 0x3bc5e0) == 0);
static_assert(GameBuildRva(GameBuild::V271, 0x123456) == 0);
int main() {}
