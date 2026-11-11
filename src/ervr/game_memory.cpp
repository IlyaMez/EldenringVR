#include "game_memory.h"
#include "config.h"
#include "log.h"
#include "rtti.h"
#include <windows.h>
#include <sstream>
#include <cstring>
#include <vector>
#include <algorithm>
#include <atomic>
#include <thread>

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
    // Non-canonical / kernel-half addresses (0xFFFFFFFFFFFFFFFF fill, -1 sentinels) are rejected
    // up front: the SEH guard would catch them, but the crash VEH logs every fault it sees first.
    if (!addr || (uintptr_t)addr < 0x10000 || (uintptr_t)addr + size > 0x7FFFFFFF0000ull) return false;
    return GuardedCopy(out, addr, size);
}
// Read that never faults: the range must be committed and readable per VirtualQuery. A syscall
// per call, so for probing pointers of unknown provenance, not for hot paths.
bool ProbeRead(const void* addr, void* out, size_t size) {
    if (!addr || (uintptr_t)addr < 0x10000 || (uintptr_t)addr + size > 0x7FFFFFFF0000ull) return false;
    // A range may span several regions (.data is a patchwork of protections): every one must be readable.
    const uint8_t* p = (const uint8_t*)addr; const uint8_t* end = p + size;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)) return false;
        DWORD prot = mbi.Protect & 0xFF;
        if (prot != PAGE_READONLY && prot != PAGE_READWRITE && prot != PAGE_EXECUTE_READ && prot != PAGE_EXECUTE_READWRITE && prot != PAGE_WRITECOPY && prot != PAGE_EXECUTE_WRITECOPY) return false;
        p = (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
    }
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

std::vector<uint8_t*> AobScanData(const std::string& pattern) {
    std::vector<uint8_t*> hits;
    auto pat = Parse(pattern);
    if (pat.empty()) return hits;
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        if (!(sec->Characteristics & IMAGE_SCN_MEM_READ)) continue;
        uint8_t* start = base + sec->VirtualAddress;
        size_t size = sec->Misc.VirtualSize;
        if (size < pat.size()) continue;
        for (size_t off = 0; off + pat.size() <= size; ++off) {
            size_t k = 0;
            for (; k < pat.size(); ++k) if (!pat[k].wild && start[off + k] != pat[k].v) break;
            if (k == pat.size()) hits.push_back(start + off);
        }
    }
    return hits;
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

// ---- singleton slots ---------------------------------------------------------------
// Every Dantelion2 singleton (DLRF) has a getter of the shape
//     mov rax,[rip+slot] ; test rax,rax ; jnz ok ; call DLPanic-ish ; lea rcx,[rip+"CSFeMan"] ...
// so the class-name string leads to the getter and the getter to the static slot. That is
// what fromsoftware-rs / the practice tool do too, and it survives game updates without an
// AOB per class. Verified on eldenring.exe 2.6.2.0 for FD4PadManager, CSFeMan and CSMenuMan.
// CAUTION (measured 2026-08-24): for CSMenuMan / CSFeMan the slot holds a small STATIC
// wrapper object inside the exe image, not the heap CSMenuManImp / CSFeManImp that carries
// the state - those are a pointer or two away from the wrapper (see FindImpFrom).
namespace {
struct ModSection { uint8_t* start; size_t size; bool exec; };
std::vector<ModSection> ModuleSections() {
    std::vector<ModSection> out;
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_READ)) continue;
        out.push_back({base + sec->VirtualAddress, (size_t)sec->Misc.VirtualSize, (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0});
    }
    return out;
}
bool InMainModule(const void* p) {
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    return p >= base && p < base + nt->OptionalHeader.SizeOfImage;
}
bool PlausiblePtr(uintptr_t v) { return v >= 0x10000 && v < 0x7FFFFFFF0000ull && (v & 7) == 0; }
unsigned long long ExeOff(const void* p) { return (unsigned long long)((const uint8_t*)p - (const uint8_t*)GetModuleHandleW(nullptr)); }
}

