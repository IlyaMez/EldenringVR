#pragma once
// Reads item icons straight out of ELDEN RING's own archives - read-only, nothing extracted to disk.
//
// The game's files live in Data0-3.bdt, indexed by Data0-3.bhd. An index is RSA-encrypted with a
// public key the executable carries in PEM form in its .rdata (decrypting = the raw public operation,
// which Windows CNG does as BCryptEncrypt with no padding); its file headers are found by ELDEN RING's
// 64-bit path hash, and parts of a file may be AES-128-ECB encrypted with a key the header points to.
// menu/hi/00_solo.tpfbhd lists the per-item icons, 00_Solo\MENU_Knowledge_<iconId>.tpf.dcx, stored in
// menu/hi/00_solo.tpfbdt as Oodle (KRAK) DCX files - decompressed with the game's own oo2core DLL.
// Same files and the same route SpellWheel reads the menu atlases by (layout checked against the
// 2.7.2.0 archives, 2026-11-01). The HUD itself uses atlases in 01_common, so the per-item files are
// otherwise loaded only for an inventory's large preview - waiting for the game to load them was a
// dead end (round 2 of the weapon wheel: 0 captured).
#include <cstdint>
#include <string>
#include <vector>

namespace ervr {

// The decompressed MENU_Knowledge_<iconId> TPF. The first call opens the archive index (well under a
// second); call it from a background thread, never the game's or the pad worker's. Thread-safe.
// On failure `why` (optional) says what was missing.
bool ArchiveReadItemIcon(int iconId, std::vector<uint8_t>& tpf, std::string* why = nullptr,
                         const std::wstring& gameDirectory = {});
// Launcher callers name the installed game directory; the mod defaults to its executable's directory.
// Serialized with icon reads. These functions perform IO/decompression only on background threads.
bool ArchiveReadFile(const char* archivePath, std::vector<uint8_t>& bytes, std::string& why,
                     const std::wstring& gameDirectory = {});
bool ArchiveDecompress(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& out, std::string& why,
                       const std::wstring& gameDirectory = {});
bool ArchiveReadRegulation(std::vector<uint8_t>& out, std::string& why, const std::wstring& gameDirectory);

// ELDEN RING's archive path hash: lower case, forward slashes, h = h * 0x85 + c over 64 bits.
uint64_t ArchivePathHash(const char* path);

}
