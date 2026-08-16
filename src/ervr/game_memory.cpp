#include "game_memory.h"
#include "config.h"
#include "log.h"
#include <windows.h>
#include <sstream>
#include <cstring>

namespace ervr {

// ---- helpers ------------------------------------------------------------------
namespace {
struct PatByte { uint8_t v; bool wild; };
std::vector<PatByte> Parse(const std::string& s) {
    std::vector<PatByte> out;
    std::istringstream ss(s);
    std::string tok;
    while (ss >> tok) {
        if (tok == "?" || tok == "??") out.push_back({0, true});
        else out.push_back({(uint8_t)strtoul(tok.c_str(), nullptr, 16), false});
    }
    return out;
}
}

// SEH-guarded copies: free when the access is valid (no syscall), safe when it isn't.
static bool GuardedCopy(void* dst, const void* src, size_t size) {
    __try { std::memcpy(dst, src, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeRead(const void* addr, void* out, size_t size) {
    if (!addr || (uintptr_t)addr < 0x10000) return false;
    return GuardedCopy(out, addr, size);
}
bool SafeWrite(void* addr, const void* in, size_t size) {
    if (!addr || (uintptr_t)addr < 0x10000) return false;
    return GuardedCopy(addr, in, size);
}

uint8_t* AobScan(const std::string& pattern) {
    auto pat = Parse(pattern);
    if (pat.empty()) return nullptr;
    HMODULE mod = GetModuleHandleW(nullptr);
    auto* base = (uint8_t*)mod;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* start = base + sec->VirtualAddress;
        size_t size = sec->Misc.VirtualSize;
        if (size < pat.size()) continue;
        for (size_t off = 0; off + pat.size() <= size; ++off) {
            size_t k = 0;
            for (; k < pat.size(); ++k) if (!pat[k].wild && start[off + k] != pat[k].v) break;
            if (k == pat.size()) return start + off;
        }
    }
    return nullptr;
}

uint8_t* ResolveRipRelative(uint8_t* at, int relOffset, int instrLen) {
    int32_t rel = 0;
    if (!SafeRead(at + relOffset, &rel, sizeof(rel))) return nullptr;
    return at + instrLen + rel;
}

std::vector<int> ParseChain(const std::string& s) {
    std::vector<int> out;
    std::string tok; std::istringstream ss(s);
    while (std::getline(ss, tok, ',')) { if (!tok.empty()) out.push_back((int)strtol(tok.c_str(), nullptr, 0)); }
    return out;
}

uint8_t* FollowChain(uint8_t* base, const std::vector<int>& offsets) {
    uint8_t* p = nullptr;
    if (!SafeRead(base, &p, sizeof(p)) || !p) return nullptr;
    for (size_t i = 0; i < offsets.size(); ++i) {
        uint8_t* addr = p + offsets[i];
        if (i + 1 == offsets.size()) return addr;      // last offset addresses the value itself
        if (!SafeRead(addr, &p, sizeof(p)) || !p) return nullptr;
    }
    return p;
}

// ---- hand aim ------------------------------------------------------------------
namespace {
uint8_t* g_camBasePtr = nullptr;      // address holding pointer to camera manager
std::vector<int> g_chain;
bool g_aimEngaged = false;
float g_yaw0 = 0, g_pitch0 = 0, g_handYaw0 = 0, g_handPitch0 = 0;
bool g_initTried = false;
}

void GameHandAimInit() {
    const Config& c = Cfg();
    g_initTried = true;
    if (!c.handAimEnabled) return;
    if (c.cameraAob.empty()) { LOGW("[handaim] HandAim=true but CameraAOB is empty; disabled"); return; }
    uint8_t* hit = AobScan(c.cameraAob);
    if (!hit) { LOGW("[handaim] CameraAOB not found in eldenring.exe; disabled"); return; }
    g_camBasePtr = ResolveRipRelative(hit, c.cameraAobOffset, c.cameraAobInstrLen);
    g_chain = ParseChain(c.cameraChain);
    LOGI("[handaim] AOB hit %p -> base pointer %p, chain of %zu offsets", (void*)hit, (void*)g_camBasePtr, g_chain.size());
}

void GameHandAimTick(const VRTracking& tr, const VRInputState& in, double /*now*/) {
    const Config& c = Cfg();
    if (!g_initTried) GameHandAimInit();
    if (!g_camBasePtr) return;
    uint8_t* cam = FollowChain(g_camBasePtr, g_chain);
    if (!cam) { g_aimEngaged = false; return; }
    float* yaw = (float*)(cam + c.cameraYawOffset);
    float* pitch = (float*)(cam + c.cameraPitchOffset);
    const Pose& h = tr.hand[HandRight];
    bool want = h.valid && in.handTrigger[HandRight] >= c.gripThreshold;
    if (!want) { g_aimEngaged = false; return; }
    float hy = h.rot.Yaw(), hp = h.rot.Pitch();
    if (!g_aimEngaged) {
        if (!SafeRead(yaw, &g_yaw0, 4) || !SafeRead(pitch, &g_pitch0, 4)) return;
        g_handYaw0 = hy; g_handPitch0 = hp; g_aimEngaged = true;
        return;
    }
    // Game yaw is typically CW-positive when viewed from above in FromSoft titles; sign is configurable via strength.
    float ny = g_yaw0 - WrapPi(hy - g_handYaw0) * c.handAimStrength;
    float np = Clamp(g_pitch0 + (hp - g_handPitch0) * c.handAimStrength, -1.4f, 1.4f);
    SafeWrite(yaw, &ny, 4);
    SafeWrite(pitch, &np, 4);
}
}