uint8_t* FindSingletonSlot(const char* name) {
    size_t n = strlen(name) + 1;                      // include the terminator
    // Every place the name STARTS a string, not ends one. The byte before need not be zero: 2.7.x
    // packs "CSGaitem" straight after a float constant (00 00 80 3F), so only an identifier
    // character there means a longer name. The getter below decides which candidate is real.
    auto ident = [](uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == ':'; };
    std::vector<uint8_t*> strs;
    for (auto& s : ModuleSections()) {
        if (s.exec || s.size < n) continue;
        for (size_t off = 0; off + n <= s.size; ++off) {
            if (s.start[off] != (uint8_t)name[0] || memcmp(s.start + off, name, n) != 0) continue;
            if (off > 0 && ident(s.start[off - 1])) continue;
            strs.push_back(s.start + off);
        }
    }
    if (strs.empty()) { LOGW("[singleton] \"%s\": name string not in eldenring.exe", name); return nullptr; }
    for (auto& s : ModuleSections()) {
        if (!s.exec) continue;
        for (size_t off = 0x30; off + 7 <= s.size; ++off) {
            uint8_t* p = s.start + off;
            if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x0D) continue;      // lea rcx,[rip+X]
            if (std::find(strs.begin(), strs.end(), ResolveRipRelative(p, 3, 7)) == strs.end()) continue;
            for (uint8_t* q = p - 11; q >= p - 0x30; --q) {                  // mov rax,[rip+X]; test rax,rax; jnz
                if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x05 && q[7] == 0x48 && q[8] == 0x85 && q[9] == 0xC0 && q[10] == 0x75) {
                    uint8_t* slot = ResolveRipRelative(q, 3, 7);
                    LOGI("[singleton] %s: getter at exe+0x%llX -> slot exe+0x%llX", name, ExeOff(q), ExeOff(slot));
                    return slot;
                }
            }
        }
    }
    LOGW("[singleton] \"%s\": name found but no null-check getter references it", name);
    return nullptr;
}

