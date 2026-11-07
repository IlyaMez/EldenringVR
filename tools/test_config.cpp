// ERVR.ini loading: the retired pass-2 keep sets an installed ini still carries give way to the current
// defaults (and nothing else does), and the shipped ini reads the Valve Index keys.
#include "config.h"
#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace ervr {
void Log(LogLevel, const char*, ...) {}
}

namespace {
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}

std::wstring TempIni(const char* body) {
    wchar_t dir[MAX_PATH]{}, path[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"erv", 0, path);
    FILE* f = _wfopen(path, L"wb");
    Check(f != nullptr, "temp ini must be writable");
    std::fputs(body, f);
    std::fclose(f);
    return path;
}

ervr::Config LoadFrom(const char* body) {
    const std::wstring path = TempIni(body);
    ervr::Config c;
    c.Load(path);
    DeleteFileW(path.c_str());
    return c;
}

const char* kFull060 = "0-7,10-14,16-21,27,37-38,43,45-46,48-63,65-79,81,83-84,86-88,95-96,98,106-109,113-125,127-132,141-142,144-160,164,168";
const char* kSafe060 = "0-7,10-14,16-21,27,37-38,43,45-46,48-63,65-79,81,83-84,86-88,95-96,98,106-109,113-117,119-122,127-132,141-142,144-160,164,168";
const char* kFull1625 = "0-7,10-14,16-25,27,37-38,43,45-46,48-63,65-79,81,83-84,86-88,95-96,98,106-109,113-125,127-132,141-142,144-160,164,168";
const char* kSafe1625 = "0-7,10-14,16-25,27,37-38,43,45-46,48-63,65-79,81,83-84,86-88,95-96,98,106-109,113-117,119-122,127-132,141-142,144-160,164,168";
const char* kFull074 = "0-7,10-14,16-21,25,27,37-38,43,45-46,48-63,65-79,81,83-84,86-88,95-96,98,106-109,113-125,127-132,141-142,144-160,164,168";
const char* kSafe074 = "0-7,10-14,16-21,25,27,37-38,43,45-46,48-63,65-79,81,83-84,86-88,95-96,98,106-109,113-117,119-122,127-132,141-142,144-160,164,168";

// Whether a keep spec ("0-7,11,20") contains group g.
bool Keeps(const std::string& spec, int g) {
    size_t at = 0;
    while (at < spec.size()) {
        const size_t comma = spec.find(',', at);
        const std::string part = spec.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        const size_t dash = part.find('-');
        const int lo = std::stoi(part.substr(0, dash)), hi = dash == std::string::npos ? lo : std::stoi(part.substr(dash + 1));
        if (g >= lo && g <= hi) return true;
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    return false;
}
}

