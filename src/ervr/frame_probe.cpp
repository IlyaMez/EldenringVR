// M2 spike: map ELDEN RING's frame task system.
//
// True stereo needs the scene drawn twice from two cameras at ONE simulation time. Every
// mechanism for that goes through Dantelion2's task scheduler: the frame is a strictly
// ordered list of NAMED task groups, all simulation lives upstream of `Draw_Pre`, and the
// draw itself is `DrawStep, DrawBegin, GameSceneDraw, AdhocDraw, DrawEnd`. If those can be
// re-driven with a second camera, we get stereo for ~1x CPU and 2x GPU. If they cannot, we
// fall back to double-stepping the whole engine with a banked delta time.
//
// Before any of that we have to know the table is really there on eldenring.exe 2.6.2.0 and
// that its order matches the map libER documents (Dasaav, Apache-2.0 w/ LLVM exceptions;
// include/coresystem/taskgroups.inl). This file only READS - it locates the table, walks it,
// and reports what it found. Nothing here changes game state.
#include "frame_probe.h"
#include "game_er.h"
#include "game_memory.h"
#include "rtti.h"
#include "reshade_addon.h"
#include "config.h"
#include "log.h"
#include <MinHook.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace ervr {
namespace {

// libER: CS::CSTaskBase::cstg_control { cstgi id; wchar_t name[64]; bool active; }
// The stride is discovered at runtime rather than assumed - padding is a compiler choice.
constexpr size_t kNameOff = 4;
constexpr size_t kActiveOff = kNameOff + 64 * sizeof(wchar_t);   // 132, confirmed on 2.6.2.0
constexpr int    kMaxGroups = 256;

TaskGroupTable g_table;

// The frame order libER documents. Used only to VERIFY what we find; the live table wins.
const wchar_t* kExpected[] = {
    L"FrameBegin", L"SteamThread0", L"SteamThread1", L"SteamThread2", L"SteamThread3",
    L"SteamThread4", L"SteamThread5", L"SystemStep", L"ResStep", L"PadStep", L"GameFlowStep",
    L"EndShiftWorldPosition", L"GameMan", L"TaskLineIdx_Sys", L"TaskLineIdx_Test",
    L"TaskLineIdx_NetworkFlowStep", L"TaskLineIdx_InGame_InGameStep",
    L"TaskLineIdx_InGame_InGameStayStep", L"MovieStep", L"RemoStep",
    L"TaskLineIdx_InGame_MoveMapStep", L"FieldArea_EndWorldAiManager", L"EmkSystem_Pre",
    L"EmkSystem_ConditionStatus", L"EmkSystem_Post", L"EventMan",
    L"FlverResDelayDelectiionBegin", L"TaskLineIdx_InGame_FieldAreaStep",
    L"TaskLineIdx_InGame_TestNetStep", L"TaskLineIdx_InGame_InGameMenuStep",
    L"TaskLineIdx_InGame_TitleMenuStep", L"TaskLineIdx_InGame_CommonMenuStep",
    L"TaskLineIdx_FrpgNet_Sys", L"TaskLineIdx_FrpgNet_Lobby", L"TaskLineIdx_FrpgNet_ConnectMan",
    L"TaskLineIdx_FrpgNet_Connect", L"TaskLineIdx_FrpgNet_Other", L"SfxMan", L"FaceGenMan",
    L"FrpgNetMan", L"NetworkUserManager", L"SessionManager", L"BlockList", L"LuaConsoleServer",
    L"RmiMan", L"ResMan", L"SfxDebugger", L"REMOTEMAN", L"Geom_WaitActivateFade",
    L"Geom_UpdateDraw", L"Grass_BatchUpdate", L"Grass_ResourceLoadKick", L"Grass_ResourceLoad",
    L"Grass_ResourceCleanup", L"WorldChrMan_Respawn", L"WorldChrMan_Prepare",
    L"ChrIns_CalcUpdateInfo_PerfBegin", L"ChrIns_CalcUpdateInfo", L"ChrIns_CalcUpdateInfo_PerfEnd",
    L"WorldChrMan_PrePhysics", L"WorldChrMan_CalcOmissionLevel_Begin",
    L"WorldChrMan_CalcOmissionLevel", L"WorldChrMan_CalcOmissionLevel_End",
    L"WorldChrMan_ConstructUpdateList", L"WorldChrMan_ChrNetwork", L"ChrIns_Prepare",
    L"ChrIns_NaviCache", L"ChrIns_AILogic_PerfBegin", L"ChrIns_AILogic", L"ChrIns_AILogic_PerfEnd",
    L"AI_SimulationStep", L"ChrIns_PreBehavior", L"ChrIns_PreBehaviorSafe",
    L"GeomModelInsCreatePartway_Begin", L"HavokBehavior", L"GeomModelInsCreatePartway_End",
    L"ChrIns_BehaviorSafe", L"ChrIns_PrePhysics_Begin", L"ChrIns_PrePhysics",
    L"ChrIns_PrePhysics_End", L"NetFlushSendData", L"ChrIns_PrePhysicsSafe", L"ChrIns_RagdollSafe",
    L"ChrIns_GarbageCollection", L"GeomModelInsCreate", L"AiBeginCollectGabage",
    L"WorldChrMan_Update_RideCheck", L"InGameDebugViewer", L"LocationStep",
    L"LocationUpdate_PrePhysics", L"LocationUpdate_PrePhysics_Parallel",
    L"LocationUpdate_PrePhysics_Post", L"LocationUpdate_PostCloth",
    L"LocationUpdate_PostCloth_Parallel", L"LocationUpdate_PostCloth_Post",
    L"LocationUpdate_DebugDraw", L"EventCondition_BonfireNearEnemyCheck", L"HavokWorldUpdate_Pre",
    L"RenderingSystemUpdate", L"HavokWorldUpdate_Post", L"ChrIns_PreCloth", L"ChrIns_PreClothSafe",
    L"HavokClothUpdate_Pre_AddRemoveRigidBody", L"HavokClothUpdate_Pre_ClothModelInsSafe",
    L"HavokClothUpdate_Pre_ClothModelIns", L"HavokClothUpdate_Pre_ClothManager", L"CameraStep",
    L"DrawParamUpdate", L"GetNPAuthCode", L"SoundStep", L"HavokClothUpdate_Post_ClothManager",
    L"HavokClothUpdate_Post_ClothModelIns", L"HavokClothVertexUpdateFinishWait",
    L"ChrIns_PostPhysics", L"ChrIns_PostPhysicsSafe", L"CSDistViewManager_Update",
    L"HavokAi_SilhouetteGeneratorHelper_Begin", L"WorldChrMan_PostPhysics",
    L"GameFlowInGame_MoveMap_PostPhysics_0", L"HavokAi_SilhouetteGeneratorHelper_End",
    L"DmgMan_Pre", L"DmgMan_ShapeCast", L"DmgMan_Post",
    L"GameFlowInGame_MoveMap_PostPhysics_1_Core0", L"GameFlowInGame_MoveMap_PostPhysics_1_Core1",
    L"GameFlowInGame_MoveMap_PostPhysics_1_Core2", L"MenuMan",
    L"WorldChrMan_Update_BackreadRequestPre", L"ChrIns_Update_BackreadRequest",
    L"WorldChrMan_Update_BackreadRequestPost", L"HavokAi_World",
    L"WorldAiManager_BeginUpdateFormation", L"WorldAiManager_EndUpdateFormation",
    L"GameFlowInGame_TestNet", L"GameFlowInGame_InGameMenu", L"GameFlowInGame_TitleMenu",
    L"GameFlowInGame_CommonMenu", L"GameFlowFrpgNet_Sys", L"GameFlowFrpgNet_Lobby",
    L"GameFlowFrpgNet_ConnectMan", L"GameFlowFrpgNet_Connect", L"GameFlowStep_Post",
    L"ScaleformStep", L"FlverResDelayDelectiionEnd", L"Draw_Pre", L"GraphicsStep",
    L"DebugDrawMemoryBar", L"DbgMenuStep", L"DbgRemoteStep", L"PlaylogSystemStep", L"ReviewMan",
    L"ReportSystemStep", L"DbgDispStep", L"DrawStep", L"DrawBegin", L"GameSceneDraw",
    L"AdhocDraw", L"DrawEnd", L"Draw_Post", L"SoundPlayLimitterUpdate",
    L"BeginShiftWorldPosition", L"FileStep", L"FileStepUpdate_Begin", L"FileStepUpdate_End",
    L"Flip", L"DelayDeleteStep", L"AiEndCollectGabage", L"RecordHeapStats", L"FrameEnd",
};
constexpr int kExpectedCount = (int)(sizeof(kExpected) / sizeof(kExpected[0]));

// A snapshot of every readable committed region, sorted, so validity checks are a binary
// search instead of a syscall. The discovery pass makes millions of them; at one VirtualQuery
// each it would take minutes.
struct Region { uintptr_t lo, hi; };
std::vector<Region> g_regions;

void BuildRegionMap() {
    g_regions.clear();
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uint8_t* p = (uint8_t*)si.lpMinimumApplicationAddress;
    uint8_t* const hi = (uint8_t*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi{};
    while (p < hi && VirtualQuery(p, &mbi, sizeof(mbi))) {
        uint8_t* base = (uint8_t*)mbi.BaseAddress;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            g_regions.push_back({(uintptr_t)base, (uintptr_t)base + mbi.RegionSize});
        uint8_t* next = base + mbi.RegionSize;
        if (next <= p) break;
        p = next;
    }
    std::sort(g_regions.begin(), g_regions.end(),
              [](const Region& a, const Region& b) { return a.lo < b.lo; });
}

bool Readable(const void* p, size_t n) {
    const uintptr_t a = (uintptr_t)p;
    if (g_regions.empty()) {   // before the map is built, fall back to the syscall
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        return a + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    auto it = std::upper_bound(g_regions.begin(), g_regions.end(), a,
                               [](uintptr_t v, const Region& r) { return v < r.lo; });
    if (it == g_regions.begin()) return false;
    --it;
    return a >= it->lo && a + n <= it->hi;
}

// Wide-string compare that tolerates a non-terminated fixed field.
bool NameIs(const uint8_t* entry, const wchar_t* want) {
    const wchar_t* got = (const wchar_t*)(entry + kNameOff);
    for (int i = 0; i < 64; ++i) {
        if (got[i] != want[i]) return false;
        if (want[i] == 0) return true;
    }
    return false;
}

uint64_t g_bytesScanned = 0;

// One region's worth of searching, isolated so the SEH frame contains no C++ objects.
const uint8_t* ScanRegion(const uint8_t* base, size_t size, const uint8_t* needle, size_t n) {
    __try {
        for (size_t i = 0; i + n <= size; ++i)
            if (base[i] == needle[0] && memcmp(base + i, needle, n) == 0) return base + i;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

// Walks committed private memory for the first byte sequence `needle`.
//
// This is measured in gigabytes on a running ELDEN RING, so it MUST NOT run on the worker
// thread - that thread also drives BackendsTick(), and blocking it means the OpenXR instance
// is never created and the headset never comes up at all.
uint8_t* ScanHeap(const uint8_t* needle, size_t n, size_t maxRegion, uint8_t* startAt,
                  uint64_t deadlineTick) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uint8_t* p = startAt ? startAt : (uint8_t*)si.lpMinimumApplicationAddress;
    uint8_t* const hi = (uint8_t*)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi{};
    while (p < hi && VirtualQuery(p, &mbi, sizeof(mbi))) {
        uint8_t* base = (uint8_t*)mbi.BaseAddress;
        size_t   size = mbi.RegionSize;
        const bool usable = mbi.State == MEM_COMMIT && (mbi.Type & MEM_PRIVATE) &&
                            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                            (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY |
                                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
        if (usable && size >= n && size <= maxRegion) {
            if (GetTickCount64() > deadlineTick) {
                LOGW("[m2] scan deadline hit after %llu MB", (unsigned long long)(g_bytesScanned >> 20));
                return nullptr;
            }
            g_bytesScanned += size;
            if (const uint8_t* hit = ScanRegion(base, size, needle, n)) return (uint8_t*)hit;
        }
        uint8_t* next = base + size;
        if (next <= p) break;
        p = next;
    }
    return nullptr;
}

// Finds a qword anywhere in committed private memory whose value is `target`. Used to walk
// back from the task-group ARRAY to the vector that owns it, and from there to the object.
uint8_t* FindPointerTo(const void* target, size_t maxRegion, uint64_t deadlineTick) {
    uint8_t needle[8];
    memcpy(needle, &target, 8);
    return ScanHeap(needle, 8, maxRegion, nullptr, deadlineTick);
}

// A real vtable's slots all point into the module's EXECUTABLE sections. The first attempt at
// this just read *obj and printed it, which produced convincing-looking nonsense - vft[0] came
// back equal to `this`. Verify before believing.
bool ExecRange(const void* p, uint8_t*& lo, uint8_t*& hi) {
    static uint8_t* s_lo = nullptr; static uint8_t* s_hi = nullptr;
    if (!s_lo) {
        auto* base = (uint8_t*)GetModuleHandleW(nullptr);
        auto* dos = (IMAGE_DOS_HEADER*)base;
        auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        auto* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            uint8_t* a = base + sec->VirtualAddress;
            uint8_t* b = a + sec->Misc.VirtualSize;
            if (!s_lo || a < s_lo) s_lo = a;
            if (!s_hi || b > s_hi) s_hi = b;
        }
    }
    lo = s_lo; hi = s_hi;
    return p >= s_lo && p < s_hi;
}

bool LooksLikeVTable(void** vt, int need) {
    if (!Readable(vt, (size_t)need * 8)) return false;
    uint8_t *lo, *hi;
    for (int i = 0; i < need; ++i)
        if (!ExecRange(vt[i], lo, hi)) return false;
    return true;
}

// Given a candidate object, look for the task_groups vector inside it: a { begin, end, cap }
// triple whose begin points at an entry that reads { id, L"FrameBegin" }. Returns the array
// and measures the element stride from where entry 1 lands.
//
// SEH-guarded, and that is not belt-and-braces: the region map is a SNAPSHOT, ELDEN RING's
// allocator churns constantly, and across ~100k candidates something will inevitably be freed
// between the check and the read. A fault here just means "not this one".
// Locals are deliberately all POD so the __try is legal.
int VectorInObject(uint8_t* obj, size_t searchBytes, uint8_t** entry0, size_t* stride,
                   size_t* vecOff) {
    __try {
        // Deliberately NO "first slot must be a vtable" pre-filter. The static that leads here
        // points 0x18 BEFORE the object's start, so slot 0 is a plain field - that filter threw
        // the real hit away and turned a working search into a silent miss.
        for (size_t off = 0; off + 8 <= searchBytes; off += 8) {
            if (!Readable(obj + off, 8)) continue;
            uint8_t* begin = *(uint8_t**)(obj + off);
            if (!begin || ((uintptr_t)begin & 7)) continue;
            if (!Readable(begin, kNameOff + 64 * sizeof(wchar_t))) continue;
            if (!NameIs(begin, kExpected[0])) continue;
            for (size_t s = 132; s <= 200; s += 4) {
                if (!Readable(begin + s, kNameOff + 64 * sizeof(wchar_t))) continue;
                if (!NameIs(begin + s, kExpected[1])) continue;
                *entry0 = begin; *stride = s; *vecOff = off;
                return 1;
            }
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Returns true when the probe is finished (found it, or gave up).
bool RunOnce(int attempt) {
    LOGI("[m2] ---- frame task-system probe (attempt %d) ----", attempt);
    g_bytesScanned = 0;
    const uint64_t t0 = GetTickCount64();

    uint8_t* entry0 = nullptr;
    uint8_t* taskObj = nullptr;
    size_t   stride = 0, vecOff = 0;

    // 1) Discovery through the module's own statics. Dantelion2 keeps its managers in globals,
    //    so the object is reachable from a few MB of .data - as opposed to hunting the string
    //    in the heap, which on a live ELDEN RING means touching gigabytes of committed-but-cold
    //    pages at ~9 MB/s while evicting the game's own working set.
    //
    //    Deliberately NOT driven by RTTI class names. 640 classes match "Task" on this build,
    //    FindVTable() is a full section scan EACH (~2 s, so ~20 minutes for the set), and MSVC's
    //    mangling of nested template classes does not survive a naive demangle anyway. The
    //    object's CONTENT identifies it far better than its name: exactly one object in the
    //    process holds a vector whose first element is named L"FrameBegin".
    BuildRegionMap();

    // 0) Named RTTI first. The concrete class is CS::CSTaskImp - NOT "CS::CSTask", which has
    //    no type descriptor of its own. But the Imp is NOT the callee: per libER's task.hpp,
    //    CSTaskImp is just { vptr, CSTask* inst } - a singleton handle. The object that owns
    //    the task_groups vector and the add/set/queue virtuals is the CSTask at Imp+0x8.
    //    (The first attempt called the IMP's vtable with the CSTask's `this` and faulted.)
    uint8_t* cstaskFromImp = nullptr;
    if (void** vt = FindVTable("CS::CSTaskImp")) {
        std::vector<void*> insts = FindInstancesOfVTable(vt, 4);
        LOGI("[m2] rtti CS::CSTaskImp: vtable %p, %zu instance(s)", (void*)vt, insts.size());
        if (!insts.empty() && Readable((uint8_t*)insts[0] + 8, 8)) {
            g_table.imp = (uint8_t*)insts[0];
            cstaskFromImp = *(uint8_t**)((uint8_t*)insts[0] + 8);
            LOGI("[m2] CSTaskImp::inst (the CSTask) = %p", (void*)cstaskFromImp);
        }
    }

    // The CSTask from the Imp is authoritative when it checks out: it must hold the vector
    // (libER: vptr@0, allocator@8, task_groups@0x10 with std::vector's allocator first, so
    // begin lands at +0x18 - exactly where the statics scan found it last time).
    if (cstaskFromImp) {
        const int r = VectorInObject(cstaskFromImp, 0x120, &entry0, &stride, &vecOff);
        if (r == 1) {
            taskObj = cstaskFromImp;
            LOGI("[m2] CSTaskImp::inst holds the table (vector at +0x%zx) - it IS the CSTask",
                 vecOff);
        } else {
            LOGW("[m2] CSTaskImp::inst does NOT hold the table (r=%d) - falling back to statics", r);
        }
    }

    std::vector<void*> statics = ModuleStaticPointers();
    long faults = 0;
    if (!taskObj) for (void* cand : statics) {
        if (!Readable(cand, 0x10)) continue;   // VectorInObject bounds-checks each slot itself
        const int r = VectorInObject((uint8_t*)cand, 0x120, &entry0, &stride, &vecOff);
        if (r < 0) { ++faults; continue; }
        if (r == 0) continue;
        taskObj = (uint8_t*)cand;
        LOGI("[m2] FOUND via module statics: object at %p, task_groups vector at +0x%zx "
             "(%zu candidates, %zu regions, %ld faults, %llu ms)",
             cand, vecOff, statics.size(), g_regions.size(), faults,
             (unsigned long long)(GetTickCount64() - t0));
        break;
    }
    if (taskObj && cstaskFromImp && taskObj != cstaskFromImp)
        LOGW("[m2] statics scan (%p) and CSTaskImp::inst (%p) DISAGREE about the CSTask - "
             "using the one that holds the vector", (void*)taskObj, (void*)cstaskFromImp);
    if (!taskObj)
        LOGW("[m2] no module static points at an object holding the table "
             "(%zu candidates, %ld faults, %llu ms); trying the heap",
             statics.size(), faults, (unsigned long long)(GetTickCount64() - t0));

    // 2) Fallback: hunt the array itself in the heap. Tightly budgeted - this is the path
    //    that thrashed the working set, so it only gets a small slice and small regions.
    if (!entry0) {
        const uint64_t deadline = GetTickCount64() + 12000;
        const wchar_t* first = kExpected[0];
        const size_t needleBytes = (wcslen(first) + 1) * sizeof(wchar_t);
        uint8_t* resume = nullptr;
        for (int probe = 0; probe < 8 && !entry0; ++probe) {
            uint8_t* hit = ScanHeap((const uint8_t*)first, needleBytes, 32u << 20, resume, deadline);
            if (!hit) break;
            resume = hit + 2;
            uint8_t* cand = hit - kNameOff;
            for (size_t s = 132; s <= 200 && !entry0; s += 4) {
                if (!Readable(cand + s, kNameOff + 64 * sizeof(wchar_t))) continue;
                if (NameIs(cand + s, kExpected[1])) { entry0 = cand; stride = s; }
            }
        }
        if (entry0) LOGI("[m2] found the table by heap scan (%llu MB)",
                         (unsigned long long)(g_bytesScanned >> 20));
    }

    if (!entry0) {
        LOGW("[m2] table not found this pass (%llu MB heap in %llu ms) - engine may still be booting",
             (unsigned long long)(g_bytesScanned >> 20),
             (unsigned long long)(GetTickCount64() - t0));
        return false;
    }
    LOGI("[m2] task-group table at %p, stride %zu bytes (id@0, name@%zu, active@%zu) in %llu ms",
         (void*)entry0, stride, kNameOff, kNameOff + 64 * sizeof(wchar_t),
         (unsigned long long)(GetTickCount64() - t0));

    // 2) The vector's `end` gives the real count. Walking until the name looks wrong overran
    //    into unrelated heap and reported 256 groups where there are 169.
    int listed = 0;
    if (taskObj && Readable(taskObj + vecOff, 24)) {
        uint8_t* vbegin = *(uint8_t**)(taskObj + vecOff);
        uint8_t* vend   = *(uint8_t**)(taskObj + vecOff + 8);
        if (vbegin == entry0 && vend > vbegin && (size_t)(vend - vbegin) % stride == 0) {
            listed = (int)((vend - vbegin) / stride);
            LOGI("[m2] vector says %d groups (begin %p end %p)", listed, (void*)vbegin, (void*)vend);
        }
    }

    // 3) Walk it, comparing against the documented order as we go.
    int count = 0, mismatches = 0;
    std::string line;
    char buf[160];
    const int walkMax = listed > 0 ? listed : kMaxGroups;
    for (int i = 0; i < walkMax; ++i) {
        uint8_t* e = entry0 + (size_t)i * stride;
        if (!Readable(e, stride)) break;
        const wchar_t* nm = (const wchar_t*)(e + kNameOff);
        if (nm[0] == 0) break;
        const int32_t id = *(const int32_t*)e;
        const uint8_t active = e[kNameOff + 64 * sizeof(wchar_t)];
        // A name field that is not NUL-terminated inside 64 chars means we walked off the end.
        bool terminated = false;
        for (int k = 0; k < 64; ++k) if (nm[k] == 0) { terminated = true; break; }
        if (!terminated) break;

        char nameA[80]{};
        WideCharToMultiByte(CP_UTF8, 0, nm, -1, nameA, sizeof(nameA) - 1, nullptr, nullptr);
        const bool known = i < kExpectedCount;
        const bool same = known && NameIs(e, kExpected[i]);
        if (known && !same) ++mismatches;
        snprintf(buf, sizeof(buf), "%d:%s%s%s ", id, nameA, active ? "" : "(off)", same || !known ? "" : "!");
        line += buf;
        if (line.size() > 150) { LOGI("[m2]   %s", line.c_str()); line.clear(); }
        ++count;
    }
    if (!line.empty()) LOGI("[m2]   %s", line.c_str());

    LOGI("[m2] %d groups live (libER documents %d); %d name mismatches%s",
         count, kExpectedCount, mismatches,
         mismatches == 0 && count == kExpectedCount
             ? " - the frame map matches this build EXACTLY, indices are usable as-is"
             : " - indices must be taken from the live table, not from libER's enum");

    // 3) The draw range is the whole point of the spike; call it out explicitly.
    for (int i = 0; i < count; ++i) {
        uint8_t* e = entry0 + (size_t)i * stride;
        for (const wchar_t* want : {L"CameraStep", L"Draw_Pre", L"GraphicsStep", L"DrawStep",
                                    L"DrawBegin", L"GameSceneDraw", L"AdhocDraw", L"DrawEnd",
                                    L"Draw_Post", L"Flip"}) {
            if (!NameIs(e, want)) continue;
            char nameA[80]{};
            WideCharToMultiByte(CP_UTF8, 0, want, -1, nameA, sizeof(nameA) - 1, nullptr, nullptr);
            // The id is NOT the index: it is 0x90000000 | index. set_task_group_state wants
            // the id, so print both.
            LOGI("[m2]   key group %-16s index %3d  id 0x%08X  entry %p  active %d",
                 nameA, i, (unsigned)*(const int32_t*)e, (void*)e,
                 e[kNameOff + 64 * sizeof(wchar_t)]);
        }
    }

    // 4) The object is what we actually need: `this` for set_task_group_state / queue_tasks.
    //    RTTI already gave it to us; the heap-scan fallback has to work backwards to it.
    if (!taskObj) {
        uint8_t* vecBegin = FindPointerTo(entry0, 32u << 20, GetTickCount64() + 12000);
        if (!vecBegin) {
            LOGW("[m2] table found but the owning object was not; no `this` for the vtable calls");
        } else {
            auto* base = (uint8_t*)GetModuleHandleW(nullptr);
            auto* modEnd = base + 0x10000000ull;
            for (size_t back = 8; back <= 64; back += 8) {
                uint8_t* obj = vecBegin - back;
                if (!Readable(obj, 8)) continue;
                uint8_t* maybeVt = *(uint8_t**)obj;
                if (maybeVt < base || maybeVt >= modEnd || !Readable(maybeVt, 8)) continue;
                uint8_t* fn = *(uint8_t**)maybeVt;
                if (fn < base || fn >= modEnd) continue;
                taskObj = obj;
                LOGI("[m2] owning object likely at %p (vector at +0x%zx)", (void*)obj, back);
                break;
            }
        }
    }
    if (taskObj) {
        LOGI("[m2] owner object `this` = %p", (void*)taskObj);
        // The callee vtable is the CSTask's OWN vptr at +0. CSTask : CSTaskBase is single
        // inheritance, and both declare a virtual destructor, so slot 0 is the dtor and the
        // libER-declared virtuals start at 1. The first attempt indexed them from 0 (and used
        // the Imp's vtable) - that is why "slot 2" faulted: it was unk_set_task_group on the
        // wrong object.
        void** vt = Readable(taskObj, 8) ? *(void***)taskObj : nullptr;
        if (vt && LooksLikeVTable(vt, 6)) {
            g_table.vtable = vt;
            auto* base = (uint8_t*)GetModuleHandleW(nullptr);
            const std::string cls = RttiNameOfVTable(vt);
            LOGI("[m2] CSTask vtable %p (rva 0x%llx) rtti \"%s\"", (void*)vt,
                 (unsigned long long)((uint8_t*)vt - base),
                 cls.empty() ? "<no rtti>" : cls.c_str());
            const char* names[6] = {"~dtor", "add_task_group", "unk_set_task_group",
                                    "set_task_group_state", "debug_print", "queue_tasks"};
            for (int i = 0; i < 6; ++i) {
                uint8_t *lo, *hi;
                if (!ExecRange(vt[i], lo, hi)) break;
                LOGI("[m2]   vft[%d] %-22s rva 0x%llx", i, names[i],
                     (unsigned long long)((uint8_t*)vt[i] - base));
            }
        } else {
            g_table.vtable = nullptr;
            LOGW("[m2] object at %p has no verified vtable at +0 - set_task_group_state stays "
                 "unavailable this run", (void*)taskObj);
        }
    }

    g_table.valid = true;
    g_table.owner = taskObj;
    g_table.entry0 = entry0;
    g_table.stride = stride;
    g_table.count = count;
    LOGI("[m2] ---- probe complete ----");
    return true;
}

std::atomic<bool> g_started{false};

// ---- experiment: register our own tasks into the frame ----------------------------------
// fromsoftware-rs (Dasaav, MIT/Apache-2.0; crates/eldenring/src/cs/task.rs + shared/task.rs,
// the same rva_ww.rs our validated camera RVAs come from) proves the game's internal
// registration function accepts ANY object shaped like { vft, pad } whose vtable is
// { get_runtime_class, destructor, execute(this, FD4TaskData*) } - no CSEzTask, no proxy,
// no RTTI. Registration: register_task(CSTaskImp* imp, uint32_t groupIndex, task*), plain
// 0-based group INDEX (not the 0x90000000|idx id). The engine then calls execute every time
// that group runs, which is the in-frame execution point everything in M3 hangs off.
constexpr uint32_t RVA_RegisterTask = 0xeb1fe0;   // exe 2.6.2.0 (rva_ww.rs `register_task`)

// FD4TaskData (libER fd4_task.hpp): FD4Time{vptr,float} then group id then runner seed.
struct FD4TaskDataView {
    void*    timeVft;    // +0x00
    float    dt;         // +0x08
    float    _pad;       // +0x0c
    uint32_t groupId;    // +0x10  (0x90000000 | index)
    int      seed;       // +0x14  task runner 0..5
};

struct ErvrFrameTask {
    void**      vft;           // +0 - the game calls through this
    uint64_t    unk8 = 0;      // +8 - present in the proven layout; never seen written
    // ours, invisible to the engine:
    const char* name = "";
    volatile long calls = 0;
    float       lastDt = 0.f;
    uint32_t    lastGroupId = 0;
    int         lastSeed = 0;
    DWORD       lastThread = 0;
};

// The registered probes the toggle experiment reads. Deliberately leaked: the engine keeps
// the pointer forever and there is no unregister on our side yet.
ErvrFrameTask* g_probeTask[3] = {};   // FrameBegin, GameSceneDraw, DrawEnd
volatile __int64 g_drawEndQpc = 0;    // stamped by the DrawEnd probe; read by the queue_tasks probe

void* TaskGetRuntimeClass(void*) { return nullptr; }
void* TaskDtor(void*, unsigned) { return nullptr; }   // engine never destroys our tasks
void  TaskExecute(ErvrFrameTask* self, FD4TaskDataView* d) {
    // Runs on the game's task-runner threads. Keep it exception-free and tiny.
    const long n = InterlockedIncrement(&self->calls);
    if (d) {
        self->lastDt = d->dt;
        self->lastGroupId = d->groupId;
        self->lastSeed = d->seed;
    }
    self->lastThread = GetCurrentThreadId();
    if (self == g_probeTask[2]) {
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        g_drawEndQpc = q.QuadPart;
    }
    if (n == 1) {
        LOGI("[m2] task \"%s\": FIRST call (dt %.5f, groupId 0x%08X, seed %d, thread %lu)",
             self->name, self->lastDt, self->lastGroupId, self->lastSeed, self->lastThread);
        // The return chain is the engine's task-driver: it leads from the runner up to
        // whoever calls CSTask::queue_tasks each frame - the function a second draw-only
        // pass will have to hook. Logged module-relative so it survives ASLR.
        void* frames[16] = {};
        const USHORT got = RtlCaptureStackBackTrace(1, 16, frames, nullptr);
        auto* base = (uint8_t*)GetModuleHandleW(nullptr);
        char line[512]; int p = 0;
        for (USHORT i = 0; i < got && p < (int)sizeof(line) - 24; ++i) {
            HMODULE m = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)frames[i], &m);
            if (m == (HMODULE)base)
                p += snprintf(line + p, sizeof(line) - p, " exe+0x%llx",
                              (unsigned long long)((uint8_t*)frames[i] - base));
            else
                p += snprintf(line + p, sizeof(line) - p, " [%p]", frames[i]);
        }
        LOGI("[m2] task \"%s\" call stack:%s", self->name, line);
    }
}
void* g_taskVft[3] = {(void*)&TaskGetRuntimeClass, (void*)&TaskDtor, (void*)&TaskExecute};

// SEH-wrapped engine call; POD locals only.
int CallRegisterTask(void* fn, void* imp, uint32_t groupIndex, void* task) {
    __try {
        ((void(__fastcall*)(void*, uint32_t, void*))fn)(imp, groupIndex, task);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

ErvrFrameTask* RegisterFrameTask(const char* name, int groupIndex) {
    if (!g_table.imp || groupIndex < 0) return nullptr;
    // The table matched libER's map exactly (169/169 names) before we get here, which is the
    // version gate: on any other build that check fails and this never runs.
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    auto* t = new ErvrFrameTask();
    t->vft = g_taskVft;
    t->name = name;
    const int r = CallRegisterTask(base + RVA_RegisterTask, g_table.imp, (uint32_t)groupIndex, t);
    if (r < 0) {
        LOGE("[m2] register_task(\"%s\", %d) FAULTED - RVA 0xeb1fe0 wrong for this build?", name, groupIndex);
        return nullptr;   // t leaks; the engine may already hold it
    }
    LOGI("[m2] register_task(\"%s\", index %d) returned; waiting for first call", name, groupIndex);
    return t;
}

// ---- experiment: probe CSTask::queue_tasks --------------------------------------------
// Disassembly (2.6.2.0, rva 0x268c5d0): queue_tasks walks the cstg_control table, and for
// every entry whose `active` byte is set calls the group-queue fn (0x269ef80) keyed by the
// group id via a map at arg3+0x18, then writes [this+0xe0] to runnerMgr+0xb8 and kicks the
// 6 task runners. It IS the once-per-frame frame driver, and the active flags are consumed
// here - which the toggle experiment confirmed (our gated task stopped running). The two
// unknowns that decide mechanism B' (a second, draw-only pass per frame):
//   1. WHO calls it, on which thread - the hook point for a double pump.
//   2. Is it SYNCHRONOUS - has the whole frame finished when it returns? Measured by
//      comparing its exit timestamp against our DrawEnd task's last-call timestamp.
using PfnQueueTasks = uint64_t(__fastcall*)(void*, void*, void*);
PfnQueueTasks g_origQueueTasks = nullptr;
volatile long g_qtCalls = 0;
volatile __int64 g_qtLastEnterQpc = 0, g_qtLastLeaveQpc = 0;
void* volatile g_qtArgA = nullptr;   // captured args: the frame-driver context queue_tasks
void* volatile g_qtArgB = nullptr;   // receives every frame (b+0x18 = id->task_entry_group map)

// ---- experiment: double-pump the frame driver -------------------------------------------
// Run 3 proved the whole frame executes SYNCHRONOUSLY inside queue_tasks (~5 ms at 60 Hz on
// the main-loop thread) and that the active bytes gate exactly the groups' REGISTERED task
// lists, while the core world draw is carried by machinery the bytes do not gate. Both facts
// together make this the true-stereo mechanism: after the real frame returns, gate the sim
// range, call the frame driver AGAIN, restore. Pass 2 re-runs the frame chain (draw included)
// with every simulation task list empty - a draw-only engine frame at the same sim time.
volatile long g_pumpMode = 0;    // 0 off | 1 experiment: unconditional | 2 stereo: when a 2nd eye is stashed
volatile long g_pumpPasses = 0;
std::atomic<int> g_curPass{1};   // 1 = real frame, 2 = draw-only second pass
std::atomic<float> g_pumpLastMs{0.f};
DWORD g_qtThread = 0;            // the main-loop thread queue_tasks runs on

// Pass windows for the present-pairing probe: while g_winTrace is on, every pass-1 and
// pass-2 execution records its [enter, exit] QPC window.
struct PassWin { long long t0, t1; };
std::atomic<bool> g_winTrace{false};
PassWin g_p1Win[256], g_p2Win[256];
std::atomic<int> g_p1N{0}, g_p2N{0};

uint8_t ReadActiveRaw(int i) { return g_table.entry0[(size_t)i * g_table.stride + kActiveOff]; }
void    WriteActiveRaw(int i, uint8_t v) { g_table.entry0[(size_t)i * g_table.stride + kActiveOff] = v; }

void DoSecondPass(void* self, void* a, void* b) {
    // Stereo mode: only pump frames for which the game stashed the second eye's camera -
    // and write that camera FIRST, while every writer is quiescent. Menus, loading and
    // cinema mode simply never stash one, so those frames stay single-pass.
    if (g_pumpMode == 2 && !GameERApplySecondEyeCamera()) return;
    // We are on the main-loop thread between two frames; the runners are parked. Keep the
    // draw phase (Draw_Pre 144 .. Draw_Post 158) and Flip (164); gate everything else, which
    // also covers input, menus, sound, streaming and the Steam threads - pass 2 must not
    // advance anything.
    uint8_t saved[256];
    const int n = g_table.count < 256 ? g_table.count : 256;
    for (int i = 0; i < n; ++i) saved[i] = ReadActiveRaw(i);
    for (int i = 0; i < n; ++i) {
        const bool keep = (i >= 144 && i <= 158) || i == 164;
        if (!keep) WriteActiveRaw(i, 0);
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    g_curPass.store(2, std::memory_order_release);
    g_origQueueTasks(self, a, b);
    g_curPass.store(1, std::memory_order_release);
    QueryPerformanceCounter(&t1);
    for (int i = 0; i < n; ++i) WriteActiveRaw(i, saved[i]);
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    g_pumpLastMs.store((float)((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart),
                       std::memory_order_relaxed);
    InterlockedIncrement(&g_pumpPasses);
    if (g_winTrace.load(std::memory_order_relaxed)) {
        const int i = g_p2N.fetch_add(1, std::memory_order_relaxed);
        if (i < 256) g_p2Win[i] = {t0.QuadPart, t1.QuadPart};
    }
}

uint64_t __fastcall Hook_QueueTasks(void* self, void* a, void* b) {
    const long i = InterlockedIncrement(&g_qtCalls);
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    g_qtLastEnterQpc = t0.QuadPart;
    g_qtArgA = a; g_qtArgB = b;
    if (i <= 3) {
        void* frames[12] = {};
        const USHORT got = RtlCaptureStackBackTrace(1, 12, frames, nullptr);
        auto* base = (uint8_t*)GetModuleHandleW(nullptr);
        char line[400]; int p = 0;
        for (USHORT k = 0; k < got && p < (int)sizeof(line) - 24; ++k) {
            HMODULE m = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)frames[k], &m);
            p += (m == (HMODULE)base)
                     ? snprintf(line + p, sizeof(line) - p, " exe+0x%llx",
                                (unsigned long long)((uint8_t*)frames[k] - base))
                     : snprintf(line + p, sizeof(line) - p, " [%p]", frames[k]);
        }
        LOGI("[m2] queue_tasks call %ld: this=%p a=%p b=%p thread %lu, callers:%s",
             i, self, a, b, GetCurrentThreadId(), line);
    }
    if (!g_qtThread) g_qtThread = GetCurrentThreadId();
    const uint64_t r = g_origQueueTasks(self, a, b);
    if (g_winTrace.load(std::memory_order_relaxed)) {
        LARGE_INTEGER tm; QueryPerformanceCounter(&tm);
        const int k = g_p1N.fetch_add(1, std::memory_order_relaxed);
        if (k < 256) g_p1Win[k] = {t0.QuadPart, tm.QuadPart};
    }
    if (g_pumpMode != 0) DoSecondPass(self, a, b);
    LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
    g_qtLastLeaveQpc = t1.QuadPart;
    // Periodic timing/synchronicity log: experiment-only. In production (full stereo) the
    // DrawEnd comparison is meaningless (no probe tasks) and the line is just noise.
    if (Cfg().taskQueueProbe && (i <= 3 || (i % 600) == 0)) {
        static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
        const double inMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        // If DrawEnd's task fired INSIDE this call, the frame executes synchronously under
        // queue_tasks and a second call from the same spot would be a full second pass.
        const __int64 de = g_drawEndQpc;
        LOGI("[m2] queue_tasks call %ld took %.2f ms; DrawEnd task fired %s it returned",
             i, inMs, (de > t0.QuadPart && de < t1.QuadPart) ? "INSIDE - the frame is synchronous under queue_tasks"
                                                             : "OUTSIDE");
    }
    return r;
}

// ---- experiment: dump every group's registered task list with RTTI names ---------------
// The map at queue_tasks arg3+0x18 is an MSVC std::map<cstgi, task_entry_group*> (node walk
// visible in queue_tasks' own code: key at node+0x20, value at +0x28, nil byte at +0x19).
// task_entry_group (fromsoftware-rs, asserts match the 0x269ef80 disassembly): entries vector
// at +0x08 {alloc,begin,end,cap} of task_entry { vptr, FD4TaskBase* task, void*, cstgi }.
// Dumping the class name of every registered task per group is the map of the entire engine
// frame - it names the group that hosts the real render tasks and the sim tasks a draw-only
// second pass must gate.
struct MapNodeView {
    void* left; void* parent; void* right;
    uint8_t color, isNil; uint8_t _pad[6];
    uint32_t key; uint32_t _pad2;
    void* value;
};

// SEH-isolated: walks live engine structures that can churn under us. POD in/out only.
int DumpOneGroup(void* mapHolder, uint32_t id, void** outTasks, int maxTasks) {
    __try {
        auto* head = *(MapNodeView**)((uint8_t*)mapHolder + 0x18);
        if (!head) return -2;
        MapNodeView* node = (MapNodeView*)head->parent;   // root
        MapNodeView* best = nullptr;
        while (node && !node->isNil) {
            if (node->key >= id) { best = node; node = (MapNodeView*)node->left; }
            else node = (MapNodeView*)node->right;
        }
        if (!best || best->key != id) return -3;
        uint8_t* grp = (uint8_t*)best->value;
        if (!grp) return -4;
        uint8_t* begin = *(uint8_t**)(grp + 0x10);
        uint8_t* end   = *(uint8_t**)(grp + 0x18);
        if (!begin || end < begin || (size_t)(end - begin) > 0x4000) return -5;
        int n = 0;
        for (uint8_t* e = begin; e + 0x20 <= end && n < maxTasks; e += 0x20)
            outTasks[n++] = *(void**)(e + 8);   // task_entry::task
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

void DumpTaskLists() {
    if (!Cfg().taskDumpLists) return;
    // Wait for the hook to capture the frame-driver args.
    for (int w = 0; w < 100 && !g_qtArgB; ++w) Sleep(100);
    // And for the world: at the main menu the gameplay systems have not registered their
    // tasks yet, and a menu-time dump under-reports exactly the groups we care about.
    for (int w = 0; w < 120 && !GameERPlayerActive(); ++w) Sleep(1000);
    LOGI("[m2] TaskDumpLists: player %s", GameERPlayerActive() ? "active - dumping the in-game task lists"
                                                               : "NEVER became active - dumping menu-time lists");
    void* holder = g_qtArgB;
    if (!holder) { LOGW("[m2] TaskDumpLists: queue_tasks never captured its args"); return; }
    BuildRegionMap();   // the snapshot from probe time is stale by now
    LOGI("[m2] ---- task lists per group (map holder %p) ----", holder);
    int totalTasks = 0, groupsWithTasks = 0;
    for (int i = 0; i < g_table.count; ++i) {
        uint8_t* e = g_table.entry0 + (size_t)i * g_table.stride;
        const uint32_t id = *(const uint32_t*)e;
        char nameA[80]{};
        WideCharToMultiByte(CP_UTF8, 0, (const wchar_t*)(e + kNameOff), -1, nameA, sizeof(nameA) - 1, nullptr, nullptr);
        void* tasks[64] = {};
        const int n = DumpOneGroup(holder, id, tasks, 64);
        if (n <= 0) { if (n < -1) LOGI("[m2] group %3d %-40s <lookup %d>", i, nameA, n); continue; }
        ++groupsWithTasks; totalTasks += n;
        std::string line;
        for (int k = 0; k < n; ++k) {
            void* task = tasks[k];
            std::string cls;
            if (task && Readable(task, 8)) {
                void** vt = *(void***)task;
                if (LooksLikeVTable(vt, 1)) cls = RttiNameOfVTable(vt);
            }
            if (cls.empty()) cls = "?";
            line += cls + " ";
            if (line.size() > 140) { LOGI("[m2] group %3d %-28s [%d] %s", i, nameA, n, line.c_str()); line.clear(); }
        }
        LOGI("[m2] group %3d %-28s [%d] %s", i, nameA, n, line.c_str());
    }
    LOGI("[m2] ---- %d groups hold %d registered tasks ----", groupsWithTasks, totalTasks);
}

float PresentsPerSecond(float secs);   // defined with the toggle experiment below

void DoublePumpTest() {
    if (!Cfg().taskDoublePumpTest) return;
    if (!g_origQueueTasks) { LOGW("[m2] PumpTest needs TaskQueueProbe=true (no hook)"); return; }
    Sleep((DWORD)(Cfg().taskToggleDelay * 1000.f));   // same "wait until in-game" budget
    ErvrFrameTask* fb = g_probeTask[0];
    ErvrFrameTask* sd = g_probeTask[1];
    LOGI("[m2] PUMP TEST: 3 cycles of 4 s baseline / 4 s double-pumped. Canaries: FrameBegin "
         "must HOLD ~240/4s (sim gated in pass 2), GameSceneDraw must DOUBLE (~480/4s).");
    for (int cyc = 1; cyc <= 3; ++cyc) {
        long fb0 = fb ? fb->calls : 0, sd0 = sd ? sd->calls : 0;
        const float base = PresentsPerSecond(4.f);
        long fb1 = fb ? fb->calls : 0, sd1 = sd ? sd->calls : 0;
        g_pumpMode = 1;
        LOGI("[m2] PUMP cycle %d ON", cyc);
        long fb2 = fb ? fb->calls : 0, sd2 = sd ? sd->calls : 0;
        const float pump = PresentsPerSecond(4.f);
        long fb3 = fb ? fb->calls : 0, sd3 = sd ? sd->calls : 0;
        g_pumpMode = 0;
        LOGI("[m2] PUMP cycle %d: baseline %.1f pps (FrameBegin %ld, GameSceneDraw %ld / 4s) -> "
             "pumped %.1f pps (FrameBegin %ld, GameSceneDraw %ld), pass2 last %.2f ms, total %ld passes",
             cyc, base, fb1 - fb0, sd1 - sd0, pump, fb3 - fb2, sd3 - sd2,
             g_pumpLastMs.load(), (long)g_pumpPasses);
        Sleep(1000);
    }
    // ---- present-pairing probe ----------------------------------------------------------
    // With the pump on, trace every Present for ~1.2 s and classify each against the pass-1
    // and pass-2 execution windows. If presents land INSIDE the pass windows (and on the
    // main-loop thread), L/R pairing is deterministic: whatever presents during pass 2 IS
    // the second (right-eye) image. If they land outside / on another thread, the renderer
    // is decoupled and pairing needs the FIFO with slack instead.
    g_p1N.store(0); g_p2N.store(0);
    g_pumpMode = 1;
    g_winTrace.store(true);
    ReShadeAddonPresentTraceStart();
    Sleep(1200);
    PresentSample smp[512];
    const int ns = ReShadeAddonPresentTraceStop(smp, 512);
    g_winTrace.store(false);
    g_pumpMode = 0;
    int n1 = g_p1N.load(); if (n1 > 256) n1 = 256;
    int n2 = g_p2N.load(); if (n2 > 256) n2 = 256;
    int inP1 = 0, inP2 = 0, outside = 0, onMainThread = 0;
    DWORD otherTid = 0;
    for (int s = 0; s < ns; ++s) {
        if (smp[s].tid == g_qtThread) ++onMainThread; else otherTid = smp[s].tid;
        bool hit = false;
        for (int k = 0; k < n1 && !hit; ++k)
            if (smp[s].qpc >= g_p1Win[k].t0 && smp[s].qpc <= g_p1Win[k].t1) { ++inP1; hit = true; }
        for (int k = 0; k < n2 && !hit; ++k)
            if (smp[s].qpc >= g_p2Win[k].t0 && smp[s].qpc <= g_p2Win[k].t1) { ++inP2; hit = true; }
        if (!hit) ++outside;
    }
    LOGI("[m2] PAIRING: %d presents over 1.2 s pumped (%d pass-1 windows, %d pass-2): "
         "%d inside pass-1, %d inside pass-2, %d outside; %d/%d on the main-loop thread "
         "(%lu), other thread %lu -> %s",
         ns, n1, n2, inP1, inP2, outside, onMainThread, ns, g_qtThread, otherTid,
         (outside <= ns / 10)
             ? "Present is SYNCHRONOUS with the pass - tag the eye by pass window"
             : "Present is DECOUPLED from the frame - pair via FIFO with drop tolerance");

    LOGI("[m2] PUMP TEST done (game still alive = pass 2 is survivable)");
}

void QueueTasksProbeInstall(bool force = false) {
    if ((!Cfg().taskQueueProbe && !force) || !g_table.vtable || g_origQueueTasks) return;
    void* target = g_table.vtable[5];
    if (MH_CreateHook(target, (void*)&Hook_QueueTasks, (void**)&g_origQueueTasks) == MH_OK &&
        MH_EnableHook(target) == MH_OK)
        LOGI("[m2] queue_tasks hooked at %p", target);
    else
        LOGW("[m2] queue_tasks hook FAILED");
}

void RegisterExperiment() {
    if (!Cfg().taskRegisterTest) return;
    if (g_table.count != 169) { LOGW("[m2] TaskRegisterTest: table mismatch, not registering"); return; }
    g_probeTask[0] = RegisterFrameTask("FrameBegin", FrameProbeIndexOf("FrameBegin"));
    g_probeTask[1] = RegisterFrameTask("GameSceneDraw", FrameProbeIndexOf("GameSceneDraw"));
    g_probeTask[2] = RegisterFrameTask("DrawEnd", FrameProbeIndexOf("DrawEnd"));
    // Cadence report: are all three called once per frame, at the same rate, with what dt?
    for (int i = 0; i < 3; ++i) {
        Sleep(5000);
        std::string line = "[m2] task cadence @+" + std::to_string((i + 1) * 5) + "s:";
        char buf[160];
        for (ErvrFrameTask* t : g_probeTask) {
            if (!t) continue;
            snprintf(buf, sizeof(buf), " %s=%ld (dt %.5f, seed %d)",
                     t->name, t->calls, t->lastDt, t->lastSeed);
            line += buf;
        }
        LOGI("%s", line.c_str());
    }
}

// ---- experiment: gate a draw task group -------------------------------------------------
// The decisive question for mechanism B' is whether the draw phase is separable from the rest
// of the frame at all. Clearing `active` on one draw group and watching what stops being drawn
// answers it directly, costs one byte, and restores itself.
// Present rate over the next `secs`, which is the quantitative signal: if gating a group
// really removes GPU work, the frame rate moves. A single before/after screenshot cannot tell
// that apart from the sky changing - ELDEN RING's time of day moved from night to overcast
// inside 20 s during the first attempt at this, which is exactly the kind of thing that makes
// an eyeballed A/B lie.
float PresentsPerSecond(float secs) {
    ReShadeAddonGetStats();                       // reset the counter
    Sleep((DWORD)(secs * 1000.f));
    return (float)ReShadeAddonGetStats().presents / secs;
}

void ToggleExperiment() {
    const Config& c = Cfg();
    if (c.taskToggle.empty()) return;
    const int idx = FrameProbeIndexOf(c.taskToggle.c_str());
    if (idx < 0) { LOGW("[m2] TaskToggle: no group named \"%s\"", c.taskToggle.c_str()); return; }

    bool was = true;
    FrameProbeGetActive(idx, was);
    Sleep((DWORD)(c.taskToggleDelay * 1000.f));
    const float phase = c.taskToggleSeconds > 0.5f ? c.taskToggleSeconds : 3.f;
    const int cycles = c.taskToggleCycles > 0 ? c.taskToggleCycles : 4;
    // "byte" writes cstg_control.active directly; "api" calls CS::CSTask::set_task_group_state.
    // The byte alone provably does nothing - the engine reads it when it BUILDS the queue,
    // not when it runs it - so the API is the one that matters.
    const bool useApi = c.taskToggleMode != "byte";
    auto setState = [&](int i, bool on) {
        return useApi ? FrameProbeSetActiveApi(i, on) : FrameProbeSetActive(i, on);
    };
    LOGI("[m2] TOGGLE: A/B on \"%s\" (index %d, was active=%d) via %s: %d cycles of %.1f s off / %.1f s on",
         c.taskToggle.c_str(), idx, (int)was, useApi ? "set_task_group_state()" : "raw byte",
         cycles, phase, phase);

    // A task of OURS registered in the toggled group is the ground truth for whether the
    // gate gates anything: the screen cannot tell "scene not drawn" from "same image again",
    // but our call counter can tell "the group's tasks stopped running".
    ErvrFrameTask* watch = Cfg().taskRegisterTest ? RegisterFrameTask(_strdup((c.taskToggle + ".watch").c_str()), idx) : nullptr;
    ErvrFrameTask* ref = g_probeTask[0];   // FrameBegin, never gated - the "frames happened" baseline
    if (watch) Sleep(1000);                // let the registration take effect before measuring

    float offSum = 0, onSum = 0;
    long  watchOffSum = 0, watchOnSum = 0, refOffSum = 0, refOnSum = 0;
    for (int i = 0; i < cycles; ++i) {
        if (!setState(idx, false)) { LOGW("[m2] TOGGLE: disable failed"); break; }
        // Phase transitions are logged at the instant they happen so an external screenshot
        // series can be correlated against them. The fps number alone is not enough: ELDEN
        // RING is frame-capped, so removing GPU work often shows up as no change at all.
        bool rb = true; FrameProbeGetActive(idx, rb);
        LOGI("[m2] TOGGLE phase %d OFF (byte reads %d)", i + 1, (int)rb);
        long w0 = watch ? watch->calls : 0, r0 = ref ? ref->calls : 0;
        const float off = PresentsPerSecond(phase);
        const long wOff = watch ? watch->calls - w0 : 0, rOff = ref ? ref->calls - r0 : 0;
        setState(idx, true);
        LOGI("[m2] TOGGLE phase %d ON", i + 1);
        w0 = watch ? watch->calls : 0; r0 = ref ? ref->calls : 0;
        const float on = PresentsPerSecond(phase);
        const long wOn = watch ? watch->calls - w0 : 0, rOn = ref ? ref->calls - r0 : 0;
        offSum += off; onSum += on;
        watchOffSum += wOff; watchOnSum += wOn; refOffSum += rOff; refOnSum += rOn;
        LOGI("[m2] TOGGLE cycle %d: OFF %.1f fps (tasks: FrameBegin %ld, %s %ld), "
             "ON %.1f fps (FrameBegin %ld, %s %ld)",
             i + 1, off, rOff, c.taskToggle.c_str(), wOff, on, rOn, c.taskToggle.c_str(), wOn);
    }
    if (watch)
        LOGI("[m2] TOGGLE TASK VERDICT: during OFF our %s task ran %ld times over %ld frames; "
             "during ON %ld over %ld -> %s",
             c.taskToggle.c_str(), watchOffSum, refOffSum, watchOnSum, refOnSum,
             (watchOnSum > 0 && watchOffSum <= cycles)   // boundary races leak ~1 frame per cycle
                 ? "set_task_group_state STOPS the group's tasks (the gate works; whatever "
                   "kept rendering is not driven by this group's task list)"
                 : (watchOnSum > 0 && watchOffSum >= watchOnSum / 2)
                     ? "the group's tasks KEPT RUNNING while gated - set_task_group_state is "
                       "not the runtime gate either"
                     : "inconclusive (low counts)");
    FrameProbeSetActive(idx, was);
    if (cycles > 0)
        LOGI("[m2] TOGGLE RESULT \"%s\": mean OFF %.1f fps vs ON %.1f fps (%+.1f%%) - %s",
             c.taskToggle.c_str(), offSum / cycles, onSum / cycles,
             onSum > 0 ? (offSum / onSum - 1.f) * 100.f : 0.f,
             (offSum / (onSum > 0 ? onSum : 1.f)) > 1.10f
                 ? "GATING WORKS: disabling the group measurably removed work"
                 : "no measurable effect - either the group is cheap or the byte is not the gate");
}

} // namespace

const TaskGroupTable& FrameProbeGetTable() { return g_table; }

int FrameProbeIndexOf(const char* name) {
    if (!g_table.valid) return -1;
    wchar_t w[80]{};
    MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 79);
    for (int i = 0; i < g_table.count; ++i)
        if (NameIs(g_table.entry0 + (size_t)i * g_table.stride, w)) return i;
    return -1;
}

bool FrameProbeGetActive(int index, bool& out) {
    if (!g_table.valid || index < 0 || index >= g_table.count) return false;
    out = g_table.entry0[(size_t)index * g_table.stride + kActiveOff] != 0;
    return true;
}

bool FrameProbeSetActive(int index, bool active) {
    if (!g_table.valid || index < 0 || index >= g_table.count) return false;
    uint8_t v = active ? 1 : 0;
    return SafeWrite(g_table.entry0 + (size_t)index * g_table.stride + kActiveOff, &v, 1);
}

namespace {
using PfnSetTaskGroupState = bool(__fastcall*)(void* self, int32_t id, bool state);

// SEH-wrapped: this calls into the engine from a thread it knows nothing about. POD locals only.
int CallSetState(void* self, void* fn, int32_t id, bool state) {
    __try { return ((PfnSetTaskGroupState)fn)(self, id, state) ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
} // namespace

bool FrameProbeSetActiveApi(int index, bool active) {
    if (!g_table.valid || !g_table.owner || !g_table.vtable) return false;
    if (index < 0 || index >= g_table.count) return false;
    // The id is NOT the index: it is 0x90000000 | index, read straight from the entry.
    const int32_t id = *(const int32_t*)(g_table.entry0 + (size_t)index * g_table.stride);
    // Slot 3: [0]=~dtor, [1]=add_task_group, [2]=unk_set_task_group, [3]=set_task_group_state.
    const int r = CallSetState(g_table.owner, g_table.vtable[3], id, active);
    if (r < 0) { LOGE("[m2] set_task_group_state(0x%08X, %d) FAULTED", (unsigned)id, (int)active); return false; }
    return true;
}

void FrameProbeTick() {
    const bool fullStereo = Cfg().vrStereoMode == "full";
    if (!Cfg().m2TaskProbe && !fullStereo) return;
    if (g_started.exchange(true)) return;
    // Its OWN thread, deliberately. This can scan a lot; the worker thread it used to run on
    // also drives BackendsTick(), so blocking there meant xrCreateInstance never ran and the
    // headset never came up. Nothing the probe does needs to be in step with anything else.
    std::thread([fullStereo] {
        // The table is built during engine init, so early attempts can legitimately miss.
        for (int attempt = 1; attempt <= 6; ++attempt) {
            if (RunOnce(attempt)) {
                if (fullStereo) {
                    // Production: install the frame-driver hook and arm the stereo pump.
                    // Pass 2 only runs on frames where the game stashed a second eye, so
                    // this is inert until an OpenXR session is live and requesting stereo.
                    QueueTasksProbeInstall(true);
                    if (g_origQueueTasks) {
                        g_pumpMode = 2;
                        LOGI("[stereo] frame pump ARMED: draw-only second pass per stereo frame");
                    } else {
                        LOGW("[stereo] frame pump unavailable (hook failed); full stereo cannot run");
                    }
                }
                if (Cfg().m2TaskProbe) {
                    RegisterExperiment();
                    QueueTasksProbeInstall();
                    DumpTaskLists();
                    DoublePumpTest();
                    ToggleExperiment();
                }
                return;
            }
            Sleep(10000);
        }
        LOGW("[m2] task-group table never appeared; giving up");
    }).detach();
}

bool FramePumpReady() { return g_origQueueTasks != nullptr && g_pumpMode == 2; }
int  FramePumpCurrentPass() { return g_curPass.load(std::memory_order_acquire); }
long FramePumpTakePasses() { return InterlockedExchange(&g_pumpPasses, 0); }
float FramePumpLastMs() { return g_pumpLastMs.load(std::memory_order_relaxed); }
}