// ---- menu-open state ---------------------------------------------------------------
// Readers, all of the game's OWN menu state - none of them guess from our button presses:
//  menuman CSMenuManImp+0x1C bit 0x10 - measured 2026-08-24 on 2.6.2.0: flips 00<->10 on the
//          exact frame the pause menu opens and closes (7/7 START cycles, unattended run), in
//          step with the cursor UI element (+0xAC, ui_states[0x1C]: 00<->07). CSMenuManImp is
//          the heap object [MenuNav] MenuAOB resolves (RTTI-checked: CS::CSMenuManImp).
//  hud     CSFeManImp.hud_state (+0x78, u8): 0 HideAll, 1 ShowAll (esc menu), 2 PopupMenu (talk
//          menus: grace, shops, level-up, dialogs), 3 Default. Layout: fromsoftware-rs fe_man.rs;
//          measured 3<->1 in step with menuman. Object reached from the "CSFeMan" wrapper slot.
//  chain   the hand-configured [MenuNav] MenuChain byte on the CSMenuManImp object (legacy).
// (The game's pad routing - CSInGamePad.allow_polling - was tried too: the CSPad objects are not
// reachable from the FD4PadManager static on this build, and the two readers above suffice.)
// A reader is only TRUSTED once it has been seen in both states (a byte that never moves is
// either the wrong byte or a setting), so a wrong guess degrades to the START fallback instead
// of freezing the stick. Source=auto: chain if trusted, else "any trusted reader says open".
namespace {
uint8_t* g_menuBasePtr = nullptr;     // address of the CSMenuManImp* global (MenuAOB)
std::vector<int> g_menuChain;
std::string g_menuChainStr, g_menuAobStr;
bool g_menuInitTried = false;
double g_menuReinitAt = 0;
int  g_menuLast = -2;
uint8_t* g_menuManSlot = nullptr;     // CSMenuMan getter slot (holds the FD4Singleton runtime-class descriptor)
uint8_t* g_menuManImp = nullptr;      // CS::CSMenuManImp instance
void**   g_menuManImpVt = nullptr;
std::atomic<uint8_t*> g_feManImp{nullptr};   // CS::CSFeManImp instance (published by the background lookup)
std::atomic<void**>   g_feManImpVt{nullptr};
std::atomic<bool>     g_feLookupRunning{false};
uint8_t  g_seenMm = 0, g_seenHud = 0, g_seenChain = 0;   // bit0 = seen closed, bit1 = seen open
int g_lastMm = -2, g_lastHud = -2, g_lastChain = -2;
uint8_t g_lastMmRaw = 0xFF, g_lastMmCursor = 0xFF, g_lastHudRaw = 0xFF;
const char* g_lastVia = "";

constexpr int kMenuManImpState     = 0x1C;   // CSMenuManImp: bit 0x10 = a menu is open (measured)
// Bit 0x20: a site of grace's menu (and a talk menu) is open. Three players' 0.7.5 logs: it rises within 40 ms of
// every grace "Rest" press (action buttons 6000/6280) and falls when the menu closes 10-25 s later, while 0x10 and
// the HUD reader stay "closed" - so ERVR kept turning the view with the right stick and treating the left stick
// as walking there, and the menu only moved with the left stick click held (Paolo, 2026-11-07).
constexpr int kMenuManImpStateBit  = 0x10 | 0x20;
constexpr int kMenuManImpCursor    = 0xAC;   // CSMenuManImp.ui_states[0x1C]: cursor element (bit1 = visible)
constexpr int kFeManHudState       = 0x78;   // CSFeManImp.hud_state

// Byte-flip scanner ([MenuNav] Scan=true): watches the game objects the readers are built on
// and logs every byte that changes, so a new build's offsets can be read straight off the log.
struct ScanWatch {
    const char* tag; uint8_t** obj; size_t size;
    uint8_t* watched = nullptr; std::vector<uint8_t> prev; std::vector<uint16_t> flips; double window = 0;
};
uint8_t* g_feManImpPlain = nullptr;   // scanner's copy of the atomic
ScanWatch g_watches[] = { {"CSMenuManImp", &g_menuManImp, 0}, {"CSFeManImp", &g_feManImpPlain, 0x100} };
double g_scanNext = 0;

// CSFeManImp: its instance pointer is a static somewhere in .data (the getter slot only holds
// the FD4Singleton runtime-class descriptor, and the neighbouring statics are other managers'),
// so it is found the general way - CS::CSFeManImp vtable, then the static that points at an
// object with that vtable. That whole-module walk costs seconds, so it runs once on its own
// thread; until it lands the hud reader just reports unknown. Results are vtable-checked on use.
void StartFeManLookup() {
    if (g_feManImp.load() || g_feLookupRunning.exchange(true)) return;
    std::thread([] {
        void** vt = FindVTable("CS::CSFeManImp");
        auto inst = vt ? FindInstancesOfVTable(vt, 1) : std::vector<void*>{};
        if (!inst.empty()) { g_feManImpVt.store(vt); g_feManImp.store((uint8_t*)inst[0]); LOGI("[menu] CSFeManImp %p (background lookup) - hud reader armed", inst[0]); }
        else LOGW("[menu] CSFeManImp not found (vtable %p) - hud reader off", (void*)vt);
        g_feLookupRunning.store(false);
    }).detach();
}

// The object of RTTI class `rttiName` reachable from a getter slot: the slot's object itself, a
// pointer inside it, or (the wrapper case) a pointer one hop further; the static neighbourhood
// of the slot is searched as well. Probe reads only - never faults - and microseconds, unlike
// a whole-module RTTI scan.
uint8_t* FindImpFrom(uint8_t* slot, const char* rttiName, void*** vtOut, bool verbose) {
    if (!slot) return nullptr;
    std::string names;   // RTTI classes met on the way (logged when the search fails, to see what IS there)
    auto classOf = [&](uint8_t* q, void*** vt) -> bool {
        void** v = nullptr;
        if (!ProbeRead(q, &v, sizeof(v)) || !v || !InMainModule(v) || ((uintptr_t)v & 7)) return false;
        std::string nm = RttiNameOfVTable(v);
        if (nm.empty()) return false;
        if (nm != rttiName) { if (names.size() < 600 && names.find(nm) == std::string::npos) { names += nm; names += ' '; } return false; }
        if (vt) *vt = v;
        return true;
    };
    struct Node { uint8_t* p; size_t len; int depth; };
    std::vector<Node> queue; std::vector<uint8_t*> seen;
    uint8_t* root = nullptr;
    if (SafeRead(slot, &root, sizeof(root)) && root) {
        if (classOf(root, vtOut)) { LOGI("[menu] %s is the slot object %p", rttiName, (void*)root); return root; }
        queue.push_back({root, 0x100, 0}); seen.push_back(root);
    }
    queue.push_back({slot - 0x200, 0x600, 0});   // statics next to the slot (the instance pointers sit near the descriptors)
    for (size_t qi = 0; qi < queue.size() && qi < 300; ++qi) {
        Node nd = queue[qi];
        std::vector<uint8_t> buf(nd.len);
        if (!ProbeRead(nd.p, buf.data(), nd.len)) { nd.len = 0x40; if (!ProbeRead(nd.p, buf.data(), nd.len)) continue; }
        for (size_t off = 0; off + 8 <= nd.len; off += 8) {
            uintptr_t v; memcpy(&v, buf.data() + off, 8);
            if (!PlausiblePtr(v)) continue;
            uint8_t* q = (uint8_t*)v;
            if (classOf(q, vtOut)) { LOGI("[menu] %s at %p (hop %d, +0x%zX of %p)", rttiName, (void*)q, nd.depth + 1, off, (void*)nd.p); return q; }
            if (nd.depth < 2 && std::find(seen.begin(), seen.end(), q) == seen.end()) { seen.push_back(q); queue.push_back({q, 0x100, nd.depth + 1}); }
        }
    }
    if (verbose) LOGW("[menu] %s not reachable from slot exe+0x%llX (classes met: %s)", rttiName, ExeOff(slot), names.empty() ? "none" : names.c_str());
    return nullptr;
}
}