int main() {
    const ervr::Config defaults;
    // The draw-only sets (2026-11-07): no game flow, characters, events, damage, AI or menus in the second eye's pass;
    // the world update (20, the environment lighting) and the full set's camera family (118, 123-125) stay.
    for (int g : {9, 10, 12, 16, 17, 21, 22, 23, 24, 25, 27, 54, 68, 71, 74, 78, 113, 117, 120, 121, 122, 126, 130, 141})
        Check(!Keeps(defaults.vrPass2FullFrameKeep, g) && !Keeps(defaults.vrPass2SafeKeep, g),
              "no game logic group in the second eye's pass");
    for (int g : {0, 11, 20, 37, 98, 106, 107, 142, 144, 155, 160, 164, 168})
        Check(Keeps(defaults.vrPass2FullFrameKeep, g) && Keeps(defaults.vrPass2SafeKeep, g), "the draw-side groups stay");
    for (int g : {118, 123, 124, 125})
        Check(Keeps(defaults.vrPass2FullFrameKeep, g) && !Keeps(defaults.vrPass2SafeKeep, g),
              "the camera family carries the second eye's camera in the full set, not in the settle window's");
    Check(!defaults.vrPass2Characters && !defaults.vrPass2TalkScripts && defaults.vrPass2FieldUpdate &&
          defaults.vrPass2PromptsInPass1, "characters and talk scripts once per frame; FieldArea (the lighting) in both");

    for (const auto& pair : {std::make_pair(kFull060, kSafe060), std::make_pair(kFull1625, kSafe1625),
                             std::make_pair(kFull074, kSafe074)}) {
        const std::string body = std::string("[VR]\r\nPass2FullFrameKeep=") + pair.first + "\r\nPass2SafeKeep=" + pair.second + "\r\n";
        const ervr::Config c = LoadFrom(body.c_str());
        Check(c.vrPass2FullFrameKeep == defaults.vrPass2FullFrameKeep, "a retired full keep set gives way to the default");
        Check(c.vrPass2SafeKeep == defaults.vrPass2SafeKeep, "a retired safe keep set gives way to the default");
        Check(c.pass2KeepNote == "Pass2FullFrameKeep and Pass2SafeKeep", "the replacement is noted for the log");
    }
    {
        const ervr::Config c = LoadFrom("[VR]\r\nPass2FullFrameKeep=0-7,10-14,16-21,27\r\nPass2SafeKeep=0-7\r\n");
        Check(c.vrPass2FullFrameKeep == "0-7,10-14,16-21,27" && c.vrPass2SafeKeep == "0-7",
              "any other keep set is the player's own and stays");
        Check(c.pass2KeepNote.empty(), "nothing replaced, nothing noted");
    }
    {
        const std::string body = std::string("[VR]\r\nPass2SafeKeep=") + kSafe060 + "\r\n";
        const ervr::Config c = LoadFrom(body.c_str());
        Check(c.vrPass2SafeKeep == defaults.vrPass2SafeKeep && c.pass2KeepNote == "Pass2SafeKeep",
              "either line is replaced on its own");
    }
    {   // Two-handing: the old shipped side nudge (for a hand placed by its wrist) reads 0; a player's own value stays.
        Check(defaults.twoHandPinSideOffset == 0.f && !defaults.retiredPinSideOffset,
              "the pinned palm sits on the shaft by default");
        const ervr::Config old = LoadFrom("[Combat]\r\nTwoHandPinSideOffset=0.03\r\n");
        Check(old.twoHandPinSideOffset == 0.f && old.retiredPinSideOffset, "an ini still saying 0.03 reads 0, noted");
        const ervr::Config own = LoadFrom("[Combat]\r\nTwoHandPinSideOffset=-0.02\r\n");
        Check(own.twoHandPinSideOffset == -0.02f && !own.retiredPinSideOffset, "any other side offset is used as written");
    }
#ifdef ERVR_TEST_INI
    {
        const std::string path = ERVR_TEST_INI;
        ervr::Config c;
        c.Load(std::wstring(path.begin(), path.end()));
        Check(c.vrPass2FullFrameKeep == defaults.vrPass2FullFrameKeep && c.vrPass2SafeKeep == defaults.vrPass2SafeKeep &&
              c.pass2KeepNote.empty(), "the shipped ini leaves the keep sets to ERVR's own");
        Check(c.btnLeftPad == "MENU" && c.btnRightPad == "DPAD" && c.padForce == 0.5f && c.indexGripForce == 0.5f,
              "the shipped ini carries the Valve Index trackpad and grip keys at their defaults");
        Check(c.twoHandPinSideOffset == 0.f && !c.retiredPinSideOffset && !c.vrPass2RegionReset &&
              c.vrPass2KeepScriptPrompts && c.vrPass2RegionBank && c.vrPass2EventsOnce,
              "the shipped ini leaves the two-hand side offset, the region table, the script prompts and the "
              "once-per-frame event manager to ERVR's own");
    }
#endif
    std::puts("ERVR.ini loading checks passed.");
}
