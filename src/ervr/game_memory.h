#pragma once
#include "vr_backend.h"
#include <cstdint>
#include <string>
#include <vector>

namespace ervr {

// --- generic helpers ---------------------------------------------------------
// Scans the main module's executable sections for an IDA-style pattern ("48 8B 05 ?? ?? ?? ??").
uint8_t* AobScan(const std::string& pattern);
// Same pattern syntax over the module's readable NON-executable sections (.data/.rdata),
// returning every match: data tables can exist in more than one copy.
std::vector<uint8_t*> AobScanData(const std::string& pattern);
// Resolves a rip-relative reference located at `at + relOffset` for an instruction of length `instrLen`.
uint8_t* ResolveRipRelative(uint8_t* at, int relOffset, int instrLen);
// Follows a pointer chain "0x20,0x60,..." starting from *base (i.e. base is the address of a pointer).
uint8_t* FollowChain(uint8_t* base, const std::vector<int>& offsets);
bool SafeRead(const void* addr, void* out, size_t size);
bool SafeWrite(void* addr, const void* in, size_t size);
std::vector<int> ParseChain(const std::string& s);

// --- experimental: hand aim -----------------------------------------------------
// Rotates the game camera by the delta of the right controller's yaw/pitch while
// the aim modifier (right grip by default) is held. Requires [Experimental]
// CameraAOB / CameraChain / offsets in ERVR.ini; silently inactive otherwise.
void GameHandAimInit();
void GameHandAimTick(const VRTracking& tr, const VRInputState& in, double now);

// --- menu-open flag ---------------------------------------------------------------
// Resolves [MenuNav] MenuAOB -> CSMenuMan* global and, if MenuChain is set, reads a byte
// that is nonzero while a menu is open. Returns -1 when unknown/unconfigured, else 0/1.
// With [MenuNav] Scan=true it logs which bytes of the CSMenuMan object change over time,
// which is how you find MenuChain for a new game version.
void GameMenuInit();
int  GameMenuOpen(double now);
}