uint8_t* FindSingletonInstance(const char* name, const char* rttiName, bool verbose, void*** vtOut) {
    uint8_t* slot = FindSingletonSlot(name);
    return slot ? FindImpFrom(slot, rttiName, vtOut, verbose) : nullptr;
}

void GameMenuInit() {
    const Config& c = Cfg();
    g_menuInitTried = true;
    g_menuBasePtr = nullptr;
    g_menuAobStr = c.menuAob; g_menuChainStr = c.menuChain;
    g_menuChain = ParseChain(c.menuChain);
    if (!g_menuManSlot) g_menuManSlot = FindSingletonSlot("CSMenuMan");   // fallback route to CSMenuManImp (its pointer is 0x50 before this slot)
    g_menuManImp = nullptr; g_menuManImpVt = nullptr;
    g_seenMm = g_seenHud = g_seenChain = 0;
    for (auto& w : g_watches) { w.watched = nullptr; w.prev.clear(); }
    if (!c.menuAob.empty()) {
        uint8_t* hit = AobScan(c.menuAob);
        if (!hit) LOGW("[menu] MenuAOB not found in eldenring.exe");
        else {
            g_menuBasePtr = ResolveRipRelative(hit, c.menuAobOffset, c.menuAobInstrLen);
            uint8_t* obj = nullptr; void** vt = nullptr;
            if (SafeRead(g_menuBasePtr, &obj, sizeof(obj)) && obj && SafeRead(obj, &vt, sizeof(vt)) && vt && InMainModule(vt)) {
                std::string nm = RttiNameOfVTable(vt);
                if (nm == "CS::CSMenuManImp") { g_menuManImp = obj; g_menuManImpVt = vt; }
                else LOGW("[menu] MenuAOB object %p is a %s, not CS::CSMenuManImp - ignoring it", (void*)obj, nm.empty() ? "?" : nm.c_str());
            }
            LOGI("[menu] AOB hit exe+0x%llX -> slot exe+0x%llX -> CSMenuManImp %p, MenuChain of %zu offsets", ExeOff(hit), ExeOff(g_menuBasePtr), (void*)g_menuManImp, g_menuChain.size());
        }
    }
    if (!g_menuManImp) g_menuManImp = FindImpFrom(g_menuManSlot, "CS::CSMenuManImp", &g_menuManImpVt, true);
    StartFeManLookup();
}

// vtable-checked object access: a torn-down / re-created manager reads as "unknown", not garbage.
static uint8_t* LiveObject(uint8_t* obj, void** vt) {
    void** v = nullptr;
    if (!obj || !vt || !SafeRead(obj, &v, sizeof(v)) || v != vt) return nullptr;
    return obj;
}

static void MenuScanTick(double now) {
    const Config& c = Cfg();
    if (now < g_scanNext) return;                  // 20 Hz compare
    g_scanNext = now + 0.05;
    for (auto& w : g_watches) {
        uint8_t* obj = *w.obj;
        if (!obj) continue;
        size_t n = w.size ? w.size : (size_t)Clamp((float)c.menuScanSize, 16.f, 65536.f);
        std::vector<uint8_t> cur(n);
        if (!ProbeRead(obj, cur.data(), n)) continue;
        if (obj != w.watched || w.prev.size() != n) {
            w.watched = obj; w.prev = cur; w.flips.assign(n, 0); w.window = now;
            LOGI("[menuscan] watching %s object %p (%zu bytes); open/close menus and look for offsets that flip once each way", w.tag, (void*)obj, n);
            continue;
        }
        for (size_t i = 0; i < n; ++i) {
            if (cur[i] == w.prev[i]) continue;
            if (w.flips[i] < 65535) ++w.flips[i];
            if (w.flips[i] <= 3)   // stop reporting noisy offsets (counters, timers)
                LOGI("[menuscan] %s +0x%zX: %02X -> %02X", w.tag, i, w.prev[i], cur[i]);
        }
        w.prev = cur;
        if (now - w.window > 10.0) { w.flips.assign(n, 0); w.window = now; }
    }
}

