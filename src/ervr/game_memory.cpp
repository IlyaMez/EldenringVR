#include "game_memory.h"
#include "config.h"
#include "log.h"
#include <windows.h>
#include <sstream>
#include <cstring>
#include <vector>

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

// ---- menu-open flag ------------------------------------------------------------
namespace {
uint8_t* g_menuBasePtr = nullptr;     // address of the CSMenuMan* global
std::vector<int> g_menuChain;
std::string g_menuChainStr, g_menuAobStr;
bool g_menuInitTried = false;
int  g_menuLast = -1;
// scan helper
std::vector<uint8_t> g_scanPrev;
std::vector<uint16_t> g_scanFlips;    // per-offset change count within the current window
double g_scanWindow = 0, g_scanNext = 0;
}

void GameMenuInit() {
    const Config& c = Cfg();
    g_menuInitTried = true;
    g_menuBasePtr = nullptr;
    g_menuAobStr = c.menuAob; g_menuChainStr = c.menuChain;
    g_menuChain = ParseChain(c.menuChain);
    if (c.menuAob.empty()) return;
    uint8_t* hit = AobScan(c.menuAob);
    if (!hit) { LOGW("[menu] MenuAOB not found in eldenring.exe; menu detection falls back to the START button"); return; }
    g_menuBasePtr = ResolveRipRelative(hit, c.menuAobOffset, c.menuAobInstrLen);
    LOGI("[menu] AOB hit %p -> CSMenuMan* global %p, chain of %zu offsets%s", (void*)hit, (void*)g_menuBasePtr,
         g_menuChain.size(), g_menuChain.empty() ? " (MenuChain empty: flag disabled)" : "");
}

static void MenuScanTick(double now) {
    const Config& c = Cfg();
    uint8_t* obj = nullptr;
    if (!SafeRead(g_menuBasePtr, &obj, sizeof(obj)) || !obj) return;
    size_t n = (size_t)Clamp((float)c.menuScanSize, 16.f, 65536.f);
    std::vector<uint8_t> cur(n);
    if (!SafeRead(obj, cur.data(), n)) return;
    if (g_scanPrev.size() != n) { g_scanPrev = cur; g_scanFlips.assign(n, 0); g_scanWindow = now; LOGI("[menuscan] watching CSMenuMan object %p (%zu bytes); open/close a menu and look for offsets that flip once each way", (void*)obj, n); return; }
    if (now >= g_scanNext) {                       // 20 Hz compare
        g_scanNext = now + 0.05;
        for (size_t i = 0; i < n; ++i) {
            if (cur[i] == g_scanPrev[i]) continue;
            if (g_scanFlips[i] < 65535) ++g_scanFlips[i];
            if (g_scanFlips[i] <= 3)   // stop reporting noisy offsets (counters, timers)
                LOGI("[menuscan] +0x%zX: %02X -> %02X", i, g_scanPrev[i], cur[i]);
        }
        g_scanPrev = cur;
        if (now - g_scanWindow > 10.0) { g_scanFlips.assign(n, 0); g_scanWindow = now; }
    }
}

int GameMenuOpen(double now) {
    const Config& c = Cfg();
    if (!g_menuInitTried || c.menuAob != g_menuAobStr || c.menuChain != g_menuChainStr) GameMenuInit();
    if (!g_menuBasePtr) return -1;
    if (c.menuScan) MenuScanTick(now);
    if (g_menuChain.empty()) return -1;
    uint8_t* flag = FollowChain(g_menuBasePtr, g_menuChain);
    uint8_t v = 0;
    if (!flag || !SafeRead(flag, &v, 1)) return -1;
    int open = v ? 1 : 0;
    if (open != g_menuLast) { LOGD("[menu] flag %p = %u -> %s", (void*)flag, v, open ? "open" : "closed"); g_menuLast = open; }
    return open;
}
}
