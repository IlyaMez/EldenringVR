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
// Like SafeRead, but VirtualQuery-checked first so it never faults (the crash VEH logs every
// fault, even ones SEH then swallows). For pointers of unknown provenance, not hot paths.
bool ProbeRead(const void* addr, void* out, size_t size);
std::vector<int> ParseChain(const std::string& s);

// --- experimental: hand aim -----------------------------------------------------
// Rotates the game camera by the delta of the right controller's yaw/pitch while
// the aim modifier (right grip by default) is held. Requires [Experimental]
// CameraAOB / CameraChain / offsets in ERVR.ini; silently inactive otherwise.
void GameHandAimInit();
void GameHandAimTick(const VRTracking& tr, const VRInputState& in, double now);

// --- singletons -------------------------------------------------------------------
// Address of the static slot holding a Dantelion2 singleton ("CSFeMan", "FD4PadManager",
// "WorldChrMan"...), found through the game's own null-check getter that panics with the
// class name. Version-proof: no per-class AOB. nullptr if the name or getter is not found.
uint8_t* FindSingletonSlot(const char* name);
// The singleton's live object of RTTI class `rttiName` (e.g. "CS::CSSessionManagerImp"),
// reached from that slot - which may hold a static wrapper rather than the object itself.
// Scans the module for the slot: call it off the hot threads. nullptr until it exists.
uint8_t* FindSingletonInstance(const char* name, const char* rttiName, bool verbose, void*** vtOut = nullptr);

// --- menu-open state ---------------------------------------------------------------
// Is a game menu owning the controller right now? Read from the game's own state (see
// game_memory.cpp: CSFeMan HUD state, FD4PadManager pad routing, or the legacy MenuChain
// byte), selected by [MenuNav] Source. Returns -1 while no reader is trusted yet, else 0/1.
// With [MenuNav] Scan=true it logs which bytes of the CSMenuMan object change over time.
void GameMenuInit();
int  GameMenuOpen(double now);
}