// menuman reader: CSMenuManImp+0x1C bits 0x10 (menus) and 0x20 (grace and talk menus); the cursor element state
// rides along for the log.
static int MenuManOpen(uint8_t* raw, uint8_t* cursor) {
    uint8_t* obj = LiveObject(g_menuManImp, g_menuManImpVt);
    uint8_t v = 0, cur = 0;
    if (!obj || !SafeRead(obj + kMenuManImpState, &v, 1)) return -1;
    SafeRead(obj + kMenuManImpCursor, &cur, 1);
    *raw = v; *cursor = cur;
    return (v & kMenuManImpStateBit) ? 1 : 0;
}

// Chain reader: the configured byte, nonzero = open.
static int MenuChainOpen() {
    if (!g_menuBasePtr || g_menuChain.empty()) return -1;
    uint8_t* flag = FollowChain(g_menuBasePtr, g_menuChain);
    uint8_t v = 0;
    if (!flag || !SafeRead(flag, &v, 1)) return -1;
    return v ? 1 : 0;
}

// HUD-state reader. Returns -1 when unresolved, else 0/1; raw byte in *raw.
static int MenuHudOpen(uint8_t* raw) {
    g_feManImpPlain = g_feManImp.load();
    uint8_t* obj = LiveObject(g_feManImpPlain, g_feManImpVt.load());
    uint8_t v = 0;
    if (!obj || !SafeRead(obj + kFeManHudState, &v, 1)) return -1;
    *raw = v;
    if (v == 1 || v == 2) return 1;
    if (v == 0 || v == 3) return 0;
    return -1;
}

int GameMenuOpen(double now) {
    const Config& c = Cfg();
    if (!g_menuInitTried || c.menuAob != g_menuAobStr || c.menuChain != g_menuChainStr) { GameMenuInit(); g_menuReinitAt = now + 10.0; }
    if (c.menuScan) MenuScanTick(now);

    uint8_t mmRaw = 0xFF, mmCursor = 0xFF, hudRaw = 0xFF;
    int chain = MenuChainOpen();
    int mm = MenuManOpen(&mmRaw, &mmCursor);
    int hud = MenuHudOpen(&hudRaw);
    if (mm < 0 && hud < 0 && now >= g_menuReinitAt) { GameMenuInit(); g_menuReinitAt = now + 10.0; }   // managers re-created? look again
    auto seen = [](uint8_t& mask, int v) { if (v == 0) mask |= 1; else if (v == 1) mask |= 2; return mask == 3; };
    bool chainLive = seen(g_seenChain, chain), mmLive = seen(g_seenMm, mm), hudLive = seen(g_seenHud, hud);

    int open = -1; const char* via = "start-fallback";
    const std::string& src = c.menuSource;
    if      (src == "chain")   { open = chain; via = "chain"; }
    else if (src == "menuman") { open = mm;    via = "menuman"; }
    else if (src == "hud")     { open = hud;   via = "hud"; }
    else if (src == "start")   { }
    else if (chain >= 0 && chainLive) { open = chain; via = "chain"; }
    else if (mmLive || hudLive) {     // auto: any trusted reader saying open counts (talk menus, map: whichever flips)
        open = (mmLive && mm == 1) || (hudLive && hud == 1) ? 1 : 0;
        via = mmLive && hudLive ? "menuman+hud" : mmLive ? "menuman" : "hud";
    }
    if (open != g_menuLast || mm != g_lastMm || hud != g_lastHud || chain != g_lastChain ||
        mmRaw != g_lastMmRaw || mmCursor != g_lastMmCursor || hudRaw != g_lastHudRaw || via != g_lastVia) {
        LOGI("[menu] open=%d via %s | menuman=%d (state %02X cursor %02X%s) hud=%d (raw %u%s) chain=%d%s",
             open, via, mm, mmRaw, mmCursor, mmLive ? ", trusted" : "",
             hud, hudRaw, hudLive ? ", trusted" : "",
             chain, chainLive ? " (trusted)" : "");
        g_menuLast = open; g_lastMm = mm; g_lastHud = hud; g_lastChain = chain;
        g_lastMmRaw = mmRaw; g_lastMmCursor = mmCursor; g_lastHudRaw = hudRaw; g_lastVia = via;
    }
    return open;
}
}
