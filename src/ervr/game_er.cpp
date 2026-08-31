// ELDEN RING integration (game 1.16.2 / eldenring.exe 2.6.2.0).
//
// Layouts and hook points come from the open-source community work on this exact
// version: erfps2 (Dasaav, camera step hook + head hiding + aim matrix),
// ERSkeletonMan (Dasaav, live Havok pose access via hkbCharacter), fromsoftware-rs
// (struct layouts), FreecamMod / TGA (FieldArea -> GameRend -> CSPersCam), and the
// classic WorldChrMan AOB. Nothing here is guessed from scratch; see README.
#include "game_er.h"
#include "game_memory.h"
#include "config.h"
#include "debug_keys.h"
#include "log.h"
#include "vr_backend.h"
#include "midhook.h"
#include "rtti.h"
#include "combat.h"
#include "frame_probe.h"
#include "stereo.h"
#include <windows.h>
#include <MinHook.h>
#include "virtual_pad.h"
#pragma comment(lib, "version.lib")
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include <cmath>
#include <cstring>

namespace ervr {
namespace {

// ---------------------------------------------------------------- offsets (1.16.2)
namespace off {
    constexpr size_t WCM_MainPlayer   = 0x1E508;   // WorldChrMan -> PlayerIns*
    constexpr size_t WCM_ChrCam       = 0x1ECE0;   // WorldChrMan -> ChrCam*
    constexpr size_t Chr_ChrCtrl      = 0x58;
    constexpr size_t Chr_IsLockedOn   = 0xC9;
    constexpr size_t Chr_LockOnTarget = 0xD0;      // float4 world position the character faces when locked
    constexpr size_t Ctrl_IsUnlocked  = 0x199;     // ChrCtrl: 1 = free movement (body turns to move dir), 0 = strafe
    constexpr size_t PI_LockedEnemy   = 0x6B0;     // PlayerIns: FieldInsHandle of the locked-on enemy (-1 = none)
    constexpr size_t Chr_AimViewMtx   = 0x120;     // 4x4 float, camera-style rows
    constexpr size_t Chr_Modules      = 0x190;
    constexpr size_t Chr_BaseTransp   = 0x24C;
    constexpr size_t Mod_Behavior     = 0x28;
    constexpr size_t Mod_Physics      = 0x68;
    constexpr size_t Phys_Orientation = 0x50;      // quat xyzw
    constexpr size_t Phys_InterpOrient = 0x60;     // quat the game interpolates toward (write both to pin facing)
    constexpr size_t Phys_Position    = 0x70;      // xyzw
    constexpr size_t Beh_Unk10        = 0x10;      // -> +0x30 hkbCharacter (TGA / ERSkeletonMan)
    constexpr size_t Beh_HkbContext   = 0x60;      // -> +0x30 hkbCharacter (fromsoftware-rs erfps2 branch)
    constexpr size_t Ctx_HkbCharacter = 0x30;
    constexpr size_t Hkb_Setup        = 0x90;      // -> +0x28 hkaSkeleton
    constexpr size_t Setup_Skeleton   = 0x28;
    constexpr size_t Hkb_PoseHolder   = 0x38;      // -> ptr D ; pose = [D] + *(int*)([D]+0x54)
    constexpr size_t Hka_ParentIdx    = 0x20;
    constexpr size_t Hka_BoneCount    = 0x28;
    constexpr size_t Hka_NameLayout   = 0x30;      // char** ; name = [i*2]
    constexpr size_t Hka_RefPose      = 0x40;
    constexpr size_t Ctrl_ModelMatrix = 0x230;     // ChrCtrl model matrix (row 3 = position)
    constexpr size_t PI_AsmModelRes   = 0x640;
    constexpr size_t PI_AsmModelIns   = 0x648;
    constexpr size_t Asm_Parts        = 0x28;      // CSModelIns*[27]
    constexpr size_t MI_DispEntity    = 0x18;
    constexpr size_t Disp_Flags1      = 0x20;
    constexpr size_t Cam_Matrix       = 0x10;      // CSCam: 4 rows right/up/forward/pos
    constexpr size_t Cam_Fov          = 0x50;
    constexpr size_t FA_GameRend      = 0x20;
    // CSCamera (fromsoftware-rs): +0x08 = pers_cam_1, the FINAL render camera; +0x10/+0x18/+0x20 = sources the
    // camera step copies into it per camera_mask (+0x28). Writing only +0x20 (the old GR_PersCam1) meant the true
    // render camera only ever got our matrix through the game's own copy/interpolation - the camera lag.
    constexpr size_t GR_PersCamSlots[4] = {0x08, 0x10, 0x18, 0x20};
}
// RVAs for eldenring.exe 2.6.2.0 (erfps2 src/rva/ww.rs), with AOB fallbacks below.
constexpr uint32_t RVA_CameraStepUpdate = 0x3bc5e0;
constexpr uint32_t RVA_GetDmyPos        = 0x3e94d0;
constexpr uint32_t RVA_MmsUpdateChrCam  = 0xb02390;   // per-frame ChrCam update (erfps2 MMS_UPDATE_CHR_CAM)
constexpr uint32_t RVA_FollowCamFollow  = 0x3b6e20;   // ChrExFollowCam::follow (erfps2 FOLLOW_CAM_FOLLOW)
constexpr uint32_t RVA_UpdateFollowCam  = 0x3b7560;   // outer follow-cam update, fn(ChrExFollowCam*) (erfps2 UPDATE_FOLLOW_CAM)
constexpr uint32_t RVA_UpdateChrModelPos = 0x3ca950;  // per-chr model matrix update, fn(ChrCtrl*) (erfps2 UPDATE_CHR_MODEL_POS) - late in the frame, after the upper-body anim layer
// Havok pose evaluation. Found with the pose scanner: the bulk pose store at +0x24e3 (rva 0x146b793) rewrites the
// live pose AFTER all three of our write points, which is what the weapon attachments then sample - the arms stay
// ours (the mesh consumer at 0x1652f80 reads earlier) while the weapons keep swinging with the animation.
// Function bounds resolved from the exe's .pdata exception table (exact, unlike scanning back for a prologue:
// the earlier 0x14692b0 guess turned out to be an unrelated 45-byte function, so that hook did nothing).
constexpr uint32_t RVA_PoseEval = 0x146b5e0;    // 0x146b5e0..0x146b845, contains the bulk pose store at 0x146b793
// The game rebuilds the camera matrix here (store at 0x68129c) AFTER our model-pos write - camscan phase
// tags put it past every ERVR write point - so its follow-cam-derived, interpolated position is what ends
// up rendered: the view lags the body while moving sideways and converges once you stop.
constexpr uint32_t RVA_CamFinalize = 0x680d70;  // 0x680d70..0x68180c
constexpr uint32_t RVA_CamWallRecovery  = 0x3b15c80;  // global bool: camera wall-recovery pushes (erfps2 clears it per frame in first person)
// Near-camera dither lookup (erfps2 USES_DITHERING): bool fn(mgr, out, id) walks mgr's entry table [+0x1060,+0x1068)
// (24-byte entries {int id; float a0, a1; float nearStart, nearEnd; int x}) and copies the id's entry to *out. Leaf,
// register-only - erfps2 detours it with exactly this signature. The AOB below is the function's own first 36 bytes.
constexpr uint32_t RVA_UsesDithering    = 0x1a679c0;
constexpr size_t   OFF_FollowCam_ResetCamera = 0x315;
constexpr size_t   OFF_FollowCam_AutoRotation = 0x240; // ChrExFollowCam.camera_auto_rotation (fromsoftware-rs; erfps2 clears in first person)
constexpr size_t   OFF_FollowCam_LockedOffset = 0x2E4; // ChrExFollowCam.locked_on_cam_offset (lateral lock-on camera offset; erfps2 zeroes it)

// ---------------------------------------------------------------- game types
struct HkQsTransform { float t[4]; float q[4]; float s[4]; };   // hkQsTransform (parent-relative)

using FnCameraStep = void (__fastcall*)(void* cscamera, void* fd4time);
using FnGetDmyPos  = float* (__fastcall*)(void* chrIns, float* outMtx4x4, const uint32_t* dmyId, int one);

// ---------------------------------------------------------------- state
uint8_t* g_base = nullptr;
uint8_t* g_camWallRecovery = nullptr;    // address of the camera wall-recovery bool global (0 while first person)
uint8_t** g_worldChrManPtr = nullptr;    // address of the WorldChrMan* global
uint8_t** g_fieldAreaPtr = nullptr;      // address of the FieldArea* global
uint8_t** g_gameDataManPtr = nullptr;    // address of the GameDataMan* global (stats cheat)
uint8_t** g_gameManPtr = nullptr;        // address of the GameMan* global (warp_requested)
uint8_t** g_eventFlagManPtr = nullptr;   // address of the CSEventFlagMan* global (loading byte)
std::atomic<ULONGLONG> g_warpReqTick{0}; // last tick GameMan.warp_requested read TRUE
std::atomic<ULONGLONG> g_loadingTick{0}; // last tick the loading-screen event byte read TRUE
std::atomic<ULONGLONG> g_bigJumpTick{0}; // last >100 m camera jump (teleport) tick
std::atomic<ULONGLONG> g_lastStashTick{0}; // last frame a second-eye camera was stashed
                                           // (gameplay heartbeat; goes quiet in menus)

// The pass's eye report, held back until pass end. The game's camera step re-smooths the
// final render camera AFTER our write (yaw in ~100% of frames, position >5 cm in ~half,
// measured 2026-08-22 20:13), so a report pushed at write time describes a camera the image
// was NOT rendered with - the compositor answers with head-motion skew and noisy stereo.
// UpdateCamera / GameERApplySecondEyeCamera stash here (last write of the pass wins, its
// written camera alongside); the frame pump calls GameERFinalizePassReport after each
// pass's queue_tasks returns - every writer done, Flip presented, same thread - which reads
// the camera the renderer REALLY used, folds the written->final delta into the pose, and
// only then pushes the report.
struct PendingReport {
    StereoEyeReport rep;
    Quat qWritten; Vec3 pWritten;   // the camera we wrote, game space
    bool valid = false;
};
std::mutex g_pendingRepMtx;
PendingReport g_pendingRep;

void StashPendingReport(const StereoEyeReport& rep, const Quat& qW, const Vec3& pW) {
    std::lock_guard<std::mutex> g(g_pendingRepMtx);
    g_pendingRep.rep = rep;
    g_pendingRep.qWritten = qW;
    g_pendingRep.pWritten = pW;
    g_pendingRep.valid = true;
}
FnCameraStep g_origCameraStep = nullptr;
using FnMmsUpdate = void (__fastcall*)(void*);
using FnFollow = void (__fastcall*)(void* followCam, float dt, void* c);
using FnUpdFollow = void (__fastcall*)(void* followCam);
using FnUpdModelPos = void (__fastcall*)(void* chrCtrl);
using FnPoseEval = uint64_t (__fastcall*)(void*, void*, void*, void*);   // 4 int args + RAX forwarded (still an ABI guess - see fpPoseAfterHavok)
FnMmsUpdate g_origMms = nullptr; FnFollow g_origFollow = nullptr; FnUpdFollow g_origUpdFollow = nullptr;
FnUpdModelPos g_origUpdModelPos = nullptr; FnPoseEval g_origPoseEval = nullptr;
using FnCamFinalize = uint64_t (__fastcall*)(void*, void*, void*, void*);
FnCamFinalize g_origCamFinalize = nullptr;
using FnUsesDithering = bool (__fastcall*)(void* mgr, void* out, uint32_t id);
FnUsesDithering g_origUsesDithering = nullptr;
std::atomic<bool>  g_ditherFp{false};     // first person is running: apply NearDither (refreshed once per camera step)
std::atomic<float> g_ditherScale{0.f};    // Cfg().fpNearDither, cached for the render thread
// The ChrExFollowCam instance, captured from its hooks. Its *own* embedded camera (matrix at +0x10) is the
// source the game's camera step copies/interpolates the render camera from - writing only chr-cam/pers-cam
// left this one holding the game's follow position, and whoever sampled mid-step (R.E.A.L.) rendered a view
// that lagged behind the character and caught up when you stopped. So it gets our matrix at every reassert.
std::atomic<void*> g_followCamPtr{nullptr};
// The last camera matrix ERVR wrote, replayed after the game's own camera finalizer so ours is the one rendered.
float g_lastCamMtx[16];
std::atomic<bool> g_haveCamMtx{false};
std::mutex g_camMtxLock;
FnGetDmyPos  g_getDmyPos = nullptr;
void* g_hkOrig = nullptr; void** g_hkSlot = nullptr;
GameERStatus g_status;
// Foreign-camera-FOV detector. The map screen (the only road to fast travel), cutscenes and
// other full-screen takeovers write their own FOV into the camera we own every frame - the
// 13:18 crash log shows fov 0.838 in the pre-write dumps the whole time the map was open,
// against the 1.938 we keep writing. So "the FOV we wrote last frame is not the FOV we read
// this frame" = some game system has taken the camera = a warp can be requested at any
// moment; the stereo pump uses it to sit out the request window that no other guard covers.
std::atomic<ULONGLONG> g_foreignFovTick{0};
std::atomic<float>     g_lastFovWrite{0.f};
std::mutex g_vrMtx;
VRTracking g_vrTr; VRInputState g_vrIn; bool g_vrValid = false;
// ONE VR sample per render cycle. BuildFrame runs from four write points (pose hook, camera step, model-pos,
// post-havok) spread across the game's worker threads, and each call used to take the newest tracking sample -
// so the body was posed from one head position and the camera placed from another, taken milliseconds later.
// Stick yaw is a single shared number, which is why turning with the stick never separated them; head position
// is sampled per call, so moving your head while the character moves separates body and camera by exactly how
// far your head travelled between the two hooks. The cycle ends at the model-pos hook (our last write point).
double NowSec();
double NowSec();
VRTracking g_cycTr; VRInputState g_cycIn; bool g_cycValid = false;
std::atomic<uint32_t> g_vrCycle{0}, g_vrLatched{0xFFFFFFFFu};
double g_vrLatchTime = 0;

void LatchVRForCycle(VRTracking& tr, VRInputState& in, bool& valid) {
    std::lock_guard<std::mutex> g(g_vrMtx);
    uint32_t cyc = g_vrCycle.load(std::memory_order_relaxed);
    double now = NowSec();
    if (!Cfg().fpLatchVRPerFrame) { tr = g_vrTr; in = g_vrIn; valid = g_vrValid; return; }
    // Re-latch on a new cycle, or if the cycle stopped advancing (model-pos not running: menus, load screens).
    if (g_vrLatched.load(std::memory_order_relaxed) != cyc || now - g_vrLatchTime > 0.1) {
        g_vrLatched.store(cyc, std::memory_order_relaxed);
        g_cycTr = g_vrTr; g_cycIn = g_vrIn; g_cycValid = g_vrValid; g_vrLatchTime = now;
    }
    tr = g_cycTr; in = g_cycIn; valid = g_cycValid;
}
double g_lastDebug = 0;
uint32_t g_frame = 0;
// perf counters (reset by GameERGetStatus)
volatile LONG g_hkCalls = 0, g_hkSolves = 0, g_camCalls = 0;
// Pose-fight probe: counts how often the game overwrote the R_UpperArm local rotation between our writes, split
// by which of our write points ran last (0 = pose hook, 1 = camera-step post pass, 2 = model-pos hook). Locates
// the upper-body animation layer (walk sway, weapon arts) that still shows on the arms. Measured: it evaluates
// every frame AFTER the camera step, which is why the model-pos hook (later still) carries the final write.
Quat g_probeQ; int g_probeLastPoint = -1;
volatile LONG g_poseForeign[4] = {0, 0, 0, 0};
// Which bones the foreign layer touches and how hard (max degrees per 5 s window) - distinguishes a real
// animation layer (tens of degrees on the arm chain) from an aim/twist modifier (a few degrees, twist bones).
std::vector<int> g_probeBones; std::vector<std::string> g_probeNames;
std::vector<Quat> g_probeLastQ; std::vector<uint8_t> g_probeHas; std::vector<float> g_probeMaxDeg;
// Final-render-camera probe: our matrix writes were confirmed clean (foreign yaw 0), yet the view still lagged -
// so also compare the FINAL pers cam (+0x08) against what we last wrote, position included. Nonzero counts here
// mean the game's own step re-smooths the final camera; zero counts mean the smoothing happens beyond memory
// we can see (R.E.A.L.'s own game-camera filter).
Vec3 g_lastWrittenPos{};
volatile LONG g_finalYawForeign = 0, g_finalPosForeign = 0;
// Rig-vs-camera skew probe: distance between the character root the camera write was built on and the model
// matrix the rig actually renders with, same frame. Nonzero while moving = our lag; zero = the lag is R.E.A.L.'s.
Vec3 g_lastCamRoot{}; float g_skewMax = 0.f;
float g_bodyCamLat = 0.f, g_bodyCamFwd = 0.f, g_bodyCamUp = 0.f;   // rendered head vs written eye, view axes
volatile LONGLONG g_hkTicks = 0, g_camTicks = 0;
uint32_t g_lastArmFrame = 0xFFFFFFFF;
struct ScopedTicks { volatile LONGLONG* acc; LARGE_INTEGER t0; ScopedTicks(volatile LONGLONG* a) : acc(a) { QueryPerformanceCounter(&t0); } ~ScopedTicks() { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); InterlockedExchangeAdd64(acc, t1.QuadPart - t0.QuadPart); } };

// ---------------------------------------------------------------- helpers
template <class T> bool RD(const void* addr, T& out) { return SafeRead(addr, &out, sizeof(T)); }
template <class T> bool WR(void* addr, const T& v) { return SafeWrite(addr, &v, sizeof(T)); }
uint8_t* Deref(const void* addr) { uint8_t* p = nullptr; return (SafeRead(addr, &p, sizeof(p))) ? p : nullptr; }

double NowSec() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart;
}

// VR (right-handed, -Z forward) -> game (left-handed, +Z forward): mirror Z.
// The numpad hotkey wins over the ini so the experiment can be driven from inside the headset.
float DebugPass2PitchDeg() {
    const float k = DebugKeysPass2Pitch();
    return k >= 0.f ? k : Cfg().vrDebugPass2Pitch;
}

Vec3 ToGame(const Vec3& v) { return {v.x, v.y, -v.z}; }
Quat ToGame(const Quat& q) { return {-q.x, -q.y, q.z, q.w}; }

// Camera-style rotation from yaw (about +Y, positive = turn right) and pitch (positive = look down).
void CamBasis(float yaw, float pitch, Vec3& right, Vec3& up, Vec3& fwd) {
    right = {std::cos(yaw), 0.f, -std::sin(yaw)};
    fwd   = {std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    up    = fwd.Cross(right);
}
// Cached "the world is dying" check for the per-frame write paths (evaluated at most every
// 30 ms - the full check walks the HP chain plus two singleton reads). On the RISING edge it
// also drops the cached follow-cam pointer: that cache is the one write target with no
// per-use liveness check, and the 22:21:45 death crash (pass 1 teardown dereferencing a
// FLOAT bit pattern inside CS::CSWorldGeomIns bookkeeping, with our matrix/pose writes
// running at full rate through the whole death window) is exactly what a 64-byte float
// blob landing in freed-and-reused memory looks like.
bool WorldDyingNow() {
    static std::atomic<ULONGLONG> s_next{0};
    static std::atomic<bool> s_val{false};
    const ULONGLONG now = GetTickCount64();
    ULONGLONG next = s_next.load(std::memory_order_relaxed);
    if (now >= next && s_next.compare_exchange_strong(next, now + 30)) {
        const bool d = GameERWorldDying();
        const bool was = s_val.load(std::memory_order_relaxed);
        if (d && !was) {
            g_followCamPtr.store(nullptr, std::memory_order_relaxed);
            LOGI("[guard] world dying - game-state writes stand down (camera/pose/facing)");
        } else if (!d && was) {
            LOGI("[guard] world stable - game-state writes resume");
        }
        s_val.store(d, std::memory_order_relaxed);
    }
    return s_val.load(std::memory_order_relaxed);
}

void WriteCamMatrix(uint8_t* cam, const Vec3& r, const Vec3& u, const Vec3& f, const Vec3& p) {
    // Single choke point for every camera-matrix write ERVR makes (pers cams, chr cam,
    // follow cam, aim view, second eye, debug pitch): while the world is dying, the game's
    // own camera (death cam included - erfps2 stands down for it too) owns the view.
    if (WorldDyingNow()) return;
    float m[16] = { r.x, r.y, r.z, 0.f, u.x, u.y, u.z, 0.f, f.x, f.y, f.z, 0.f, p.x, p.y, p.z, 1.f };
    SafeWrite(cam, m, sizeof(m));
}
Vec3 QuatRotate(const Quat& q, const Vec3& v) { return q.Rotate(v); }

// ---------------------------------------------------------------- game object access
uint8_t* WorldChrMan() { return g_worldChrManPtr ? Deref(g_worldChrManPtr) : nullptr; }
uint8_t* MainPlayer() { uint8_t* w = WorldChrMan(); return w ? Deref(w + off::WCM_MainPlayer) : nullptr; }
uint8_t* ChrCam() { uint8_t* w = WorldChrMan(); return w ? Deref(w + off::WCM_ChrCam) : nullptr; }
uint8_t* GameRend() {
    uint8_t* fa = g_fieldAreaPtr ? Deref(g_fieldAreaPtr) : nullptr; if (!fa) return nullptr;
    return Deref(fa + off::FA_GameRend);
}
// Does this pointer really address a CSCam? A slot that is not a camera must never be written: the pers-cam
// slot layout is version- and build-dependent (the +0x08 "final camera" read back null on 2.6.2.0), and a 64-byte
// matrix written into some other object is silent corruption. Orthonormal basis + a sane fov is proof enough.
bool LooksLikeCam(uint8_t* p) {
    if (!p) return false;
    float m[16]; if (!SafeRead(p + off::Cam_Matrix, m, sizeof(m))) return false;
    for (int r = 0; r < 3; ++r) {
        float L = std::sqrt(m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2]);
        if (L < 0.9f || L > 1.1f) return false;
    }
    float fov; if (!SafeRead(p + off::Cam_Fov, &fov, 4)) return false;
    return fov > 0.05f && fov < 3.2f;
}
// All distinct, validated CSPersCam objects hanging off CSCamera. Returns count; logs the layout once.
int AllPersCams(uint8_t* out[4]) {
    uint8_t* gr = GameRend(); if (!gr) return 0;
    int n = 0; uint32_t mask = 0;
    for (int s = 0; s < 4; ++s) {
        uint8_t* p = Deref(gr + off::GR_PersCamSlots[s]);
        if (!LooksLikeCam(p)) continue;
        mask |= 1u << s;
        bool dup = false; for (int i = 0; i < n; ++i) if (out[i] == p) dup = true;
        if (!dup) out[n++] = p;
    }
    static uint32_t s_logged = 0xFFFFFFFF;
    if (mask != s_logged) {
        s_logged = mask;
        LOGI("[cam] CSCamera slots holding real cameras: %s%s%s%s (%d distinct)",
             (mask & 1) ? "+0x08 " : "", (mask & 2) ? "+0x10 " : "", (mask & 4) ? "+0x18 " : "", (mask & 8) ? "+0x20 " : "", n);
    }
    return n;
}
// The camera the game actually renders from: the last validated slot (the classic +0x20 on 2.6.2.0).
uint8_t* PersCam1() {
    uint8_t* cams[4]; int n = AllPersCams(cams);
    return n ? cams[n - 1] : nullptr;
}
uint8_t* Physics(uint8_t* chr) { uint8_t* m = Deref(chr + off::Chr_Modules); return m ? Deref(m + off::Mod_Physics) : nullptr; }
uint8_t* Behavior(uint8_t* chr) { uint8_t* m = Deref(chr + off::Chr_Modules); return m ? Deref(m + off::Mod_Behavior) : nullptr; }

bool PlayerRoot(uint8_t* chr, Vec3& pos, Quat& q) {
    uint8_t* ph = Physics(chr); if (!ph) return false;
    float p[4], r[4];
    if (!SafeRead(ph + off::Phys_Position, p, 16) || !SafeRead(ph + off::Phys_Orientation, r, 16)) return false;
    pos = {p[0], p[1], p[2]}; q = {r[0], r[1], r[2], r[3]};
    return true;
}
bool ModelMatrixPos(uint8_t* chr, Vec3& pos) {
    uint8_t* ctrl = Deref(chr + off::Chr_ChrCtrl); if (!ctrl) return false;
    float p[4]; if (!SafeRead(ctrl + off::Ctrl_ModelMatrix + 48, p, 16)) return false;
    pos = {p[0], p[1], p[2]}; return true;
}
// Full ChrCtrl model matrix (rows 0..2 = basis, row 3 = position). This is the transform the *renderer*
// hangs the skeleton off, so bone world transforms have to be built from it or the arms sit in a slightly
// different place than everything you can see - most visibly while running, when it and the physics
// position are a frame of motion apart.
bool ModelMatrixRoot(uint8_t* chr, Vec3& pos, Quat& q) {
    uint8_t* ctrl = Deref(chr + off::Chr_ChrCtrl); if (!ctrl) return false;
    float m[16]; if (!SafeRead(ctrl + off::Ctrl_ModelMatrix, m, sizeof(m))) return false;
    Vec3 X{m[0], m[1], m[2]}, Y{m[4], m[5], m[6]}, Z{m[8], m[9], m[10]};
    float lx = X.Length(), ly = Y.Length(), lz = Z.Length();
    if (std::fabs(lx - 1.f) > 0.05f || std::fabs(ly - 1.f) > 0.05f || std::fabs(lz - 1.f) > 0.05f) return false;
    pos = {m[12], m[13], m[14]};
    q = Quat::FromBasis(X * (1.f / lx), Y * (1.f / ly), Z * (1.f / lz));
    return true;
}
// Which local axis of the ChrCtrl model matrix points where the character faces. The physics module is the
// verified reference (local -Z = facing); the model matrix basis is authored per model, and if the two disagree
// then any vector we express in the physics frame comes out rotated when the renderer applies the model matrix -
// which is what a body-follows-head shift stepping the wrong way looks like.
int g_modelFacingSign = 0;   // 0 = not calibrated yet, -1 = local -Z is forward, +1 = local +Z
void CalibrateModelFacing(uint8_t* chr) {
    if (g_modelFacingSign) return;
    Vec3 mp, pp; Quat mq, pq;
    if (!ModelMatrixRoot(chr, mp, mq) || !PlayerRoot(chr, pp, pq)) return;
    Vec3 pf = pq.Rotate({0, 0, -1}), mf = mq.Rotate({0, 0, -1});
    float d = std::fabs(WrapPi(std::atan2(mf.x, mf.z) - std::atan2(pf.x, pf.z)));
    g_modelFacingSign = (d < Deg2Rad(90.f)) ? -1 : 1;
    LOGI("[fp] model matrix vs physics facing: %.0f deg apart -> model forward is local %cZ%s", Rad2Deg(d),
         g_modelFacingSign < 0 ? '-' : '+', g_modelFacingSign > 0 ? " (shifts expressed in the physics frame would be reversed)" : "");
}
// The rotation the RENDERER hangs the skeleton off. World vectors must be converted into this frame before they
// are written into root bone translations, or the rendered shift points somewhere else entirely.
Quat RenderRootQ(uint8_t* chr, const Quat& fallback) {
    Vec3 mp; Quat mq;
    return ModelMatrixRoot(chr, mp, mq) ? mq : fallback;
}

// One root for the camera anchor and the bone solve, so they never drift apart.
//   model       (default) position from the ChrCtrl model matrix, rotation from the physics module (local -Z =
//               facing, verified; the model matrix rotation was never verified against the physics one)
//   modelmatrix both from the model matrix
//   physics     both from the physics module
bool SkeletonRoot(uint8_t* chr, Vec3& pos, Quat& q) {
    const std::string& src = Cfg().fpArmRootSource;
    if (src == "modelmatrix" && ModelMatrixRoot(chr, pos, q)) return true;
    if (!PlayerRoot(chr, pos, q)) return false;
    if (src != "physics") { Vec3 mp; if (ModelMatrixPos(chr, mp)) pos = mp; }
    return true;
}

// ---------------------------------------------------------------- head hiding
void SetPartVisible(uint8_t* asmIns, int idx, bool visible) {
    uint8_t* part = Deref(asmIns + off::Asm_Parts + idx * 8); if (!part) return;
    uint8_t* disp = Deref(part + off::MI_DispEntity); if (!disp) return;
    uint32_t f; if (!RD(disp + off::Disp_Flags1, f)) return;
    uint32_t nf = (f & ~1u) | (visible ? 1u : 0u);
    if (nf != f) WR(disp + off::Disp_Flags1, nf);
}
void ApplyHeadVisibility(uint8_t* player, bool hideHead, bool hideSheathed) {
    // Model-part flag writes stand down while the world is dying (see WorldDyingNow); the
    // flags keep their last state, so nothing changes visually during the death cam.
    if (WorldDyingNow()) return;
    uint8_t* asmIns = Deref(player + off::PI_AsmModelIns);
    if (asmIns) {
        static const int kHead[] = {0, 2, 6, 21, 22, 23, 24, 25};
        for (int i : kHead) SetPartVisible(asmIns, i, !hideHead);
        // 7 = left weapon, 8 = left sheath, 11 = right weapon, 12 = right sheath. Never touch the weapons.
        static const int kSheath[] = {8, 12};
        for (int i : kSheath) SetPartVisible(asmIns, i, !hideSheathed);
        SetPartVisible(asmIns, 7, true); SetPartVisible(asmIns, 11, true);
    }
    if (uint8_t* res = Deref(player + off::PI_AsmModelRes)) {
        uint8_t b;
        if (RD(res + 0x58, b)) WR(res + 0x58, (uint8_t)((b & ~8) | (hideHead ? 8 : 0)));
        if (RD(res + 0x49, b)) WR(res + 0x49, (uint8_t)((b & ~8) | (hideHead ? 8 : 0)));
        if (RD(res + 0x50, b)) WR(res + 0x50, (uint8_t)((b & ~32) | (hideHead ? 32 : 0)));
    }
}

// ---------------------------------------------------------------- skeleton
struct Skeleton {
    uint8_t* chr = nullptr;
    uint8_t* hkb = nullptr;
    int boneCount = 0;
    std::vector<std::string> names;
    std::vector<int16_t> parents;
    HkQsTransform* refPose = nullptr;
    std::vector<HkQsTransform> refPoseCopy;
    int L_ua = -1, L_fa = -1, L_hand = -1, R_ua = -1, R_fa = -1, R_hand = -1;
    Vec3 handFwd[2], handThumb[2]; bool handAxesOk[2]{};   // hand-bone local frame, from ref pose finger roots
    int torso = -1, L_clav = -1, R_clav = -1, head = -1;
    bool warned = false;

    HkQsTransform* LivePose() const {
        uint8_t* d = Deref(hkb + off::Hkb_PoseHolder); if (!d) return nullptr;
        uint8_t* p = Deref(d); if (!p) return nullptr;
        int o; if (!RD(p + 0x54, o) || o <= 0 || o > 0x100000) return nullptr;
        return (HkQsTransform*)(p + o);
    }
    int Find(const std::string& n) const { for (int i = 0; i < boneCount; ++i) if (names[i] == n) return i; return -1; }
};
Skeleton g_skel;
// PoseMode=vr: the whole rig is rewritten from this captured pose on every Havok tick, so no game animation
// ever shows on the body - rolls, attacks and runs still *displace* the character (root motion goes through
// the physics module, which we do not touch) but the rig itself stays 1:1 with the VR pose. The template is
// captured at skeleton build (whatever the animation held right then) and re-captured whenever you stand
// still for a second, so stance changes (idle, crouch, sitting) still come through as discrete poses.
std::vector<HkQsTransform> g_poseTemplate;
Vec3 g_statPos{}; double g_statSince = 0; double g_templLastCapture = 0;
bool CaptureTemplate(uint8_t* chr, const char* why);
namespace posescan { void Disarm(const char* why); void MarkPhase(int bit); void Drive(void* pose, size_t bytes); }
// Per-root-bone bookkeeping for the BodyFollowsHead shift, so applying it twice in a frame is a no-op.
std::vector<Vec3> g_rootShiftBase, g_rootShiftWritten;
std::vector<Quat> g_rootRotBase, g_rootRotWritten;
std::vector<uint8_t> g_rootShiftHas;
void ResetRecenter();
extern bool g_anchorSmInit; extern float g_headRestHeight;

uint8_t* FindHkbCharacter(uint8_t* chr) {
    uint8_t* beh = Behavior(chr); if (!beh) return nullptr;
    for (size_t o : { off::Beh_Unk10, off::Beh_HkbContext }) {
        uint8_t* ctx = Deref(beh + o); if (!ctx) continue;
        uint8_t* hkb = Deref(ctx + off::Ctx_HkbCharacter); if (!hkb) continue;
        uint8_t* setup = Deref(hkb + off::Hkb_Setup); if (!setup) continue;
        uint8_t* hka = Deref(setup + off::Setup_Skeleton); if (!hka) continue;
        int count; if (!RD(hka + off::Hka_BoneCount, count) || count <= 0 || count > 1024) continue;
        return hkb;
    }
    return nullptr;
}

bool BuildSkeleton(uint8_t* chr) {
    Skeleton s; s.chr = chr;
    s.hkb = FindHkbCharacter(chr); if (!s.hkb) return false;
    uint8_t* setup = Deref(s.hkb + off::Hkb_Setup); uint8_t* hka = setup ? Deref(setup + off::Setup_Skeleton) : nullptr;
    if (!hka) return false;
    if (!RD(hka + off::Hka_BoneCount, s.boneCount)) return false;
    char** layout = (char**)Deref(hka + off::Hka_NameLayout);
    int16_t* parents = (int16_t*)Deref(hka + off::Hka_ParentIdx);
    s.refPose = (HkQsTransform*)Deref(hka + off::Hka_RefPose);
    if (!layout || !parents || !s.refPose) return false;
    s.names.resize(s.boneCount); s.parents.resize(s.boneCount); s.refPoseCopy.resize(s.boneCount);
    if (!SafeRead(s.refPose, s.refPoseCopy.data(), sizeof(HkQsTransform) * s.boneCount)) return false;
    for (int i = 0; i < s.boneCount; ++i) {
        char* nm = nullptr; RD(layout + i * 2, nm);
        char buf[64]{}; if (nm) SafeRead(nm, buf, 63);
        s.names[i] = buf;
        RD(parents + i, s.parents[i]);
    }
    const Config& c = Cfg();
    s.L_ua = s.Find(c.boneLUpperArm); s.L_fa = s.Find(c.boneLForearm); s.L_hand = s.Find(c.boneLHand);
    s.R_ua = s.Find(c.boneRUpperArm); s.R_fa = s.Find(c.boneRForearm); s.R_hand = s.Find(c.boneRHand);
    s.torso = s.Find(c.boneTorso); s.head = s.Find(c.boneHead);
    s.L_clav = s.Find("L_Clavicle"); s.R_clav = s.Find("R_Clavicle");
    if (s.L_clav < 0) s.L_clav = s.L_ua; if (s.R_clav < 0) s.R_clav = s.R_ua;
    // Anatomical hand axes from the reference pose: finger roots = forward, thumb root = thumb side.
    for (int h = 0; h < 2; ++h) {
        const char* pre = h == HandLeft ? "L_" : "R_";
        int hand = h == HandLeft ? s.L_hand : s.R_hand;
        int f2 = s.Find(std::string(pre) + "Finger2"), f1 = s.Find(std::string(pre) + "Finger1"), f3 = s.Find(std::string(pre) + "Finger3"), f0 = s.Find(std::string(pre) + "Finger0");
        if (hand < 0 || f2 < 0 || f0 < 0) continue;
        auto rootOf = [&](int i) {   // accumulate local offsets up to the hand bone
            Vec3 acc{}; int cur = i; int guard = 0;
            while (cur >= 0 && cur != hand && guard++ < 8) { const HkQsTransform& t = s.refPoseCopy[cur]; Quat lq{t.q[0], t.q[1], t.q[2], t.q[3]}; acc = lq.Rotate(acc) + Vec3{t.t[0], t.t[1], t.t[2]}; cur = s.parents[cur]; }
            return acc; };
        Vec3 F = rootOf(f2); if (f1 >= 0) F = F + rootOf(f1); if (f3 >= 0) F = F + rootOf(f3);
        Vec3 T = rootOf(f0);
        if (F.Length() < 1e-3f || T.Length() < 1e-3f) continue;
        s.handFwd[h] = F.Normalized(); s.handThumb[h] = T; s.handAxesOk[h] = true;
        LOGI("[skel] %shand axes (local): fwd(%.2f,%.2f,%.2f) thumb(%.2f,%.2f,%.2f)", pre, s.handFwd[h].x, s.handFwd[h].y, s.handFwd[h].z, T.x, T.y, T.z);
    }
    if (c.dumpSkeleton) {
        std::string all;
        for (int i = 0; i < s.boneCount; ++i) { all += std::to_string(i) + ":" + s.names[i] + "(p" + std::to_string(s.parents[i]) + ") "; if (all.size() > 1500) { LOGI("[skel] %s", all.c_str()); all.clear(); } }
        if (!all.empty()) LOGI("[skel] %s", all.c_str());
    }
    LOGI("[skel] player skeleton: %d bones, hkbCharacter %p; arms L(%d,%d,%d) R(%d,%d,%d)", s.boneCount, (void*)s.hkb, s.L_ua, s.L_fa, s.L_hand, s.R_ua, s.R_fa, s.R_hand);
    g_skel = std::move(s);
    g_rootShiftBase.clear(); g_rootShiftWritten.clear(); g_rootShiftHas.clear(); g_rootRotBase.clear(); g_rootRotWritten.clear();
    ResetRecenter();   // new character / area load: start centred on wherever you are standing now
    g_anchorSmInit = false; g_headRestHeight = -1.f;
    g_poseTemplate.clear(); g_statPos = {}; g_statSince = 0; g_templLastCapture = 0;
    posescan::Disarm("skeleton rebuild");   // the watched buffer may be gone
    // Pose-fight probe bones: right arm chain + torso, plus twist helpers if the model has them.
    g_probeBones.clear(); g_probeNames.clear();
    auto addProbe = [&](int i) { if (i >= 0) { g_probeBones.push_back(i); g_probeNames.push_back(g_skel.names[i]); } };
    addProbe(g_skel.R_clav); addProbe(g_skel.R_ua); addProbe(g_skel.R_fa); addProbe(g_skel.R_hand); addProbe(g_skel.torso);
    int twists = 0;
    for (int i = 0; i < g_skel.boneCount && twists < 2; ++i)
        if (g_skel.names[i].find("Twist") != std::string::npos) { addProbe(i); ++twists; }
    g_probeLastQ.assign(g_probeBones.size(), {}); g_probeHas.assign(g_probeBones.size(), 0); g_probeMaxDeg.assign(g_probeBones.size(), 0.f);
    g_probeLastPoint = -1;
    CaptureTemplate(chr, "skeleton build");   // pose buffer is untouched by us at this point
    return true;
}

// World transforms for all bones from a parent-relative local pose + character root.
struct WorldPose { std::vector<Vec3> pos; std::vector<Quat> q; };
void WorldFromLocal(const Skeleton& s, const std::vector<HkQsTransform>& local, const Vec3& rootPos, const Quat& rootQ, WorldPose& out) {
    out.pos.assign(s.boneCount, {}); out.q.assign(s.boneCount, {});
    for (int i = 0; i < s.boneCount; ++i) {
        const HkQsTransform& t = local[i];
        Vec3 lt{t.t[0], t.t[1], t.t[2]}; Quat lq{t.q[0], t.q[1], t.q[2], t.q[3]};
        int p = s.parents[i];
        if (p < 0 || p >= i) { out.q[i] = rootQ * lq; out.pos[i] = rootPos + rootQ.Rotate(lt); }
        else { out.q[i] = out.q[p] * lq; out.pos[i] = out.pos[p] + out.q[p].Rotate(lt); }
    }
}
bool ComputeWorld(const Skeleton& s, HkQsTransform* pose, const Vec3& rootPos, const Quat& rootQ, WorldPose& out, std::vector<HkQsTransform>& local) {
    local.resize(s.boneCount);
    if (!SafeRead(pose, local.data(), sizeof(HkQsTransform) * s.boneCount)) return false;
    WorldFromLocal(s, local, rootPos, rootQ, out);
    return true;
}

// World position of the head bone from the live pose, walking only the parent chain (cheap enough to run from the
// camera hooks). Uses the *live* pose - with CameraAnchor=head nothing of ours moves the root bones any more, so
// what we read is the animation's own head placement, which is exactly what the camera should sit on.
bool HeadBoneWorld(uint8_t* player, const Vec3& rootPos, const Quat& rootQ, Vec3& out) {
    if (g_skel.chr != player || !g_skel.hkb) { if (!BuildSkeleton(player)) return false; }
    if (g_skel.head < 0) return false;
    HkQsTransform* pose = g_skel.LivePose(); if (!pose) return false;
    int chain[64]; int n = 0;
    for (int i = g_skel.head; i >= 0 && n < 64; i = (g_skel.parents[i] < i ? g_skel.parents[i] : -1)) chain[n++] = i;
    Vec3 p = rootPos; Quat q = rootQ;
    for (int k = n - 1; k >= 0; --k) {
        HkQsTransform t; if (!SafeRead(&pose[chain[k]], &t, sizeof(t))) return false;
        Vec3 lt{t.t[0], t.t[1], t.t[2]}; Quat lq{t.q[0], t.q[1], t.q[2], t.q[3]};
        p = p + q.Rotate(lt); q = (q * lq).Normalized();
    }
    out = p; return true;
}
// Head bone in the TEMPLATE pose, body yaw applied, WITHOUT the body-follows-head shift. PoseMode=vr rewrites the
// live pose every frame including that shift, so anchoring the camera to the live head bone and then moving the
// body onto the camera is a feedback loop that walks the character away a headset-offset per frame. Measuring on
// the template breaks it: this is where the head would be if the body were not following you.
bool HeadBoneTemplateWorld(const Vec3& rootPos, const Quat& rootQ, float bodyYaw, Vec3& out) {
    if ((int)g_poseTemplate.size() != g_skel.boneCount || g_skel.head < 0) return false;
    Vec3 f = rootQ.Rotate({0.f, 0.f, -1.f});
    float facing = WrapPi(std::atan2(f.x, f.z) + Deg2Rad(Cfg().bodyFacingOffsetDeg));
    Quat rotL = Cfg().bodyRotateToHead ? (rootQ.Conj() * Quat::YawQ(WrapPi(bodyYaw - facing)) * rootQ).Normalized() : Quat{};
    int chain[64]; int n = 0;
    for (int i = g_skel.head; i >= 0 && n < 64; i = (g_skel.parents[i] < i ? g_skel.parents[i] : -1)) chain[n++] = i;
    Vec3 p{}; Quat q{};
    for (int k = n - 1; k >= 0; --k) {
        int idx = chain[k];
        const HkQsTransform& t = g_poseTemplate[idx];
        Vec3 lt{t.t[0], t.t[1], t.t[2]}; Quat lq{t.q[0], t.q[1], t.q[2], t.q[3]};
        int pa = g_skel.parents[idx];
        if (pa < 0 || pa >= idx) { lt = rotL.Rotate(lt); lq = (rotL * lq).Normalized(); }
        p = p + q.Rotate(lt); q = (q * lq).Normalized();
    }
    out = rootPos + rootQ.Rotate(p);
    return true;
}
Vec3 g_anchorSm{}; bool g_anchorSmInit = false; double g_anchorSmLast = 0;
float g_headRestHeight = -1.f;   // head-bone height above the root when first seen standing still (bob reference)

// ---------------------------------------------------------------- pose access scan (diagnostic)
// One-shot memory watch: page-protect the live Havok pose buffer and record the RIP of every read/write for
// ~1 second, then restore and log the unique code addresses (module-relative). Finds the upper-body animation
// writer AND the renderer-side consumer that our timed writes keep missing. Armed by PoseWriterScan=true.
namespace posescan {
    std::atomic<bool> g_armed{false};
    uint8_t* g_pageLo = nullptr; size_t g_pageLen = 0;
    uint8_t* g_watchLo = nullptr; uint8_t* g_watchHi = nullptr;   // only accesses in here are recorded
    const char* g_what = "pose";
    DWORD g_oldProt = PAGE_READWRITE;
    struct Hit { void* rip; int isWrite; volatile LONG count; volatile LONG phaseMask; };
    Hit g_hitsArr[64]; volatile LONG g_hitN = 0;
    // Our three write points stamp this each time they run: bit0 pose-hook, bit1 camera-step, bit2 model-pos.
    // Each recorded access ORs in the phases seen since the last write point, so the log shows whether the
    // consumer/writer runs before or after each of our writes - that is the whole ordering question.
    volatile LONG g_phase = 0;
    volatile LONG g_events = 0;
    double g_armedAt = 0;
    bool g_done = false;          // latch: one scan per false->true flip of the config flag
    thread_local bool t_step = false;

    void SetProt(DWORD p) { DWORD old; VirtualProtect(g_pageLo, g_pageLen, p, &old); }
    void Record(void* rip, int isWrite) {
        long ph = g_phase;
        long n = g_hitN;
        for (long i = 0; i < n && i < 64; ++i)
            if (g_hitsArr[i].rip == rip && g_hitsArr[i].isWrite == isWrite) { InterlockedIncrement(&g_hitsArr[i].count); InterlockedOr(&g_hitsArr[i].phaseMask, ph); return; }
        long i = InterlockedIncrement(&g_hitN) - 1;
        if (i < 64) { g_hitsArr[i].rip = rip; g_hitsArr[i].isWrite = isWrite; g_hitsArr[i].count = 1; g_hitsArr[i].phaseMask = ph; }
    }
    // Mark that one of our write points ran. The first pose-hook mark after a camera/model mark begins a new
    // frame, so reset there - that keeps the mask meaning "which of our writes preceded this access, this frame".
    void MarkPhase(int bit) { if (bit == 0 && (g_phase & 6)) g_phase = 1; else InterlockedOr(&g_phase, 1 << bit); }
    LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_SINGLE_STEP) {
            if (!t_step) return EXCEPTION_CONTINUE_SEARCH;
            t_step = false;
            if (g_armed.load(std::memory_order_relaxed)) SetProt(PAGE_NOACCESS);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (code != EXCEPTION_ACCESS_VIOLATION || !g_armed.load(std::memory_order_relaxed)) return EXCEPTION_CONTINUE_SEARCH;
        uint8_t* addr = (uint8_t*)ep->ExceptionRecord->ExceptionInformation[1];
        if (addr < g_pageLo || addr >= g_pageLo + g_pageLen) return EXCEPTION_CONTINUE_SEARCH;
        // Only accesses to the bytes we actually care about are recorded; the rest of the page still faults
        // (page granularity) but is let through silently - otherwise a busy page fills the cap instantly.
        if (addr >= g_watchLo && addr < g_watchHi) {
            Record((void*)ep->ContextRecord->Rip, (int)ep->ExceptionRecord->ExceptionInformation[0]);
            InterlockedIncrement(&g_events);
        }
        SetProt(g_oldProt);
        ep->ContextRecord->EFlags |= 0x100;   // trap flag: re-protect right after this one access
        t_step = true;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    void Install() { AddVectoredExceptionHandler(1, Veh); }
    // Which module an address belongs to, e.g. "RealVR64.dll+0x1234" - the whole point when hunting who reads
    // the camera: a read from R.E.A.L. tells us exactly when it samples, which no game-side RVA can.
    void DescribeAddr(uint8_t* rip, char* out, size_t n) {
        if (rip >= g_base && rip < g_base + 0x4000000) { snprintf(out, n, "eldenring.exe+0x%llx", (unsigned long long)(rip - g_base)); return; }
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &mod) && mod) {
            char path[MAX_PATH]{}; GetModuleFileNameA(mod, path, MAX_PATH);
            const char* base = strrchr(path, '\\'); base = base ? base + 1 : path;
            snprintf(out, n, "%s+0x%llx", base, (unsigned long long)(rip - (uint8_t*)mod));
        } else snprintf(out, n, "%p (unknown module)", (void*)rip);
    }
    void Disarm(const char* why) {
        if (!g_armed.exchange(false)) return;
        SetProt(g_oldProt);
        LOGI("[%sscan] done (%s): %ld accesses, %ld unique sites:", g_what, why, g_events, (long)g_hitN);
        for (long i = 0; i < g_hitN && i < 64; ++i) {
            long pm = g_hitsArr[i].phaseMask;
            char ph[16]; snprintf(ph, 16, "%s%s%s", (pm & 1) ? "P" : "-", (pm & 2) ? "C" : "-", (pm & 4) ? "M" : "-");
            char who[160]; DescribeAddr((uint8_t*)g_hitsArr[i].rip, who, sizeof(who));
            LOGI("[%sscan]   %s %s hits=%ld phases[%s]", g_what, g_hitsArr[i].isWrite ? "WRITE" : "READ ", who, g_hitsArr[i].count, ph);
        }
    }
    // Called once per frame with the current pose buffer. Fires automatically ~60 s into gameplay (once per
    // launch), and additionally on every false->true flip of the PoseWriterScan ini flag.
    void Drive(void* pose, size_t bytes) {
        double now = NowSec();
        static double s_first = 0; if (s_first == 0) s_first = now;
        static bool s_autoFired = false;
        if (g_armed.load()) {
            if (g_events >= 400 || now - g_armedAt > 1.0) Disarm(g_events >= 400 ? "event cap" : "time cap");
            return;
        }
        bool fire = false;
        (void)s_autoFired;   // auto-scans retired: they answered the ordering questions and each one is a brief VEH fault storm
        if (Cfg().fpPoseScan) { if (!g_done) fire = true; } else g_done = false;
        if (!fire) return;
        // Alternate targets: the pose buffer (who animates the rig) and the final render camera matrix (who
        // reads the camera, and when - the module names in the report identify R.E.A.L.'s own sample point).
        static int s_target = 0;
        void* watch = pose; size_t wbytes = bytes; const char* what = "pose";
        if (s_target == 1) {
            uint8_t* pc = PersCam1(); if (!pc) { s_target = 0; return; }
            watch = pc + off::Cam_Matrix; wbytes = 64; what = "cam";
        }
        s_target ^= 1;
        if (!watch || !wbytes) return;
        g_hitN = 0; g_events = 0; g_armedAt = now; g_done = true; g_what = what;
        g_watchLo = (uint8_t*)watch; g_watchHi = g_watchLo + wbytes;
        uintptr_t lo = (uintptr_t)watch & ~(uintptr_t)0xFFF;
        uintptr_t hi = ((uintptr_t)watch + wbytes + 0xFFF) & ~(uintptr_t)0xFFF;
        g_pageLo = (uint8_t*)lo; g_pageLen = hi - lo;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(g_pageLo, &mbi, sizeof(mbi))) g_oldProt = mbi.Protect ? mbi.Protect : PAGE_READWRITE;
        LOGI("[%sscan] armed on %p..%p (watching %p..%p)", what, (void*)lo, (void*)hi, (void*)g_watchLo, (void*)g_watchHi);
        g_armed.store(true);
        SetProt(PAGE_NOACCESS);
    }
}

// ---------------------------------------------------------------- VR -> game frame
// ---------------------------------------------------------------- inertial arm
// The rendered hand chases the controller through a critically damped spring (with velocity
// feed-forward, so a steadily moving light hand tracks with no lag) whose acceleration is capped
// by the carried weight (CombatHandWeight): linear cap KineticAccel/(1+w/W), angular cap
// InertiaAngAccel/(1+w/W). Empty hands track 1:1. Runs on the game thread, once per frame
// cycle, in the play-space frame the arm solver is fed, so the weapon the game sweeps IS the
// lagged one and combat reads its motion back through GameERWeaponMotion.
struct InertiaState {
    bool init = false;
    Vec3 pos, vel, lastTarget; Quat q, lastTargetQ; Vec3 angVel;
    double t = 0;
    float arc = 0.f, tipSpeed = 0.f, handSpeed = 0.f; double stamp = 0;   // report (under g_inertiaMtx)
};
InertiaState g_inertia[2];
std::mutex g_inertiaMtx;

Vec3 QuatToRotVec(const Quat& qIn) {   // axis * angle of a unit quaternion, shortest way round
    Quat n = qIn; if (n.w < 0) { n.x = -n.x; n.y = -n.y; n.z = -n.z; n.w = -n.w; }
    float s = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (s < 1e-6f) return {};
    float ang = 2.f * std::atan2(s, n.w);
    return Vec3{n.x, n.y, n.z} * (ang / s);
}

void InertiaStep(int h, Vec3& p, Quat& q, double now) {
    const Config& c = Cfg();
    InertiaState& s = g_inertia[h];
    const float weight = CombatHandWeight(h);
    float dt = s.init ? (float)(now - s.t) : 0.f; s.t = now;
    const bool goodDt = dt > 0.f && dt <= 0.1f;
    Vec3 vT = goodDt ? (p - s.lastTarget) * (1.f / dt) : Vec3{};
    Vec3 wT = goodDt ? QuatToRotVec((q * s.lastTargetQ.Conj()).Normalized()) * (1.f / dt) : Vec3{};
    s.lastTarget = p; s.lastTargetQ = q;
    if (!c.kineticInertia || weight <= 0.f || !s.init || !goodDt || (p - s.pos).Length() > 1.5f) {
        // 1:1 (empty hand / disabled / first sample / snap): velocities are the controller's own.
        s.vel = vT; s.angVel = wT; s.pos = p; s.q = q; s.init = true;
    } else {
        const float wf = 1.f + weight / (c.kineticAccelWeight > 0.1f ? c.kineticAccelWeight : 4.f);
        const float aMax = (c.kineticAccel > 1.f ? c.kineticAccel : 60.f) / wf;
        const float alMax = (c.inertiaAngAccel > 1.f ? c.inertiaAngAccel : 300.f) / wf;
        const float w0 = 6.2831853f * (c.inertiaSpringHz > 0.5f ? c.inertiaSpringHz : 8.f);
        const float k = w0 * w0, d = 2.f * w0;
        // Braking is allowed at twice the cap: stopping a swing is easier than making one.
        Vec3 a = (p - s.pos) * k + (vT - s.vel) * d;
        { float m = a.Length(), lim = a.Dot(s.vel) < 0 ? 2.f * aMax : aMax; if (m > lim) a = a * (lim / m); }
        s.vel = s.vel + a * dt; s.pos = s.pos + s.vel * dt;
        Vec3 al = QuatToRotVec((q * s.q.Conj()).Normalized()) * k + (wT - s.angVel) * d;
        { float m = al.Length(), lim = al.Dot(s.angVel) < 0 ? 2.f * alMax : alMax; if (m > lim) al = al * (lim / m); }
        s.angVel = s.angVel + al * dt;
        float wm = s.angVel.Length();
        if (wm > 1e-6f) s.q = (Quat::AxisAngle(s.angVel * (1.f / wm), wm * dt) * s.q).Normalized();
        p = s.pos; q = s.q;
    }
    // Motion report: the weapon tip = hand + forward (game +Z) * reach.
    const float reach = h == HandRight ? c.weaponReachRight : c.weaponReachLeft;
    Vec3 tipV = s.vel + s.angVel.Cross(s.q.Rotate({0, 0, 1}) * reach);
    const float tip = tipV.Length();
    std::lock_guard<std::mutex> g(g_inertiaMtx);
    if (goodDt) { if (tip >= c.swingOffSpeed) s.arc += tip * dt; else s.arc = 0.f; }
    s.tipSpeed = tip; s.handSpeed = s.vel.Length(); s.stamp = now;
}

struct FrameInfo {
    bool valid = false;
    bool lockedOn = false; // a real enemy is locked on
    Vec3 root; Quat rootQ; // character root the whole frame is built on (model matrix, physics fallback)
    Vec3 anchor;         // eye anchor in game world (no HMD offset)
    float baseYaw = 0;   // game camera yaw the HMD is relative to
    float basePitch = 0; // game camera pitch (from chr cam)
    bool  headValid = false; // HMD tracking is live (otherwise eye == anchor and there is no stereo)
    Quat  headQ;         // HMD orientation in game frame (relative to tracking origin)
    Vec3  headP;         // HMD position in game frame (relative to the *tracking* origin)
    Vec3  headRel;       // HMD position relative to our own play-space origin (drift removed)
    Vec3  eye;           // where the eye is rendered: anchor + R(baseYaw)*headRel
    float chrYaw = 0;    // physics facing of the character (what the body actually points at)
    float headYaw = 0;   // HMD yaw relative to the tracking origin (game convention, positive = right)
    float bodyYaw = 0;   // absolute yaw the body is driven toward: facing, torso twist, elbow poles
    Vec3  handP[2]; Quat handQ[2]; bool handValid[2]{};
};

// ---------------------------------------------------------------- play-space origin
// The runtime hands us head/hand positions relative to *its* origin (wherever you were standing when the
// runtime last recentred). Everything downstream - the camera, the arm targets, the skeleton shift - is then
// rotated by the camera yaw, so a metre of "I am standing off to the side of my play space" turns into the
// camera swinging around the character every time you turn with the stick, and into a body that is
// permanently shoved sideways. We therefore keep our own origin that slowly follows your head: fast motion
// (leaning, ducking, a step) still comes through, slow drift is absorbed.
Vec3 g_psOrigin{};
bool g_psInit = false;
double g_psLast = 0;
std::mutex g_psMtx;
std::atomic<uint32_t> g_psFrame{0xFFFFFFFFu};

void ResetRecenter() { std::lock_guard<std::mutex> g(g_psMtx); g_psInit = false; g_psOrigin = {}; }

// Returns the play-space origin to measure the head against.
Vec3 UpdateRecenter(const Vec3& headP, double now) {
    const Config& c = Cfg();
    std::lock_guard<std::mutex> g(g_psMtx);
    if (!c.fpRecenter) { g_psOrigin = {}; g_psInit = false; return g_psOrigin; }
    if (!g_psInit) { g_psOrigin = headP; g_psInit = true; g_psLast = now; return g_psOrigin; }
    Vec3 d = headP - g_psOrigin;
    float horiz = std::sqrt(d.x * d.x + d.z * d.z);
    // A large residual means the runtime's origin is nowhere near where you actually stand - a room-scale setup
    // recentred at the other end of the room, say (measured: 1.2 m). Easing that away over RecenterSeconds is
    // what put the body a clamped half-metre from the camera and then slid it back for seconds afterwards, and
    // it is why recentring in the runtime "fixed" everything. Snap instead: the same thing the runtime's own
    // recenter does, and after it the residual is only ever your own leaning.
    if (c.fpRecenterSnapRadius > 1e-3f && (horiz > c.fpRecenterSnapRadius || std::fabs(d.y) > c.fpRecenterSnapRadius)) {
        g_psOrigin.x = headP.x; g_psOrigin.z = headP.z;
        if (c.fpRecenterHeight) g_psOrigin.y = headP.y;
        g_psLast = now;
        LOGD("[fp] play-space origin snapped to the head (residual was %.2f m horizontal, %.2f m vertical)", horiz, d.y);
        return g_psOrigin;
    }
    // Advance the smoothing exactly once per rendered frame. BuildFrame runs from several hooks and the game
    // calls those from a pool of worker threads (8 seen in one session), so letting every call apply its own
    // exponential step made the origin converge at a rate that depended on how many threads happened to call in.
    uint32_t fr = g_frame;
    if (g_psFrame.exchange(fr) != fr) {
        float dt = Clamp((float)(now - g_psLast), 0.f, 0.25f);
        g_psLast = now;
        float k = c.fpRecenterSeconds > 1e-3f ? 1.f - std::exp(-dt / c.fpRecenterSeconds) : 1.f;
        g_psOrigin.x += d.x * k; g_psOrigin.z += d.z * k;
        if (c.fpRecenterHeight) g_psOrigin.y += d.y * k;
        // Hard limit: never let the residual grow past the allowed radius (walking across the room).
        d = headP - g_psOrigin;
        horiz = std::sqrt(d.x * d.x + d.z * d.z);
        if (c.fpRecenterMaxRadius > 1e-3f && horiz > c.fpRecenterMaxRadius) {
            float pull = 1.f - c.fpRecenterMaxRadius / horiz;
            g_psOrigin.x += d.x * pull; g_psOrigin.z += d.z * pull;
        }
        if (c.fpRecenterHeight && c.fpRecenterMaxRadius > 1e-3f) g_psOrigin.y += d.y - Clamp(d.y, -c.fpRecenterMaxRadius, c.fpRecenterMaxRadius);
    }
    return g_psOrigin;
}

// ---------------------------------------------------------------- view yaw
// CameraYawSource=ervr: the view yaw is ERVR's own state, integrated from the turn stick by virtual_pad.
// Reading it back out of the game's chr-cam (the "game" setting) means inheriting everything that moves that
// camera - the follow cam drifting behind you, the lock-on logic framing a target, R.E.A.L. touching it - and
// every one of those shows up as the view sliding off the character.
std::atomic<float> g_camYaw{0.f};
std::atomic<float> g_chrYaw{0.f};
// Yaw applied by the stick since the last frame, still to be pivoted about the head: rotating the view must
// spin the world around YOUR HEAD, not around the body anchor - otherwise standing anywhere but dead centre
// of the play space makes the eye swing sideways on every turn (needs Recenter=true to persist the shift).
std::atomic<float> g_pendingYawPivot{0.f};
// Body yaw: where the character body is turned toward. BodyYaw=head makes it chase the headset yaw (with a time
// constant / deadzone), so shoulders and torso stay under and behind your head when you turn your real body;
// BodyYaw=camera keeps it on the stick yaw (the old behaviour: only the head moves).
std::atomic<float> g_bodyYaw{0.f}, g_headYaw{0.f};
std::atomic<float> g_bodyStickYaw{0.f};   // BodyYaw=stick: integrated from the right stick, absolute
std::atomic<bool>  g_headYawValid{false};
bool g_bodyYawInit = false; double g_bodyYawLast = 0;

float UpdateBodyYaw(float target, double now, bool instant) {
    const Config& c = Cfg();
    if (!g_bodyYawInit) { g_bodyYaw.store(target); g_bodyYawInit = true; g_bodyYawLast = now; return target; }
    if (instant) { g_bodyYaw.store(target); g_bodyYawLast = now; return target; }   // BodyYaw=camera: no lag behind the view
    float dt = Clamp((float)(now - g_bodyYawLast), 0.f, 0.25f); g_bodyYawLast = now;
    float cur = g_bodyYaw.load();
    float d = WrapPi(target - cur);
    float dz = Deg2Rad(std::fmax(0.f, c.bodyYawDeadzoneDeg));
    if (std::fabs(d) <= dz) return cur;
    d -= (d > 0 ? dz : -dz);                     // only the part outside the deadzone pulls
    float k = c.bodyYawFollowSeconds > 1e-3f ? 1.f - std::exp(-dt / c.bodyYawFollowSeconds) : 1.f;
    cur = WrapPi(cur + d * k);
    g_bodyYaw.store(cur);
    return cur;
}
std::atomic<bool>  g_moveActive{false}, g_moveLocked{false};
bool g_camYawInit = false;

float GameCamYaw() {
    uint8_t* cc = ChrCam();
    float fwd[4] = {0, 0, 1, 0};
    if (cc) SafeRead(cc + off::Cam_Matrix + 32, fwd, 16);
    return std::atan2(fwd[0], fwd[2]);
}

// ERVR now owns rendering, so it is unconditionally the sole owner of where the eye sits.
// (This used to negotiate with R.E.A.L. over who applied the HMD offset.)

bool BuildFrame(uint8_t* player, FrameInfo& f) {
    const Config& c = Cfg();
    Vec3 root; Quat rootQ;
    if (!SkeletonRoot(player, root, rootQ)) return false;
    f.root = root; f.rootQ = rootQ;
    f.anchor = root + Vec3{0.f, c.fpEyeHeight, 0.f};
    // CameraAnchor=head: the camera rides on the animation's head bone (what a first-person mod does), so the neck,
    // shoulders and arms are under the view whatever the animation does - run lean, bob, turn - instead of us
    // dragging the skeleton back under a root-anchored camera every tick and losing that fight on some frames.
    // Horizontal: the head bone (low-passed by CameraAnchorSmoothing). Vertical: EyeHeight above the root plus
    // CameraBob of the head bone's own vertical excursion (0 = no bob at all).
    if (c.fpCameraAnchor == "head") {
        Vec3 hb;
        // PoseMode=vr: measure on the template (see HeadBoneTemplateWorld) so the anchor is independent of the
        // shift we are about to apply; otherwise fall back to the live pose.
        bool haveHb = false;
        if (c.fpPoseMode == "vr" && g_skel.chr == player && g_skel.hkb)
            haveHb = HeadBoneTemplateWorld(root, rootQ, g_bodyYaw.load(), hb);
        if (!haveHb) haveHb = HeadBoneWorld(player, root, rootQ, hb);
        if (haveHb) {
            float hh = hb.y - root.y;
            if (g_headRestHeight < 0.f) g_headRestHeight = hh;
            // Smooth the head bone's offset *from the root* (the animation's part), never the root motion itself:
            // smoothing the world position lagged the camera behind a running character by speed x time constant
            // (15 cm at a jog - measured), which is the whole "disconnected while running" gap.
            Vec3 off{hb.x - root.x, c.fpEyeHeight + c.fpCameraBob * (hh - g_headRestHeight), hb.z - root.z};
            double now = NowSec();
            if (!g_anchorSmInit) { g_anchorSm = off; g_anchorSmInit = true; g_anchorSmLast = now; }
            else {
                static std::atomic<uint32_t> s_anchorFrame{0xFFFFFFFFu};
                uint32_t fr = g_frame;
                if (s_anchorFrame.exchange(fr) != fr) {   // once per frame, not once per worker thread
                    float dt = Clamp((float)(now - g_anchorSmLast), 0.f, 0.1f); g_anchorSmLast = now;
                    float k = c.fpCameraAnchorSmoothing > 1e-3f ? 1.f - std::exp(-dt / c.fpCameraAnchorSmoothing) : 1.f;
                    g_anchorSm = g_anchorSm + (off - g_anchorSm) * k;
                }
            }
            f.anchor = root + g_anchorSm;
        }
    }

    float fwd[4] = {0, 0, 1, 0};
    if (uint8_t* cc = ChrCam()) SafeRead(cc + off::Cam_Matrix + 32, fwd, 16);
    f.basePitch = std::asin(Clamp(-fwd[1], -1.f, 1.f));

    int64_t enemy = -1; RD(player + off::PI_LockedEnemy, enemy);
    f.lockedOn = enemy != -1 && enemy != 0;

    // Which way the character actually faces right now. Read from the physics module: the model matrix carries
    // the animation's root rotation on top and wobbles while moving.
    // (physics forward is local -Z: with the character facing our lock-on point, +Z read back 180 degrees off)
    { Vec3 pp; Quat pq; Quat fq = PlayerRoot(player, pp, pq) ? pq : rootQ; Vec3 chrF = fq.Rotate({0, 0, -1}); f.chrYaw = std::atan2(chrF.x, chrF.z); }

    if (c.fpCameraYawSource == "game") {
        f.baseYaw = std::atan2(fwd[0], fwd[2]);
    } else {
        // First frame: start the view where the character faces. The game's chr-cam is not usable for this - during
        // loading it is an identity matrix (yaw 0), and initialising from it snapped the view to world north and
        // then FaceCamera turned the whole character to match ("body starts rotated").
        if (!g_camYawInit) { g_camYaw.store(f.chrYaw); g_camYawInit = true; }
        // Locked on: the game picked the direction, follow the point it makes the character face.
        if (f.lockedOn) {
            float t[4];
            if (SafeRead(player + off::Chr_LockOnTarget, t, 16)) {
                float dx = t[0] - root.x, dz = t[2] - root.z;
                if (dx * dx + dz * dz > 0.01f) g_camYaw.store(std::atan2(dx, dz));
            }
        }
        f.baseYaw = g_camYaw.load();
    }
    g_chrYaw.store(f.chrYaw);
    g_moveLocked.store(f.lockedOn);
    g_moveActive.store(true);

    VRTracking tr; VRInputState in; bool vr;
    LatchVRForCycle(tr, in, vr);
    f.bodyYaw = f.baseYaw;
    if (vr && tr.head.valid) {
        Quat hq = ToGame(tr.head.rot); Vec3 hp = ToGame(tr.head.pos);
        // Pivot stick turns about the head: baseYaw already contains the new yaw, so shift the play-space origin
        // such that R(baseYaw)*headRel - and with it the eye - is unchanged by the turn itself.
        float dy = g_pendingYawPivot.exchange(0.f);
        if (dy != 0.f) { g_psOrigin = hp - Quat::YawQ(-dy).Rotate(hp - g_psOrigin); g_psInit = true; }
        Vec3 psOrigin = UpdateRecenter(hp, NowSec());
        f.headValid = true;
        f.headQ = hq; f.headP = hp; f.headRel = hp - psOrigin;
        f.eye = f.anchor + Quat::YawQ(f.baseYaw).Rotate(f.headRel);
        { Vec3 hf = hq.Rotate({0, 0, 1}); f.headYaw = std::atan2(hf.x, hf.z); }
        g_headYaw.store(f.headYaw); g_headYawValid.store(true);
        // Body: chase the head yaw (default) so shoulders/torso follow your real body; locked on, the game aims it.
        float want;
        if (!g_bodyYawInit) g_bodyStickYaw.store(f.baseYaw);   // start facing the view, not world yaw 0
        if (f.lockedOn || c.bodyYawSource == "camera") want = f.baseYaw;
        else if (c.bodyYawSource == "head") want = WrapPi(f.baseYaw + f.headYaw);
        else want = g_bodyStickYaw.load();                       // stick: absolute, right stick turns it
        f.bodyYaw = UpdateBodyYaw(want, NowSec(), f.lockedOn || c.bodyYawSource == "camera");
        for (int h = 0; h < 2; ++h) {
            const Pose& hand = tr.hand[h];
            f.handValid[h] = hand.valid && in.controllerConnected[h];
            if (!f.handValid[h]) continue;
            Vec3 p = ToGame(hand.pos); Quat q = ToGame(hand.rot);
            // Weight: the hand the arm solver gets is the inertial one, filtered in drift-free play space.
            { Vec3 pPlay = p - psOrigin; InertiaStep(h, pPlay, q, NowSec()); p = pPlay + psOrigin; }
            // All three origins end up drift-free: "head" is head-relative by construction (the head offset is
            // added back at the target), the other two are measured from our own play-space origin.
            if (c.handOrigin == "head") p = p - hp;
            else if (c.handOrigin == "floor") p = p - Vec3{psOrigin.x, c.fpEyeHeight, psOrigin.z};
            else p = p - psOrigin;
            f.handP[h] = p; f.handQ[h] = q;
        }
    }
    if (!(vr && tr.head.valid)) { f.eye = f.anchor; g_headYawValid.store(false); }
    f.valid = true;
    return true;
}

// ---------------------------------------------------------------- camera
// Keep the body facing the camera direction (strafe/lock-on mode) unless a real enemy is locked on,
// otherwise walking backwards/sideways turns the torso away and the arms cannot reach the hands.
//
// The fake lock-on point is measured from the character root, flat, and far away on purpose. The game's
// lock-on camera keeps nudging its own yaw to frame this point, and that yaw is what we read back as
// baseYaw - so any lateral error between the game's camera position and ours feeds straight back into our
// facing. At the old 10 m the loop had enough gain that simply strafing (the game's follow cam lags behind
// you) rotated you a little further every frame; at fpLockTargetDistance the same lag is a rounding error.
void ApplyFaceCamera(uint8_t* player, const Vec3& root, float bodyYaw) {
    const Config& c = Cfg();
    if (!c.fpFaceCamera || c.fpFaceCameraMode == "off") return;
    // Facing/lock-state choke point: while the world is dying, do not pin a dying body's
    // physics orientation or write lock flags - the ragdoll/death anim owns the character.
    if (WorldDyingNow()) return;
    int64_t enemy = -1; RD(player + off::PI_LockedEnemy, enemy);
    bool realLock = enemy != -1 && enemy != 0;
    if (uint8_t* ctrl = Deref(player + off::Chr_ChrCtrl)) WR(ctrl + off::Ctrl_IsUnlocked, (uint8_t)0);
    if (c.fpFaceCameraMode == "strafe") {
        // Strafe movement WITHOUT the game's lock-on state. Forcing Chr_IsLockedOn=1 (the old "lock" mode)
        // engages the game's lock-on *camera* - a camera writer we do not hook - and that camera orbits the
        // character while strafing and swallows our stick turns. Ctrl_IsUnlocked=0 alone keeps the movement in
        // strafe animations; the rig facing is ours anyway (BodyRotate / PoseMode=vr).
        if (!realLock) {
            WR(player + off::Chr_IsLockedOn, (uint8_t)0);   // clear a flag a previous "lock" mode may have left
            // Pin the physics facing to BodyYaw. The game decides forward-run vs strafe from the angle between
            // the move direction and this facing: without the pin, physically turning your head while walking
            // (Forward=head rotates the stick) reads as a sideways stick and you side-step at strafe speed
            // instead of just walking the new way. Strafe-mode dodges are directional (no turn-then-roll), so
            // pinning does not break roll direction.
            if (c.fpPinBodyFacing) {
                if (uint8_t* ph = Physics(player)) {
                    Quat q = Quat::YawQ(WrapPi(bodyYaw + 3.14159265f));   // physics forward is local -Z
                    float r4[4] = {q.x, q.y, q.z, q.w};
                    SafeWrite(ph + off::Phys_Orientation, r4, 16);
                    SafeWrite(ph + off::Phys_InterpOrient, r4, 16);
                }
            }
        }
        return;
    }
    WR(player + off::Chr_IsLockedOn, (uint8_t)1);
    if (realLock) return;
    float d = std::fmax(1.f, c.fpLockTargetDistance);
    Vec3 flat{std::sin(bodyYaw), 0.f, std::cos(bodyYaw)};
    Vec3 tgt = root + Vec3{0.f, c.fpEyeHeight, 0.f} + flat * d;
    float t4[4] = {tgt.x, tgt.y, tgt.z, 1.f};
    SafeWrite(player + off::Chr_LockOnTarget, t4, 16);
}

// One eye per game frame, latched once so that the several UpdateCamera calls a frame makes
// (camera step, camera finalize, post-model-pos) all agree on which eye they are drawing.
struct EyeLatch { bool active = false; StereoEyeRequest req; };
EyeLatch g_eyeLatch;
std::atomic<uint32_t> g_eyeLatchFrame{0xFFFFFFFFu};

const EyeLatch& LatchEyeForFrame() {
    uint32_t fr = g_frame;
    if (g_eyeLatchFrame.exchange(fr) != fr) {
        StereoEyeRequest r;
        g_eyeLatch.active = StereoTakeEyeRequest(r);
        g_eyeLatch.req = r;
    }
    return g_eyeLatch;
}

// Full stereo: everything the draw-only second pass needs to place the OTHER eye's camera.
// Written by UpdateCamera during the real pass; consumed between passes by the frame pump
// (the camera step is gated in pass 2, so the pump's write is what that pass renders from).
struct SecondEyeStash {
    std::mutex mtx;
    bool  valid = false;
    bool  haveHeadBasis = false;
    Quat  qHead;                 // head orientation, game convention (basis when haveHeadBasis)
    Vec3  r, u, fw;              // fallback basis when !haveHeadBasis
    Vec3  posBase;               // the head-centre eye position before any eye offset
    Vec3  offset;                // other eye, head-frame, game convention
    Quat  rot;                   // other eye's cant
    int   eye = 1;
    uint64_t pair = 0;
    float fovY = 0;              // what pass 1 read back from Cam_Fov (pass 2 renders the same)
    Vec3  headPos; Quat headRot; // XR convention, for the report
    // The finished pass-2 camera basis, computed by GameERApplySecondEyeCamera and re-applied
    // by UpdateCamera on every camera-step call DURING pass 2. Writing the matrices once
    // between the passes proved insufficient (DebugPass2Pitch showed in NEITHER eye): the
    // renderer picks the camera up inside the camera-step machinery, so pass 2 must re-run
    // group 106 (Pass2Keep) and the hook must feed it the other eye - the same write path
    // pass 1 provably renders from.
    bool  camValid = false;
    Vec3  camR, camU, camF, camPos;
};
SecondEyeStash g_secondEye;

void UpdateCamera(uint8_t* player, const FrameInfo& f) {
    const Config& c = Cfg();
    // Draw-only second pass: the camera step re-runs (Pass2Keep includes 106) purely so the
    // engine transfers the camera into its render-side state, and the camera it must carry
    // is the OTHER eye's. No latch, no report, no stash: pass 2 renders, it does not simulate.
    if (FramePumpCurrentPass() == 2) {
        Vec3 r2, u2, f2, p2; float fov2;
        bool have = false;
        {
            std::lock_guard<std::mutex> g(g_secondEye.mtx);
            if (g_secondEye.camValid) {
                r2 = g_secondEye.camR; u2 = g_secondEye.camU;
                f2 = g_secondEye.camF; p2 = g_secondEye.camPos;
                fov2 = g_secondEye.fovY;
                have = true;
            }
        }
        // Flat experiments (pump mode 1, no headset): no second-eye stash exists, but the
        // pitch diagnostic still needs a distinguishable pass-2 camera on the monitor.
        if (!have) {
            const float pd = DebugPass2PitchDeg();
            if (pd == 0.f || !g_haveCamMtx.load(std::memory_order_relaxed)) return;
            float m[16];
            { std::lock_guard<std::mutex> g(g_camMtxLock); memcpy(m, g_lastCamMtx, sizeof(m)); }
            r2 = {m[0], m[1], m[2]}; u2 = {m[4], m[5], m[6]};
            f2 = {m[8], m[9], m[10]}; p2 = {m[12], m[13], m[14]};
            const Quat pq = Quat::AxisAngle(r2, Deg2Rad(pd));
            u2 = pq.Rotate(u2); f2 = pq.Rotate(f2);
            fov2 = 0.f;
        }
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            LOGI("[cam] pass-2 camera path ACTIVE: second-eye basis re-applied at camera-step time");
        }
        uint8_t* pcs2[4]; const int npc2 = AllPersCams(pcs2);
        for (int i = 0; i < npc2; ++i) {
            WriteCamMatrix(pcs2[i] + off::Cam_Matrix, r2, u2, f2, p2);
            if (fov2 > 0.05f) SafeWrite(pcs2[i] + off::Cam_Fov, &fov2, 4);
        }
        if (uint8_t* cc = ChrCam()) {
            WriteCamMatrix(cc + off::Cam_Matrix, r2, u2, f2, p2);
            if (fov2 > 0.05f) SafeWrite(cc + off::Cam_Fov, &fov2, 4);
        }
        if (uint8_t* fc = (uint8_t*)g_followCamPtr.load(std::memory_order_relaxed))
            WriteCamMatrix(fc + off::Cam_Matrix, r2, u2, f2, p2);
        // Keep the reassert hooks (follow cam, CamFinalize) replaying THIS eye for the rest
        // of the pass.
        std::lock_guard<std::mutex> g(g_camMtxLock);
        float m[16] = { r2.x, r2.y, r2.z, 0.f, u2.x, u2.y, u2.z, 0.f,
                        f2.x, f2.y, f2.z, 0.f, p2.x, p2.y, p2.z, 1.f };
        memcpy(g_lastCamMtx, m, sizeof(m));
        g_haveCamMtx.store(true, std::memory_order_relaxed);
        return;
    }
    float yaw = f.baseYaw, pitch = 0.f;
    Vec3 r, u, fw;
    // Which eye this frame renders, if the render thread is driving stereo.
    const EyeLatch& el = LatchEyeForFrame();
    const bool stereo = el.active && f.headValid;
    if (c.fpCameraRotation == "game") pitch = f.basePitch;
    bool haveHeadBasis = false;
    Quat qHead;
    if (c.fpCameraRotation == "hmd") {
        // full HMD orientation composed onto the base yaw
        qHead = Quat::YawQ(f.baseYaw) * f.headQ;
        haveHeadBasis = true;
        Quat q = stereo ? qHead * el.req.rot : qHead;   // per-eye cant; identity on most headsets
        fw = q.Rotate({0, 0, 1}); r = q.Rotate({1, 0, 0}); u = q.Rotate({0, 1, 0});
    } else CamBasis(yaw, pitch, r, u, fw);
    // Capture probe (Numpad 3, third state): tilt the REAL pass's camera. If both eyes show
    // the tilt, the renderer captures its camera once per sim frame from pass 1 - which is
    // why nothing written for pass 2 has ever reached pass 2's image. One tilted eye instead
    // names the eye that receives pass 1's content.
    const float p1p = DebugKeysPass1Pitch();
    if (p1p != 0.f) {
        const Quat pq = Quat::AxisAngle(r, Deg2Rad(p1p));
        u = pq.Rotate(u); fw = pq.Rotate(fw);
    }
    // Positional head tracking: nothing downstream adds an HMD offset any more, so the eye
    // we compute is the eye that gets rendered.
    Vec3 pos = f.eye;
    // Stereo: displace to this frame's eye. The offset is measured in the HEAD's frame, so it
    // is rotated by the head basis - not by the per-eye canted one, which would shear the two
    // eyes apart by the cant angle.
    if (stereo) {
        const Vec3& o = el.req.offset;
        pos = pos + (haveHeadBasis ? qHead.Rotate(o) : (r * o.x + u * o.y + fw * o.z));
    }

    uint8_t* pcs[4]; int npc = AllPersCams(pcs);
    for (int i = 0; i < npc; ++i) WriteCamMatrix(pcs[i] + off::Cam_Matrix, r, u, fw, pos);
    // Widen the frustum so one rendered image can contain a whole eye's field of view. The
    // value comes from the render thread, which is the only side that knows what the headset
    // needs; it then crops back down to the runtime's exact asymmetric FOV, so too wide only
    // costs resolution while too narrow costs field of view.
    // [FirstPerson] ForceFovDeg: write Cam_Fov WITHOUT a VR session, so the fov-semantics
    // A/B (which height does a WRITTEN fov apply across - boot or current?) can run in the
    // flat harness. The stereo path never sets it; 0 = off.
    const bool fovOn = (stereo && el.req.fovWrite > 0.01f) || Cfg().fpForceFovDeg > 1.f;
    if (fovOn) {
        float fov = Cfg().fpForceFovDeg > 1.f ? Deg2Rad(Cfg().fpForceFovDeg) : el.req.fovWrite;
        // Before overwriting: did the game change the FOV back since OUR last write?
        // A single foreign read is NOT enough: the game resets the FOV occasionally in
        // ordinary play too (measured 2026-08-22: 9% of pre-write samples were the game's
        // 0.838 during pure gameplay - and those isolated blips kept the guard's 3 s tail
        // refreshed for a whole session, which is the "always in cinema mode" bug). What
        // separates a takeover from a blip is PERSISTENCE: with one FOV-write call per
        // frame, the map/cutscene camera wins the write race every consecutive frame
        // (16 consecutive in the map burst at 14:41), while gameplay blips are isolated.
        // So arm only on a streak - ~20 frames = ~300 ms of map, far less time than it
        // takes to pick a destination, and a length ordinary play never strings together.
        const float lastW = g_lastFovWrite.load(std::memory_order_relaxed);
        float cur = 0.f;
        if (npc > 0 && lastW > 0.01f && SafeRead(pcs[0] + off::Cam_Fov, &cur, 4)) {
            // Majority over a sliding window, NOT a consecutive streak. With stereo live
            // this code runs twice per frame, and only PASS 1 sees the menu camera's value
            // (pass 2 pre-reads our own pass-1 write) - so a takeover reads foreign, ours,
            // foreign, ours... and a consecutive streak NEVER arms. That is exactly how the
            // 14:53 fast-travel crash got through: map open, guard silent. Rates measured
            // 2026-08-22: map/menu >= 50% of reads foreign (100% in mono), ordinary
            // gameplay <= 9% in isolated blips. 16-of-40 (40%) splits those cleanly and
            // arms within ~13 frames of the map opening.
            static uint8_t s_win[40] = {};
            static int s_head = 0, s_sum = 0;
            const uint8_t f = std::fabs(cur - lastW) > 0.05f ? 1 : 0;
            s_sum += f - s_win[s_head];
            s_win[s_head] = f;
            s_head = (s_head + 1) % 40;
            if (s_sum >= 16) {
                g_foreignFovTick.store(GetTickCount64(), std::memory_order_relaxed);
                static long s_logged = 0;
                if (s_logged < 16 && ++s_logged)
                    LOGI("[cam] foreign FOV %.3f (ours %.3f), %d of last 40 reads - "
                         "menu/cutscene camera owns the view", cur, lastW, s_sum);
            }
        }
        if (!WorldDyingNow()) {   // fov writes stand down with the matrix writes
            for (int i = 0; i < npc; ++i) SafeWrite(pcs[i] + off::Cam_Fov, &fov, 4);
            if (uint8_t* cc = ChrCam()) SafeWrite(cc + off::Cam_Fov, &fov, 4);
            g_lastFovWrite.store(fov, std::memory_order_relaxed);
        }
    }
    // World-origin-shift telemetry: an origin rebase (BeginShiftWorldPosition, group 160)
    // moves every world coordinate at once, so the camera jumps farther in one frame than any
    // real motion could. Logged so in-headset glitch reports can be lined up against shifts -
    // the 2026-08-20 "geometry shifting/teleport ~1 min in" started at the first rebase.
    { static Vec3 s_prevPos{}; static bool s_havePrev = false;
      if (s_havePrev) {
          const float jump = (pos - s_prevPos).Length();
          if (jump > 20.f) {
              // Rebases move ~32 m; anything much larger is a teleport (fast travel, grace
              // warp) - suspend the second pass while the world is torn down and rebuilt
              // around it (fast travel's fade fired pass-2 DL_PANICs on 2026-08-21).
              if (jump > 100.f) g_bigJumpTick.store(GetTickCount64(), std::memory_order_relaxed);
              LOGI("[shift] camera jumped %.1f m in one frame - %s", jump,
                   jump > 100.f ? "TELEPORT (pass 2 suspended)" : "world-origin rebase");
          }
      }
      s_prevPos = pos; s_havePrev = true; }
    g_lastWrittenPos = pos; g_lastCamRoot = f.root;
    { std::lock_guard<std::mutex> g(g_camMtxLock);
      float m[16] = { r.x, r.y, r.z, 0.f, u.x, u.y, u.z, 0.f, fw.x, fw.y, fw.z, 0.f, pos.x, pos.y, pos.z, 1.f };
      memcpy(g_lastCamMtx, m, sizeof(m)); g_haveCamMtx.store(true, std::memory_order_relaxed); }
    if (uint8_t* cc = ChrCam()) WriteCamMatrix(cc + off::Cam_Matrix, r, u, fw, pos);
    if (uint8_t* fc = (uint8_t*)g_followCamPtr.load(std::memory_order_relaxed)) WriteCamMatrix(fc + off::Cam_Matrix, r, u, fw, pos);
    ApplyFaceCamera(player, f.root, f.bodyYaw);
    if (c.fpWriteAim) {
        Quat q = Quat::YawQ(f.baseYaw) * f.headQ;
        Vec3 ar = q.Rotate({1, 0, 0}), au = q.Rotate({0, 1, 0}), af = q.Rotate({0, 0, 1});
        WriteCamMatrix(player + off::Chr_AimViewMtx, ar, au, af, f.eye);   // nobody adds to this one: use the real eye
    }
    // Tell the render thread what this frame actually is. The FOV is READ BACK rather than
    // assumed: whatever the game (or the override above) left in Cam_Fov is what the image
    // gets rasterised with, and the crop has to match it or the world comes out the wrong
    // size. The head pose goes back in XR/stage convention - ToGame is its own inverse -
    // because that is the frame the compositor reprojects in.
    if (stereo) {
        float fov = 0.f;
        if (npc > 0) SafeRead(pcs[0] + off::Cam_Fov, &fov, 4);
        StereoEyeReport rep;
        rep.eye = el.req.eye;
        rep.pair = el.req.pair;
        rep.fovY = (fov > 0.05f && fov < 3.2f) ? fov : 0.f;
        rep.headPos = ToGame(f.headP);
        rep.headRot = ToGame(f.headQ);
        // The report is NOT pushed here: the game's own step re-smooths the final camera
        // after this write (measured 20:13 session: yaw off in 3270 of 3270 checks,
        // position off >5 cm in half), so the image is rendered with a camera this pose
        // does not describe - a standing image/pose mismatch the compositor turns into
        // head-motion skew. The report is STASHED and completed at pass end by
        // GameERFinalizePassReport, which reads the camera the renderer really used.
        // Pass 2's re-assert calls come through here too with the frame's PASS-1 latch -
        // they must not overwrite pass 2's own pending report (stashed by
        // GameERApplySecondEyeCamera between the passes).
        if (FramePumpCurrentPass() != 2)
            StashPendingReport(rep, Quat::FromBasis(r, u, fw), pos);
        // Full mode: stash the second eye for the pump. Overwritten by every UpdateCamera
        // call in the frame, so the pump gets the LAST (freshest) camera state, exactly
        // like the report queue does.
        std::lock_guard<std::mutex> g(g_secondEye.mtx);
        g_secondEye.valid = el.req.haveOther;
        if (el.req.haveOther) {
            g_lastStashTick.store(GetTickCount64(), std::memory_order_relaxed);
            g_secondEye.haveHeadBasis = haveHeadBasis;
            g_secondEye.qHead = qHead;
            g_secondEye.r = r; g_secondEye.u = u; g_secondEye.fw = fw;
            g_secondEye.posBase = f.eye;
            g_secondEye.offset = el.req.otherOffset;
            g_secondEye.rot = el.req.otherRot;
            g_secondEye.eye = el.req.otherEye;
            g_secondEye.pair = el.req.pair;
            g_secondEye.fovY = rep.fovY;
            g_secondEye.headPos = rep.headPos;
            g_secondEye.headRot = rep.headRot;
        }
    }
}

// ---------------------------------------------------------------- arms
void SolveArm(const Skeleton& s, HkQsTransform* pose, std::vector<HkQsTransform>& local, WorldPose& w, int iUA, int iFA, int iHand,
              const Vec3& target, const Quat& targetQ, const Vec3& pole) {
    if (iUA < 0 || iFA < 0 || iHand < 0) return;
    if (s.parents[iFA] != iUA || s.parents[iHand] != iFA) return;
    const HkQsTransform& rFA = s.refPoseCopy[iFA]; const HkQsTransform& rHand = s.refPoseCopy[iHand];
    Vec3 axisUA{rFA.t[0], rFA.t[1], rFA.t[2]}, axisFA{rHand.t[0], rHand.t[1], rHand.t[2]};
    float L1 = axisUA.Length(), L2 = axisFA.Length();
    if (L1 < 0.05f || L2 < 0.05f) return;
    axisUA = axisUA * (1.f / L1); axisFA = axisFA * (1.f / L2);

    Vec3 S = w.pos[iUA];
    Vec3 toT = target - S; float d = toT.Length();
    if (d < 1e-4f) return;
    float maxReach = (L1 + L2) * 0.999f;
    float stretch = 1.f;
    if (d > maxReach) {
        float want = d / maxReach;
        stretch = std::fmin(want, std::fmax(1.f, Cfg().armStretch));
        L1 *= stretch; L2 *= stretch; maxReach *= stretch;
        if (d > maxReach) { toT = toT * (maxReach / d); d = maxReach; }
    }
    Vec3 dir = toT * (1.f / d);
    float cosA = Clamp((L1 * L1 + d * d - L2 * L2) / (2.f * L1 * d), -1.f, 1.f);
    float sinA = std::sqrt(std::fmax(0.f, 1.f - cosA * cosA));
    Vec3 perp = (pole - dir * pole.Dot(dir)).Normalized();
    if (perp.Length() < 1e-3f) perp = Vec3{0, -1, 0};
    Vec3 E = S + dir * (L1 * cosA) + perp * (L1 * sinA);
    Vec3 T = S + toT;

    // Upper arm: rotate so its child axis points at the elbow.
    int pUA = s.parents[iUA];
    Quat parentQ = pUA >= 0 ? w.q[pUA] : Quat{};
    Vec3 curAxis = w.q[iUA].Rotate(axisUA);
    Quat qUA = (Quat::FromTo(curAxis, (E - S).Normalized()) * w.q[iUA]).Normalized();
    Quat lUA = (parentQ.Conj() * qUA).Normalized();
    // Forearm: child axis points at the wrist.
    const HkQsTransform& curFA = local[iFA];
    Quat lFAcur{curFA.q[0], curFA.q[1], curFA.q[2], curFA.q[3]};
    Quat qFAcur = qUA * lFAcur;
    Vec3 curAxisFA = qFAcur.Rotate(axisFA);
    Quat qFA = (Quat::FromTo(curAxisFA, (T - E).Normalized()) * qFAcur).Normalized();
    Quat lFA = (qUA.Conj() * qFA).Normalized();
    // Hand: absolute orientation from the controller.
    Quat lHand = (qFA.Conj() * targetQ).Normalized();

    auto writeQ = [&](int i, const Quat& q) {
        HkQsTransform& t = local[i]; t.q[0] = q.x; t.q[1] = q.y; t.q[2] = q.z; t.q[3] = q.w;
        SafeWrite(&pose[i].q[0], t.q, 16);
    };
    auto writeT = [&](int i, float scale) {   // stretch bone length along its (reference) offset
        const HkQsTransform& r = s.refPoseCopy[i]; HkQsTransform& t = local[i];
        t.t[0] = r.t[0] * scale; t.t[1] = r.t[1] * scale; t.t[2] = r.t[2] * scale;
        SafeWrite(&pose[i].t[0], t.t, 16);
    };
    writeQ(iUA, lUA); writeQ(iFA, lFA); writeQ(iHand, lHand);
    if (stretch != 1.f) { writeT(iFA, stretch); writeT(iHand, stretch); }
    w.q[iUA] = qUA; w.q[iFA] = qFA; w.q[iHand] = targetQ; w.pos[iFA] = E; w.pos[iHand] = T;
}

// Yaw the character root faces: local -Z of the (physics) root, the convention verified against the lock-on
// point. Deliberately *not* built from the Master bone: for c0000 that bone's own orientation is not identity, so
// its -Z is not the body's forward, and aligning it to the view spun the whole body ~90 degrees (log: "rendered
// facing-cam 0.0, shoulder-line-cam -98.5").
float BodyFacingYaw(const Quat& rootQ, const WorldPose&) {
    Vec3 fwd = rootQ.Rotate({0, 0, -1});
    return WrapPi(std::atan2(fwd.x, fwd.z) + Deg2Rad(Cfg().bodyFacingOffsetDeg));
}
// Yaw the rendered shoulders face (perpendicular to the L/R clavicle line). Which clavicle is on which side is
// model-authoring dependent, so the 180-degree ambiguity is resolved at runtime against the root facing, measured
// on the animation's own (unrotated) pose where the two must agree to within a few tens of degrees.
bool g_shoulderFlip = false;
float ShoulderLineYawRaw(const WorldPose& w) {
    if (g_skel.L_clav < 0 || g_skel.R_clav < 0) return 0.f;
    Vec3 right = w.pos[g_skel.R_clav] - w.pos[g_skel.L_clav]; right.y = 0.f;
    return std::atan2(-right.z, right.x);
}
float ShoulderLineYaw(const WorldPose& w) { return WrapPi(ShoulderLineYawRaw(w) + (g_shoulderFlip ? 3.14159265f : 0.f)); }
void CalibrateShoulderFlip(const Quat& rootQ, const WorldPose& w) {
    if (g_skel.L_clav < 0 || g_skel.R_clav < 0) return;
    float d = std::fabs(WrapPi(ShoulderLineYawRaw(w) - BodyFacingYaw(rootQ, w)));
    bool was = g_shoulderFlip;
    if (d < Deg2Rad(60.f)) g_shoulderFlip = false; else if (d > Deg2Rad(120.f)) g_shoulderFlip = true;
    if (was != g_shoulderFlip) LOGI("[fp] shoulder line reads %s the root facing (%.0f deg off); %s", g_shoulderFlip ? "opposite to" : "along", Rad2Deg(d), g_shoulderFlip ? "flipping it" : "using it as is");
}

// Snapshot the animation's current pose as the PoseMode=vr template. Only ever called when the pose buffer
// holds the game's own output (skeleton build, or a fresh buffer while standing still), never our writes.
bool CaptureTemplate(uint8_t* chr, const char* why) {
    if (g_skel.boneCount <= 0 || !g_skel.hkb) return false;
    HkQsTransform* pose = g_skel.LivePose(); if (!pose) return false;
    std::vector<HkQsTransform> t(g_skel.boneCount);
    if (!SafeRead(pose, t.data(), sizeof(HkQsTransform) * g_skel.boneCount)) return false;
    g_poseTemplate = std::move(t);
    Vec3 rp; Quat rq;   // shoulder flip must be calibrated on the game's own (facing-aligned) pose
    if (PlayerRoot(chr, rp, rq)) { WorldPose w; WorldFromLocal(g_skel, g_poseTemplate, rp, rq, w); CalibrateShoulderFlip(rq, w); }
    // Re-base the camera's head-rest height on the new template: keeping the old reference while the template's
    // head bone height changed (run capture -> idle capture, slopes) left the camera vertically offset for good.
    g_headRestHeight = -1.f;
    LOGI("[skel] pose template captured (%s)", why);
    return true;
}

void UpdateArms(uint8_t* player, const FrameInfo& f, int writePoint = 0) {
    const Config& c = Cfg();
    // Pose-write choke point: while the world is dying (death anim, warp, load) the game's
    // own animation (ragdoll, death anim) owns the body, and our bulk pose write is 720
    // bytes of floats going into memory teardown is about to free - stand down.
    if (WorldDyingNow()) return;
    if (g_skel.chr != player || !g_skel.hkb) { if (!BuildSkeleton(player)) return; }
    HkQsTransform* pose = g_skel.LivePose(); if (!pose) return;
    // Same root the camera anchor was built from - the bones must be solved in the frame they are drawn in.
    Vec3 rootPos = f.root; Quat rootQ = f.rootQ;
    std::vector<HkQsTransform> local;
    WorldPose w; if (!ComputeWorld(g_skel, pose, rootPos, rootQ, w, local)) return;
    // Pose-fight probe: was our last R_UpperArm write replaced by the game before this call?
    int probe = g_skel.R_ua;
    if (probe >= 0 && g_probeLastPoint >= 0) {
        const HkQsTransform& t = local[probe];
        float d = std::fabs(t.q[0] - g_probeQ.x) + std::fabs(t.q[1] - g_probeQ.y) + std::fabs(t.q[2] - g_probeQ.z) + std::fabs(t.q[3] - g_probeQ.w);
        if (d > 1e-3f) InterlockedIncrement(&g_poseForeign[g_probeLastPoint]);
        // Per-bone magnitude: how hard does the foreign layer move each tracked bone?
        for (size_t k = 0; k < g_probeBones.size(); ++k) {
            if (!g_probeHas[k]) continue;
            const HkQsTransform& b = local[g_probeBones[k]];
            float dot = std::fabs(b.q[0] * g_probeLastQ[k].x + b.q[1] * g_probeLastQ[k].y + b.q[2] * g_probeLastQ[k].z + b.q[3] * g_probeLastQ[k].w);
            float ang = Rad2Deg(2.f * std::acos(Clamp(dot, 0.f, 1.f)));
            if (ang > g_probeMaxDeg[k]) g_probeMaxDeg[k] = ang;
        }
    }

    Quat baseQ = Quat::YawQ(f.baseYaw);
    // Body follows head: shift the whole skeleton (root bones) so the character hangs under your view instead of
    // under its own root. The physics root does not move (no collision cheating) and the shift is clamped.
    //
    // BodyAnchor=head puts the character's head bone *at* the eye position, whatever the animation is doing -
    // running leans the body forward, a crouch drops it, the eye height differs from the model's. Measuring the
    // correction against the bone closes all of that, where shifting by the raw headset offset (BodyAnchor=root)
    // only ever corrects for you moving in the play space.
    Vec3 eyeW = f.eye;
    Vec3 headOff = eyeW - f.anchor, bodyOff{};
    // PoseMode=vr: the rig is the captured template, yawed to BodyYaw, every tick - the animation system can
    // write whatever it wants into the pose buffer, we replace all of it. Root motion (the character's actual
    // displacement, physics module) is untouched, so rolls and attacks still move you - they just don't pose you.
    if (c.fpPoseMode == "vr" && (int)g_poseTemplate.size() != g_skel.boneCount) {
        // Build-time capture failed (pose buffer not readable yet): retry, else vr mode would silently never start.
        double nowT = NowSec();
        if (nowT - g_templLastCapture > 2.0) { g_templLastCapture = nowT; CaptureTemplate(player, "retry"); }
    }
    CalibrateModelFacing(player);   // once: does the model matrix basis agree with the physics facing?
    bool vrPose = c.fpPoseMode == "vr" && (int)g_poseTemplate.size() == g_skel.boneCount;
    bool followsHead = !vrPose && c.bodyFollowsHead && c.fpCameraAnchor != "head";   // head anchor: the camera goes to the body, not the body to the camera
    if (!vrPose && !followsHead) CalibrateShoulderFlip(rootQ, w);
    if (vrPose) {
        // Stance re-capture: standing still for a second, take the animation's own pose as the new template
        // (idle after a run capture, crouch, sitting). "The animation's own" = the buffer differs from the
        // template on bones we never write; comparing bones we do write (roots, torso, arms) would see our own
        // previous output as "fresh animation" and bake our body-yaw rotation into the template (spinning rig).
        double nowT = NowSec();
        auto oursBone = [&](int i) {
            if (i == g_skel.torso || i == g_skel.L_ua || i == g_skel.L_fa || i == g_skel.L_hand ||
                i == g_skel.R_ua || i == g_skel.R_fa || i == g_skel.R_hand) return true;
            int p = g_skel.parents[i]; return p < 0 || p >= i;   // root bones: we yaw them to BodyYaw
        };
        if ((f.root - g_statPos).Length() > 0.03f) { g_statPos = f.root; g_statSince = nowT; }
        else if (nowT - g_statSince > 1.0 && nowT - g_templLastCapture > 3.0) {
            float diff = 0.f;
            for (int i = 0; i < g_skel.boneCount; ++i) {
                if (oursBone(i)) continue;
                for (int k = 0; k < 4; ++k) diff += std::fabs(local[i].q[k] - g_poseTemplate[i].q[k]);
            }
            g_templLastCapture = nowT;
            if (diff > 0.2f) CaptureTemplate(player, "standing still");
        }
        local = g_poseTemplate;
        // Yaw the whole rig (root bones, in the model-root frame) so the body faces BodyYaw regardless of
        // where the physics character points - same math as the BodyRotate step of the animated path.
        float facing = BodyFacingYaw(rootQ, w);
        Quat rotL = (rootQ.Conj() * Quat::YawQ(WrapPi(f.bodyYaw - facing)) * rootQ).Normalized();
        for (int i = 0; i < g_skel.boneCount; ++i) {
            int p = g_skel.parents[i];
            if (!(p < 0 || p >= i)) continue;
            HkQsTransform& t = local[i];
            Vec3 tv = rotL.Rotate(Vec3{t.t[0], t.t[1], t.t[2]});
            Quat q = (rotL * Quat{t.q[0], t.q[1], t.q[2], t.q[3]}).Normalized();
            t.t[0] = tv.x; t.t[1] = tv.y; t.t[2] = tv.z; t.q[0] = q.x; t.q[1] = q.y; t.q[2] = q.z; t.q[3] = q.w;
        }
        // Body follows head: put the rig's head bone under your eye, so physically stepping/leaning moves the
        // whole character with you instead of sliding the camera off a body that stays behind. The template is
        // re-applied from scratch every frame, so this is idempotent by construction - none of the per-bone
        // "what did we write last time" bookkeeping the animated path needs. Horizontal comes from the head
        // bone (measured on the template, no feedback); vertical is only your own crouch, so the rig can never
        // float off the ground trying to match a configured eye height.
        if (c.bodyFollowsHead) {
            Vec3 headTpl;
            if (HeadBoneTemplateWorld(rootPos, rootQ, f.bodyYaw, headTpl)) {
                Vec3 off{eyeW.x - headTpl.x, 0.f, eyeW.z - headTpl.z};
                float horiz = std::sqrt(off.x * off.x + off.z * off.z);
                if (horiz > c.bodyFollowMaxHorizontal && horiz > 1e-4f) { float k = c.bodyFollowMaxHorizontal / horiz; off.x *= k; off.z *= k; }
                off.y = Clamp(f.headRel.y, -c.bodyFollowMaxDown, c.bodyFollowMaxUp);
                bodyOff = off;
                // Convert into the frame the RENDERER uses, not the physics frame: root bone translations are
                // applied by the model matrix, so a shift converted with the physics rotation comes out rotated
                // (reversed, if the two disagree by 180) - the body stepping the wrong way.
                Vec3 lo = RenderRootQ(player, rootQ).Conj().Rotate(off);
                for (int i = 0; i < g_skel.boneCount; ++i) {
                    int p = g_skel.parents[i];
                    if (!(p < 0 || p >= i)) continue;
                    HkQsTransform& t = local[i];
                    t.t[0] += lo.x; t.t[1] += lo.y; t.t[2] += lo.z;
                }
            }
        }
        SafeWrite(pose, local.data(), sizeof(HkQsTransform) * g_skel.boneCount);
        WorldFromLocal(g_skel, local, rootPos, rootQ, w);
    }
    else if (followsHead) {
        if ((int)g_rootShiftWritten.size() != g_skel.boneCount) {
            g_rootShiftWritten.assign(g_skel.boneCount, {}); g_rootShiftBase.assign(g_skel.boneCount, {}); g_rootShiftHas.assign(g_skel.boneCount, 0);
            g_rootRotBase.assign(g_skel.boneCount, {}); g_rootRotWritten.assign(g_skel.boneCount, {});
        }
        auto recomputeWorld = [&]() {
            for (int k = 0; k < g_skel.boneCount; ++k) {
                const HkQsTransform& lt = local[k]; Vec3 v{lt.t[0], lt.t[1], lt.t[2]}; Quat q{lt.q[0], lt.q[1], lt.q[2], lt.q[3]};
                int pk = g_skel.parents[k];
                if (pk < 0 || pk >= k) { w.q[k] = rootQ * q; w.pos[k] = rootPos + rootQ.Rotate(v); }
                else { w.q[k] = w.q[pk] * q; w.pos[k] = w.pos[pk] + w.q[pk].Rotate(v); }
            }
        };
        auto isRoot = [&](int i) { int p = g_skel.parents[i]; return p < 0 || p >= i; };
        // Step 1: recover the *animation's* root transforms. This runs more than once per frame and the pose
        // buffer is not always refreshed in between, so if a root bone still holds exactly what we wrote last time
        // the untouched value is the base we remembered, not the buffer contents - otherwise every call would walk
        // (and spin) the skeleton a little further. Put the bases back into the local pose and rebuild the world
        // pose from them, so everything measured below is measured on the animation's own skeleton.
        bool anyRestored = false;
        for (int i = 0; i < g_skel.boneCount; ++i) {
            if (!isRoot(i)) continue;
            HkQsTransform& t = local[i];
            Vec3 cur{t.t[0], t.t[1], t.t[2]}, base = cur;
            Quat curQ{t.q[0], t.q[1], t.q[2], t.q[3]}, baseQ_ = curQ;
            if (g_rootShiftHas[i]) {
                Vec3 d = cur - g_rootShiftWritten[i];
                const Quat& wq = g_rootRotWritten[i];
                float dq = std::fabs(curQ.x - wq.x) + std::fabs(curQ.y - wq.y) + std::fabs(curQ.z - wq.z) + std::fabs(curQ.w - wq.w);
                if (std::fabs(d.x) + std::fabs(d.y) + std::fabs(d.z) < 1e-5f && dq < 1e-5f) { base = g_rootShiftBase[i]; baseQ_ = g_rootRotBase[i]; anyRestored = true; }
            }
            g_rootShiftBase[i] = base; g_rootRotBase[i] = baseQ_; g_rootShiftHas[i] = 1;
            t.t[0] = base.x; t.t[1] = base.y; t.t[2] = base.z;
            t.q[0] = baseQ_.x; t.q[1] = baseQ_.y; t.q[2] = baseQ_.z; t.q[3] = baseQ_.w;
        }
        if (anyRestored) recomputeWorld();
        CalibrateShoulderFlip(rootQ, w);   // on the animation's own pose, before we turn anything
        // Step 2: yaw the whole skeleton (about the vertical axis through the character root) so the shoulder line
        // faces BodyYaw *now*. The game only turns the physics character toward our lock-on point when moving or
        // when the error is large; idle it happily leaves the body 30-40 degrees (or, mid-turn, 180) off your head.
        // Measured on the base pose, so it is idempotent across repeated calls.
        Quat rotL{};   // extra rotation applied to every root bone, in the model-root frame
        if (c.bodyRotateToHead) {
            // Facing of the rendered body = the model root's forward. The physics/model root maps local -Z onto
            // the direction the character faces (verified: with the character facing our lock-on point, +Z read
            // back 180 degrees off). BodyFacingOffsetDeg is the manual override if a model disagrees.
            float facing = BodyFacingYaw(rootQ, w);
            {
                Quat Yw = Quat::YawQ(WrapPi(f.bodyYaw - facing));
                rotL = (rootQ.Conj() * Yw * rootQ).Normalized();
                for (int i = 0; i < g_skel.boneCount; ++i) {
                    if (!isRoot(i)) continue;
                    HkQsTransform& t = local[i];
                    Vec3 tv = rotL.Rotate(Vec3{t.t[0], t.t[1], t.t[2]});
                    Quat q = (rotL * Quat{t.q[0], t.q[1], t.q[2], t.q[3]}).Normalized();
                    t.t[0] = tv.x; t.t[1] = tv.y; t.t[2] = tv.z; t.q[0] = q.x; t.q[1] = q.y; t.q[2] = q.z; t.q[3] = q.w;
                }
                recomputeWorld();
            }
        }
        // Step 3: how far the (rotated, unshifted) skeleton has to move so it hangs under the view.
        bodyOff = headOff;
        if (c.bodyAnchor == "head" && g_skel.head >= 0) bodyOff = eyeW - w.pos[g_skel.head];
        float horiz = std::sqrt(bodyOff.x * bodyOff.x + bodyOff.z * bodyOff.z);
        if (horiz > c.bodyFollowMaxHorizontal && horiz > 1e-4f) { float k = c.bodyFollowMaxHorizontal / horiz; bodyOff.x *= k; bodyOff.z *= k; }
        bodyOff.y = Clamp(bodyOff.y, -c.bodyFollowMaxDown, c.bodyFollowMaxUp);
        Vec3 lo = rootQ.Conj().Rotate(bodyOff);
        // Step 4: write rotation + shift and propagate.
        for (int i = 0; i < g_skel.boneCount; ++i) {
            if (!isRoot(i)) continue;
            HkQsTransform& t = local[i];
            Vec3 nw = Vec3{t.t[0], t.t[1], t.t[2]} + lo;
            Quat nq{t.q[0], t.q[1], t.q[2], t.q[3]};
            g_rootShiftWritten[i] = nw; g_rootRotWritten[i] = nq;
            t.t[0] = nw.x; t.t[1] = nw.y; t.t[2] = nw.z;
            SafeWrite(&pose[i].t[0], t.t, 16);
            SafeWrite(&pose[i].q[0], t.q, 16);
        }
        recomputeWorld();
    }
    // Torso twist: rotate the Spine bone about world Y so the shoulders face the body yaw. This is the residual
    // after BodyRotate (which only knows the root facing): whatever yaw the animation puts into the root/spine
    // bones. Clamped, so a mis-read shoulder line can never fold the torso backwards.
    if (c.torsoFaceCamera && g_skel.torso >= 0 && g_skel.L_clav >= 0 && g_skel.R_clav >= 0 && (f.handValid[0] || f.handValid[1])) {
        {
            float shoulderYaw = ShoulderLineYaw(w);
            float delta = Clamp(WrapPi(f.bodyYaw - shoulderYaw), -Deg2Rad(60.f), Deg2Rad(60.f));
            int i = g_skel.torso, p = g_skel.parents[i];
            Quat parentQ = p >= 0 ? w.q[p] : Quat{};
            Quat newW = (Quat::YawQ(delta) * w.q[i]).Normalized();
            Quat lq = (parentQ.Conj() * newW).Normalized();
            HkQsTransform& t = local[i]; t.q[0] = lq.x; t.q[1] = lq.y; t.q[2] = lq.z; t.q[3] = lq.w;
            SafeWrite(&pose[i].q[0], t.q, 16);
            // recompute world transforms from the modified local pose (cheap: local copy already updated)
            for (int k = 0; k < g_skel.boneCount; ++k) {
                const HkQsTransform& lt = local[k]; Vec3 v{lt.t[0], lt.t[1], lt.t[2]}; Quat q{lt.q[0], lt.q[1], lt.q[2], lt.q[3]};
                int pk = g_skel.parents[k];
                if (pk < 0 || pk >= k) { w.q[k] = rootQ * q; w.pos[k] = rootPos + rootQ.Rotate(v); }
                else { w.q[k] = w.q[pk] * q; w.pos[k] = w.pos[pk] + w.q[pk].Rotate(v); }
            }
        }
    }
    float handRotX = c.handRotOffX;
    DebugKeysHandRotX(&handRotX);   // Numpad 6/9 live calibration wins while active
    Quat calib = Quat::FromEulerYXZ(Deg2Rad(c.handRotOffY), Deg2Rad(handRotX), Deg2Rad(c.handRotOffZ));
    bool any = false;
    Vec3 tp[2]; Quat tq[2]; Quat tcq[2]; Vec3 tcf[2], tcu[2]; bool tok[2] = {false, false};
    for (int h = 0; h < 2; ++h) {
        if (!f.handValid[h]) continue;
        Quat cq = (baseQ * f.handQ[h] * calib).Normalized();          // controller orientation in game world
        Vec3 cf = cq.Rotate({0, 0, 1}), cu = cq.Rotate({0, 1, 0});     // controller forward / up (game frame)
        Quat q = cq;
        if (g_skel.handAxesOk[h]) q = Quat::MapPairs(g_skel.handFwd[h], g_skel.handThumb[h], cf, cu);   // hand-local -> world
        Vec3 p = f.anchor + baseQ.Rotate(f.handP[h] * c.handScale) - cf * c.handBackOffset;
        if (c.handOrigin == "head") p = p + headOff;   // hands are head-relative: re-add where the view actually is
        tp[h] = p; tq[h] = q; tcq[h] = cq; tcf[h] = cf; tcu[h] = cu; tok[h] = true;
    }
    // Two-handing: the free hand rides the weapon hand's hilt where it took hold - toward the pommel,
    // i.e. down the controller's handle (-up) or back along its aim (-forward) - so it follows the
    // (inertial, lagging) weapon instead of drifting off the shaft. It also takes the WEAPON
    // controller's orientation (mapped through its own hand's bone axes, so a left hand still grips
    // like a left hand) instead of its own free-floating controller, and sits TwoHandPinSideOffset m
    // off the hilt axis so the wrist clears the shaft instead of skewering it.
    {
        int pin = -1, wep = -1; float along = 0.f; bool handle = true;
        if (CombatTwoHandPin(&pin, &wep, &along, &handle) && pin >= 0 && wep >= 0 && tok[pin] && tok[wep]) {
            Vec3 axis = handle ? tcu[wep] : tcf[wep];                            // up the blade; pommel = -axis
            Vec3 side = (handle ? tcf[wep] : tcu[wep]).Cross(axis).Normalized();   // lateral, perpendicular to the hilt (sign verified in-game 2026-08-31)
            tp[pin] = tp[wep] + axis * -along + side * c.twoHandPinSideOffset;
            if (c.twoHandPinOrient)
                tq[pin] = g_skel.handAxesOk[pin] ? Quat::MapPairs(g_skel.handFwd[pin], g_skel.handThumb[pin], tcf[wep], tcu[wep]) : tcq[wep];
        }
    }
    for (int h = 0; h < 2; ++h) {
        if (!tok[h]) continue;
        float side = h == HandLeft ? -1.f : 1.f;
        Vec3 pole = Quat::YawQ(f.bodyYaw).Rotate(Vec3{side * c.elbowPoleOut, -c.elbowPoleDown, -c.elbowPoleBack});
        if (h == HandLeft) SolveArm(g_skel, pose, local, w, g_skel.L_ua, g_skel.L_fa, g_skel.L_hand, tp[h], tq[h], pole);
        else               SolveArm(g_skel, pose, local, w, g_skel.R_ua, g_skel.R_fa, g_skel.R_hand, tp[h], tq[h], pole);
        any = true;
    }
    g_status.armsActive = any;
    // Record what we left in the probe bone this pass, and report the fight once per 5 s.
    bool vrWrote = c.fpPoseMode == "vr" && (int)g_poseTemplate.size() == g_skel.boneCount;
    if (probe >= 0 && (any || vrWrote)) {
        const HkQsTransform& t = local[probe];
        g_probeQ = {t.q[0], t.q[1], t.q[2], t.q[3]}; g_probeLastPoint = writePoint;
        for (size_t k = 0; k < g_probeBones.size(); ++k) {
            const HkQsTransform& b = local[g_probeBones[k]];
            g_probeLastQ[k] = {b.q[0], b.q[1], b.q[2], b.q[3]}; g_probeHas[k] = 1;
        }
    }
    posescan::MarkPhase(writePoint);
    if (writePoint == 0) posescan::Drive(pose, sizeof(HkQsTransform) * (size_t)g_skel.boneCount);
    {
        static double s_pfLast = 0; double nowP = NowSec();
        if (nowP - s_pfLast > 5.0) {
            s_pfLast = nowP;
            long a = InterlockedExchange(&g_poseForeign[0], 0), b = InterlockedExchange(&g_poseForeign[1], 0), d = InterlockedExchange(&g_poseForeign[2], 0), e = InterlockedExchange(&g_poseForeign[3], 0);
            if (a + b + d + e) {
                std::string mags;
                for (size_t k = 0; k < g_probeBones.size(); ++k) {
                    if (g_probeMaxDeg[k] > 0.5f) { char buf[64]; snprintf(buf, 64, "%s %.1f ", g_probeNames[k].c_str(), g_probeMaxDeg[k]); mags += buf; }
                    g_probeMaxDeg[k] = 0.f;
                }
                LOGI("[pose] game re-wrote the arm pose %ld/%ld/%ld/%ld times after the pose hook / camera step / model-pos / post-havok write (5 s); max foreign deltas (deg): %s", a, b, d, e, mags.empty() ? "(none >0.5)" : mags.c_str());
            }
        }
    }
    if (c.fpDebug) {
        double now = NowSec();
        if (now - g_lastDebug > 1.0) {
            g_lastDebug = now;
            int hb = g_skel.Find("Head");
            Vec3 headW = hb >= 0 ? w.pos[hb] : Vec3{};
            float dmy[16]{}; Vec3 dmyP{};
            if (g_getDmyPos) { uint32_t id = 907; g_getDmyPos(player, dmy, &id, 1); dmyP = {dmy[12], dmy[13], dmy[14]}; }
            Vec3 phys; Quat physQ; Vec3 dRoot{};
            if (PlayerRoot(player, phys, physQ)) dRoot = rootPos - phys;
            LOGD("[fp] root(%.2f,%.2f,%.2f) model-phys(%.3f,%.3f,%.3f) anchor(%.2f,%.2f,%.2f) headBone(%.2f,%.2f,%.2f) anchor-head(%.2f,%.2f,%.2f) dmy907(%.2f,%.2f,%.2f) baseYaw %.1f | R hand tgt(%.2f,%.2f,%.2f) valid L%d R%d",
                 rootPos.x, rootPos.y, rootPos.z, dRoot.x, dRoot.y, dRoot.z, f.anchor.x, f.anchor.y, f.anchor.z, headW.x, headW.y, headW.z, f.anchor.x - headW.x, f.anchor.y - headW.y, f.anchor.z - headW.z, dmyP.x, dmyP.y, dmyP.z,
                 Rad2Deg(f.baseYaw), (f.anchor + headOff + baseQ.Rotate(f.handP[1])).x, (f.anchor + headOff + baseQ.Rotate(f.handP[1])).y, (f.anchor + headOff + baseQ.Rotate(f.handP[1])).z,
                 (int)f.handValid[0], (int)f.handValid[1]);
            LOGD("[fp] hmd raw(%.2f,%.2f,%.2f) origin(%.2f,%.2f,%.2f) rel(%.2f,%.2f,%.2f) bodyShift(%.2f,%.2f,%.2f) | chr-cam yaw %.1f, body-cam yaw %.1f, head-cam yaw %.1f deg | root facing-cam %.1f, shoulder-line-cam %.1f (flip %d) | locked %d",
                 f.headP.x, f.headP.y, f.headP.z, g_psOrigin.x, g_psOrigin.y, g_psOrigin.z, f.headRel.x, f.headRel.y, f.headRel.z, bodyOff.x, bodyOff.y, bodyOff.z,
                 Rad2Deg(WrapPi(f.chrYaw - f.baseYaw)), Rad2Deg(WrapPi(f.bodyYaw - f.baseYaw)), Rad2Deg(f.headYaw),
                 Rad2Deg(WrapPi(BodyFacingYaw(rootQ, w) - f.baseYaw)), Rad2Deg(WrapPi(ShoulderLineYaw(w) - f.baseYaw)), (int)g_shoulderFlip, (int)f.lockedOn);
        }
    }
}

// ---------------------------------------------------------------- hooks
bool FirstPersonActive() { return Cfg().fpEnabled && MainPlayer() != nullptr; }

int SehFilter(unsigned code) { LOGE("[game] exception 0x%08X inside hook; disabling first person for safety", code); return EXCEPTION_EXECUTE_HANDLER; }
bool g_tripped = false;

void DumpMatrix(const char* tag, uint8_t* cam) {
    float m[16]; if (!cam || !SafeRead(cam + off::Cam_Matrix, m, sizeof(m))) { LOGD("[cam] %s: unreadable", tag); return; }
    LOGD("[cam] %s r(%.2f,%.2f,%.2f,%.2f) u(%.2f,%.2f,%.2f,%.2f) f(%.2f,%.2f,%.2f,%.2f) p(%.2f,%.2f,%.2f,%.2f) fov %.3f", tag,
         m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15], *(float*)(cam + off::Cam_Fov));
}

double g_lastCamDump = 0;
float g_lastWrittenYaw = 0.f; bool g_haveWrittenYaw = false;
long g_camForeign = 0, g_camChecks = 0;   // how often the chr-cam held a yaw other than ours when we came to write

// Which camera machinery re-runs in the draw-only second pass. The renderer captures its
// camera SOMEWHERE the pass-2 keep set does not cover (writing every camera object at
// camera-step time in pass 2 changes nothing on screen); a hook that fires in pass 1 but
// never in pass 2 marks gated machinery - the capture point is downstream of one of those.
struct PassCount { volatile long p1 = 0, p2 = 0; };
PassCount g_cntFinalize, g_cntFollow, g_cntUpdFollow, g_cntMms, g_cntModelPos, g_cntPoseEval;
void CountPass(PassCount& c) {
    InterlockedIncrement(FramePumpCurrentPass() == 2 ? &c.p2 : &c.p1);
}
// Returns true when our camera was written.
bool CameraStepBody(bool& skipOriginal, bool afterOriginal) {
    const Config& c = Cfg();
    uint8_t* player = MainPlayer(); if (!player) return false;
    FrameInfo f; if (!BuildFrame(player, f)) return false;
    if (g_camWallRecovery && c.fpCameraMode != "passthrough") WR(g_camWallRecovery, (uint8_t)0);   // the game refreshes this global; keep it off
    if (g_haveWrittenYaw) {
        float y = GameCamYaw(); ++g_camChecks;
        if (std::fabs(WrapPi(y - g_lastWrittenYaw)) > Deg2Rad(1.f)) ++g_camForeign;
        // The FINAL render camera, position included - the chr-cam yaw check cannot see position smoothing there.
        if (uint8_t* pc = PersCam1()) {
            float m[16];
            if (SafeRead(pc + off::Cam_Matrix, m, sizeof(m))) {
                if (std::fabs(WrapPi(std::atan2(m[8], m[10]) - g_lastWrittenYaw)) > Deg2Rad(1.f)) InterlockedIncrement(&g_finalYawForeign);
                Vec3 dp{m[12] - g_lastWrittenPos.x, m[13] - g_lastWrittenPos.y, m[14] - g_lastWrittenPos.z};
                if (dp.Length() > 0.05f) InterlockedIncrement(&g_finalPosForeign);
            }
        }
    }
    // Always-on watchdog: if the chr-cam regularly holds someone else's yaw when we come to write, an unhooked
    // camera writer is still active - that is exactly what "the view orbits while strafing / the turn stick does
    // nothing" looks like, so say it loudly instead of hiding it behind Debug=true.
    {
        static double s_win = 0; double nw = NowSec();
        if (nw - s_win > 5.0) {
            if (g_camChecks > 50 && g_camForeign * 10 >= g_camChecks * 3)
                LOGW("[cam] foreign camera yaw in %ld of %ld writes in the last 5 s - an unhooked writer (lock-on camera?) is moving the view. FaceCameraMode=strafe avoids the game's lock-on camera.", g_camForeign, g_camChecks);
            long fy = InterlockedExchange(&g_finalYawForeign, 0), fp = InterlockedExchange(&g_finalPosForeign, 0);
            if (fy + fp) LOGI("[cam] final render camera differed from our write: yaw off in %ld, position off (>5 cm) in %ld of %ld checks (5 s) - the game step re-smooths the final camera", fy, fp, g_camChecks);
            // Per-pass camera-machinery census (only meaningful while the pump runs).
            {
                long f1 = InterlockedExchange(&g_cntFinalize.p1, 0), f2 = InterlockedExchange(&g_cntFinalize.p2, 0);
                long o1 = InterlockedExchange(&g_cntFollow.p1, 0),   o2 = InterlockedExchange(&g_cntFollow.p2, 0);
                long u1 = InterlockedExchange(&g_cntUpdFollow.p1, 0),u2 = InterlockedExchange(&g_cntUpdFollow.p2, 0);
                long m1 = InterlockedExchange(&g_cntMms.p1, 0),      m2 = InterlockedExchange(&g_cntMms.p2, 0);
                long d1 = InterlockedExchange(&g_cntModelPos.p1, 0), d2 = InterlockedExchange(&g_cntModelPos.p2, 0);
                long e1 = InterlockedExchange(&g_cntPoseEval.p1, 0), e2 = InterlockedExchange(&g_cntPoseEval.p2, 0);
                if (f2 + o2 + u2 + m2 + d2 + e2 + f1 + o1 + u1 + m1 + d1 + e1)
                    LOGI("[cam] 5s hook calls pass1/pass2: finalize %ld/%ld follow %ld/%ld updFollow %ld/%ld mms %ld/%ld modelPos %ld/%ld poseEval %ld/%ld",
                         f1, f2, o1, o2, u1, u2, m1, m2, d1, d2, e1, e2);
            }
            g_camForeign = 0; g_camChecks = 0; s_win = nw;
        }
    }
    bool dump = c.fpDebug && (NowSec() - g_lastCamDump > 3.0);
    if (dump && !afterOriginal) {
        g_lastCamDump = NowSec(); DumpMatrix("game chr_cam (before write)", ChrCam()); DumpMatrix("game pers_cam_1 (before write)", PersCam1());
        LOGD("[cam] foreign yaw found before %ld of %ld writes in this window; our yaw %.1f", g_camForeign, g_camChecks, Rad2Deg(f.baseYaw));
    }
    // fov-semantics probe: with ForceFovDeg armed, dump the pers-cam struct as floats a few
    // times, ~10 s in (after the late resize AND with our fov write long since applied). The
    // projection terms live somewhere in here - m11 = 1/tan(vFov/2) and m00 = m11/aspect -
    // and WHICH aspect appears (buffer 0.94 / 1.78 vs boot 1.78 vs desktop 2.39) answers,
    // from memory instead of screenshots, which height a WRITTEN fov applies across.
    if (c.fpForceFovDeg > 1.f) {
        static int s_dumps = 0; static double s_lastStructDump = 0;
        if (s_dumps < 3 && NowSec() - s_lastStructDump > 12.0) {
            ++s_dumps; s_lastStructDump = NowSec();
            if (uint8_t* pc = PersCam1()) {
                float v[80];
                if (SafeRead(pc, v, sizeof(v))) {
                    char line[1024]; int p = 0;
                    for (int i = 0; i < 80 && p < (int)sizeof(line) - 16; ++i)
                        p += snprintf(line + p, sizeof(line) - p, "%.4g ", v[i]);
                    LOGI("[cam] pers_cam_1 floats +0x00..+0x140: %s", line);
                }
            }
        }
    }
    ApplyHeadVisibility(player, c.fpHideHead, c.fpHideSheathed);
    if (c.fpCameraMode == "passthrough") { skipOriginal = false; return false; }
    if (c.fpCameraMode == "post" && !afterOriginal) { skipOriginal = false; return false; }   // come back after the original
    if (afterOriginal) {
        UpdateCamera(player, f); g_lastWrittenYaw = f.baseYaw; g_haveWrittenYaw = true;
        // Final word on the pose. The camera step runs after all Havok evaluation in the frame (libER taskgroup
        // order: HavokBehavior .. HavokClothUpdate -> CameraStep -> render), so a write here beats the upper-body
        // animation layer (attacks, aim) that evaluates after the pose hook and was still showing on arms/shoulders.
        if (c.handTracking) UpdateArms(player, f, 1);
        return true;
    }
    UpdateCamera(player, f);
    // fov-semantics probe, part 2 ([FirstPerson] ForcePitchDeg): pitch the written camera a
    // KNOWN angle; a landmark's on-screen displacement divided by it measures px/deg
    // directly, immune to camera position. Flat harness only; hot-reloads for one-session
    // before/after shots.
    if (c.fpForcePitchDeg != 0.f) GameERDebugPitchCamera(c.fpForcePitchDeg);
    g_lastWrittenYaw = f.baseYaw; g_haveWrittenYaw = true;
    if (dump) { DumpMatrix("ervr chr_cam (after write)", ChrCam()); DumpMatrix("ervr pers_cam_1 (after write)", PersCam1()); }
    skipOriginal = (c.fpCameraMode == "block");
    g_status.playerFound = true;
    return true;
}
// The game's follow cam has yaw state of its own (it turns behind a moving character) and writes it into the
// chr-cam from more than one place per frame: skipping the chr-cam update alone left the follow-cam output in
// the matrix at camera-step time, and the view flip-flopped between the game's yaw and ours every frame ("camera
// spinning like crazy"). So the rule is simple: after *every* place the game writes the camera that we know of
// (follow-cam function, chr-cam update, camera step), ours goes straight back in, and the dump counts how often
// a foreign matrix was still found before a write (a non-zero count = a writer we do not hook yet).
void ReassertCamera() {
    if (g_tripped || !FirstPersonActive() || Cfg().fpCameraMode != "pre") return;
    __try { bool d; CameraStepBody(d, false); } __except (SehFilter(GetExceptionCode())) { g_tripped = true; }
}
void __fastcall Hook_MmsUpdateChrCam(void* a) {
    CountPass(g_cntMms);
    g_origMms(a);
    ReassertCamera();
}
void __fastcall Hook_FollowCamFollow(void* fc, float dt, void* c) {
    CountPass(g_cntFollow);
    g_followCamPtr.store(fc, std::memory_order_relaxed);
    // Setting reset_camera disables the follow cam's position interpolation (erfps2 trick) so it stops fighting us.
    uint8_t saved = 0; bool have = false;
    if (!g_tripped && FirstPersonActive() && Cfg().fpCameraMode == "pre") {
        have = RD(((uint8_t*)fc) + OFF_FollowCam_ResetCamera, saved); if (have) WR(((uint8_t*)fc) + OFF_FollowCam_ResetCamera, (uint8_t)1);
        // The rest of erfps2's first-person follow-cam neutralization: no auto-rotation behind a moving
        // character, no lateral lock-on camera offset, no wall-recovery pushes. All three are camera writers
        // that run *after* our matrix writes and used to show up as the view orbiting / sliding on its own.
        WR(((uint8_t*)fc) + OFF_FollowCam_AutoRotation, (uint8_t)0);
        float zero = 0.f; SafeWrite(((uint8_t*)fc) + OFF_FollowCam_LockedOffset, &zero, 4);
        if (g_camWallRecovery) WR(g_camWallRecovery, (uint8_t)0);
    }
    g_origFollow(fc, dt, c);
    if (have) WR(((uint8_t*)fc) + OFF_FollowCam_ResetCamera, saved);
    if (Cfg().fpOwnChrCam) ReassertCamera();   // replace its output at the source
}
// Outer follow-cam update (erfps2's UPDATE_FOLLOW_CAM): it keeps writing camera output *after*
// ChrExFollowCam::follow returns, which was the writer the watchdog kept counting while moving.
void __fastcall Hook_UpdateFollowCam(void* fc) {
    CountPass(g_cntUpdFollow);
    g_followCamPtr.store(fc, std::memory_order_relaxed);
    g_origUpdFollow(fc);
    if (Cfg().fpOwnChrCam) ReassertCamera();
}
// Replay our camera after the game's own camera finalizer. Cheap by design: no frame rebuild, no bone solve -
// just the matrix we already computed, written back into every camera object we know of.
void ReassertCameraMatrix() {
    if (WorldDyingNow()) return;   // stand down with every other camera write
    if (!g_haveCamMtx.load(std::memory_order_relaxed)) return;
    float m[16];
    { std::lock_guard<std::mutex> g(g_camMtxLock); memcpy(m, g_lastCamMtx, sizeof(m)); }
    uint8_t* pcs[4]; int n = AllPersCams(pcs);
    for (int i = 0; i < n; ++i) SafeWrite(pcs[i] + off::Cam_Matrix, m, sizeof(m));
    if (uint8_t* cc = ChrCam()) SafeWrite(cc + off::Cam_Matrix, m, sizeof(m));
    if (uint8_t* fc = (uint8_t*)g_followCamPtr.load(std::memory_order_relaxed)) SafeWrite(fc + off::Cam_Matrix, m, sizeof(m));
}
// Pass-1 finalize call, captured fresh every frame for the pass-2 replay below. Pass 1
// always runs the full camera chain natively (our pass-2 gating never touches pass 1), so
// these are at most one frame old and their objects are the live camera system.
struct { std::atomic<void*> a{nullptr}, b{nullptr}, c{nullptr}, d{nullptr};
         std::atomic<ULONGLONG> tick{0}; } g_finArgs;

uint64_t __fastcall Hook_CamFinalize(void* a, void* b, void* c, void* d) {
    CountPass(g_cntFinalize);
    if (FramePumpCurrentPass() != 2) {
        g_finArgs.a.store(a, std::memory_order_relaxed);
        g_finArgs.b.store(b, std::memory_order_relaxed);
        g_finArgs.c.store(c, std::memory_order_relaxed);
        g_finArgs.d.store(d, std::memory_order_relaxed);
        g_finArgs.tick.store(GetTickCount64(), std::memory_order_release);
    }
    uint64_t rv = g_origCamFinalize(a, b, c, d);
    if (g_tripped || !FirstPersonActive() || !Cfg().fpCameraFinalWrite) return rv;
    __try { ReassertCameraMatrix(); } __except (SehFilter(GetExceptionCode())) { g_tripped = true; }
    return rv;
}

// Per-character model-matrix update, late in the frame. The pose probe measured the upper-body animation layer
// (walk sway, weapon arts) evaluating every frame AFTER the camera step - this hook runs later still, so the
// pose written here is what the renderer actually skins.
void UpdateChrModelPosBody(void* ctrl, bool post) {
    uint8_t* player = MainPlayer();
    if (!player || Deref(player + off::Chr_ChrCtrl) != (uint8_t*)ctrl) return;
    if (post) {
        // Skew probe: the model matrix the rig renders with this frame vs the root our camera write used.
        float p[4];
        if (SafeRead((uint8_t*)ctrl + off::Ctrl_ModelMatrix + 48, p, 16)) {
            Vec3 d{p[0] - g_lastCamRoot.x, 0.f, p[2] - g_lastCamRoot.z};
            float h = d.Length();
            if (h > g_skewMax) g_skewMax = h;
        }
        static double s_last = 0; double nw = NowSec();
        if (nw - s_last > 5.0) {
            s_last = nw;
            if (g_skewMax > 0.005f) LOGI("[cam] rig-root vs camera-root horizontal skew max %.0f mm (5 s) - nonzero while moving means the camera write used a stale root", g_skewMax * 1000.f);
            g_skewMax = 0.f;
        }
    }
    FrameInfo f; if (!BuildFrame(player, f)) return;
    UpdateArms(player, f, 2);
    // Camera last, from the model matrix that was just rebuilt - the very transform the rig renders with. The
    // camera step runs earlier in the frame, so a camera written there sits on a root that is up to a frame of
    // motion away from where the body ends up: the body then appears to slide toward a camera that looks static
    // while moving, and to snap into place when you stop. Writing here makes the two exact by construction.
    if (post && Cfg().fpCameraAfterModelPos) UpdateCamera(player, f);
    // Body-vs-camera probe: where the RENDERED head bone actually ends up versus where we put the eye, in the
    // view's own axes. "Disconnected when strafing" is a lateral number; if this reads near zero while the
    // disconnect is still visible, our placement is correct and the separation is produced downstream (R.E.A.L.
    // renders one eye per frame, and lateral motion is exactly where that shows).
    if (post) {
        Vec3 hw;
        if (HeadBoneWorld(player, f.root, f.rootQ, hw)) {
            Vec3 d = hw - g_lastWrittenPos;
            float cy = std::cos(f.baseYaw), sy = std::sin(f.baseYaw);
            float lat = d.x * cy - d.z * sy, fwd = d.x * sy + d.z * cy;
            if (std::fabs(lat) > std::fabs(g_bodyCamLat)) g_bodyCamLat = lat;
            if (std::fabs(fwd) > std::fabs(g_bodyCamFwd)) g_bodyCamFwd = fwd;
            if (std::fabs(d.y) > std::fabs(g_bodyCamUp)) g_bodyCamUp = d.y;
        }
        static double s_bc = 0; double nb = NowSec();
        if (nb - s_bc > 5.0) {
            s_bc = nb;
            if (std::fabs(g_bodyCamLat) > 0.02f || std::fabs(g_bodyCamFwd) > 0.02f || std::fabs(g_bodyCamUp) > 0.02f)
                LOGI("[body] rendered head vs camera eye (5 s max): lateral %+.0f mm, forward %+.0f mm, vertical %+.0f mm", g_bodyCamLat * 1000.f, g_bodyCamFwd * 1000.f, g_bodyCamUp * 1000.f);
            g_bodyCamLat = g_bodyCamFwd = g_bodyCamUp = 0.f;
        }
        g_vrCycle.fetch_add(1, std::memory_order_relaxed);   // next cycle takes a fresh head sample
    }
}
// Post-Havok pose reassert. This function runs for every character, so the common case must be nearly free:
// read one bone, and only if it is no longer what we wrote does the full solve run (once per frame, player only).
void RewriteIfClobbered() {
    if (g_skel.boneCount <= 0 || g_probeLastPoint < 0) return;
    int probe = g_skel.R_ua; if (probe < 0) return;
    HkQsTransform* pose = g_skel.LivePose(); if (!pose) return;
    float q[4]; if (!SafeRead(&pose[probe].q[0], q, 16)) return;
    float d = std::fabs(q[0] - g_probeQ.x) + std::fabs(q[1] - g_probeQ.y) + std::fabs(q[2] - g_probeQ.z) + std::fabs(q[3] - g_probeQ.w);
    if (d < 1e-4f) return;   // still ours: the animation has not touched the player this call
    uint8_t* player = MainPlayer(); if (!player) return;
    FrameInfo f; if (!BuildFrame(player, f)) return;
    UpdateArms(player, f, 3);
}
uint64_t __fastcall Hook_PoseEval(void* a, void* b, void* c, void* d) {
    CountPass(g_cntPoseEval);
    uint64_t rv = g_origPoseEval(a, b, c, d);
    if (g_tripped || !FirstPersonActive() || !Cfg().fpPoseAfterHavok || !Cfg().handTracking) return rv;
    __try { RewriteIfClobbered(); } __except (SehFilter(GetExceptionCode())) { g_tripped = true; }
    return rv;
}
void __fastcall Hook_UpdateChrModelPos(void* ctrl) {
    CountPass(g_cntModelPos);
    // Pre-write as well: the buffer measured OURS after this hook still rendered the layer's animation, so the
    // renderer's pose consumption happens INSIDE this function - the pose present at entry is what gets skinned.
    // The upper-body layer ran just before us; put ours back first, then let the original consume it.
    bool act = !g_tripped && FirstPersonActive() && Cfg().handTracking;
    if (act) { __try { UpdateChrModelPosBody(ctrl, false); } __except (SehFilter(GetExceptionCode())) { g_tripped = true; act = false; } }
    g_origUpdModelPos(ctrl);
    if (act) { __try { UpdateChrModelPosBody(ctrl, true); } __except (SehFilter(GetExceptionCode())) { g_tripped = true; } }
}
void HookCameraStepImpl(void* a, void* b);
// Near-camera dither fade. The renderer asks, per drawn part, for the part's dither entry and dithers it
// when entry.nearEnd exceeds the part's distance from the camera; the pixel shader then gets
// 1/(nearEnd-nearStart) and fades the part across that band. Tuned for a third-person camera metres
// behind the character, the band reaches well past a head-mounted one: walls, doorways and anything you
// lean toward go see-through long before you touch them. NearDither=0 answers "no entry" (erfps2's
// first-person behaviour, no fade at all); 0<s<1 scales the band on the copy the game just made - *out
// is the caller's stack local, so the game's own table is never modified. Render-thread hot path: only
// atomics are read here.
bool __fastcall Hook_UsesDithering(void* mgr, void* out, uint32_t id) {
    if (!g_ditherFp.load(std::memory_order_relaxed)) return g_origUsesDithering(mgr, out, id);
    const float scale = g_ditherScale.load(std::memory_order_relaxed);
    if (scale <= 0.f) return false;
    const bool hit = g_origUsesDithering(mgr, out, id);
    if (hit && out) {
        float* f = (float*)out;
        static std::atomic<int> s_logged{0};   // confirm the entry layout from live data, a few times
        if (s_logged.load(std::memory_order_relaxed) < 4 && s_logged.fetch_add(1) < 4)
            LOGI("[dither] entry id %u: a0 %.3f a1 %.3f near %.3f..%.3f x 0x%08X (NearDither %.2f)",
                 id, f[1], f[2], f[3], f[4], ((uint32_t*)out)[5], scale);
        if (scale < 1.f) { f[3] *= scale; f[4] *= scale; }
    }
    return hit;
}
void __fastcall Hook_CameraStep(void* a, void* b) {
    InterlockedIncrement(&g_camCalls);
    ScopedTicks st(&g_camTicks);
    HookCameraStepImpl(a, b);
}
void HookCameraStepImpl(void* a, void* b) {
    // Pass 2 is the SAME sim frame drawn again - keep every per-frame cache (BuildFrame,
    // eye latch) on pass 1's values by not advancing the frame counter.
    if (FramePumpCurrentPass() != 2) ++g_frame;
    bool skip = false;
    const bool fpOn = !g_tripped && FirstPersonActive();
    g_ditherFp.store(fpOn, std::memory_order_relaxed);
    g_ditherScale.store(Cfg().fpNearDither, std::memory_order_relaxed);
    if (fpOn) {
        __try { CameraStepBody(skip, false); }
        __except (SehFilter(GetExceptionCode())) { g_tripped = true; skip = false; }
    }
    if (skip) return;
    g_origCameraStep(a, b);
    if (!g_tripped && FirstPersonActive() && (Cfg().fpCameraMode == "post" || Cfg().fpCameraMode == "pre")) {
        __try { bool dummy; CameraStepBody(dummy, true); }
        __except (SehFilter(GetExceptionCode())) { g_tripped = true; }
    }
    // FINALIZE REPLAY: under the SAFE keep set the camera finalizer's native caller (the
    // GameFlowInGame_MoveMap_PostPhysics family) is gated out of pass 2, so the second-eye
    // camera written above never reaches the renderer's view state - the "no depth, same
    // eye twice" failure. Pass 1 captured the finalizer's arguments this very frame; calling
    // the game's own finalizer here, after the second-eye write, propagates eye 2 without
    // running ANY of the MoveMap family's world/grid walking - which is the entire reason
    // that family is gated. Only when the SAFE set is live (native finalize covers FULL).
    if (FramePumpCurrentPass() == 2 && Cfg().vrPass2FinalizeReplay && FramePumpSafeSetActive() &&
        g_origCamFinalize && !g_tripped) {
        const ULONGLONG t = g_finArgs.tick.load(std::memory_order_acquire);
        if (t && GetTickCount64() - t < 200) {
            __try {
                g_origCamFinalize(g_finArgs.a.load(std::memory_order_relaxed),
                                  g_finArgs.b.load(std::memory_order_relaxed),
                                  g_finArgs.c.load(std::memory_order_relaxed),
                                  g_finArgs.d.load(std::memory_order_relaxed));
                static std::atomic<bool> s_logged{false};
                if (!s_logged.exchange(true))
                    LOGI("[stereo] pass-2 finalize replay ACTIVE (SAFE set): second eye reaches "
                         "the renderer without the MoveMap family");
            } __except (SehFilter(GetExceptionCode())) { g_tripped = true; }
        }
    }
}

// Locomotion=direct: ERVR moves the character. Horizontal position integrated from the processed stick in the view
// frame; height left to the game; facing = view yaw. Only while the stick is pushed - idle, the game owns the
// character again so its own animations (roll, attack, ladder, mount) still displace it. No collision from us.
Vec3 g_locoPos{}; bool g_locoActive = false; double g_locoLast = 0; unsigned g_locoFrame = 0;
void DirectLocomotion(uint8_t* player, const FrameInfo& f) {
    const Config& c = Cfg();
    if (c.locomotion != "direct") { g_locoActive = false; return; }
    // Do not move or re-orient the character while the world is dying (see WorldDyingNow).
    if (WorldDyingNow()) { g_locoActive = false; return; }
    if (g_locoFrame == g_frame) return; g_locoFrame = g_frame;   // once per rendered frame
    MoveIntent mi = VirtualPadGetMove();
    double now = NowSec(); float dt = Clamp((float)(now - g_locoLast), 0.f, 0.05f); g_locoLast = now;
    uint8_t* ph = Physics(player); if (!ph) return;
    float p[4]; if (!SafeRead(ph + off::Phys_Position, p, 16)) return;
    float mag = std::sqrt(mi.x * mi.x + mi.y * mi.y);
    if (!mi.valid || mag < 0.15f) { g_locoActive = false; return; }
    if (!g_locoActive) { g_locoPos = {p[0], p[1], p[2]}; g_locoActive = true; return; }
    if (mag > 1.f) { mi.x /= mag; mi.y /= mag; mag = 1.f; }
    float speed = (mi.sprint ? c.locoSprintSpeed : c.locoRunSpeed) * mag;
    // The stick arrives in the frame of the camera matrix the game sees (VirtualPadUpdate rotates it into that
    // frame), which includes the HMD yaw when CameraRotation=hmd - same rule as MoveBasis::viewYaw.
    float frameYaw = (f.headValid && c.fpCameraRotation == "hmd") ? WrapPi(f.baseYaw + f.headYaw) : f.baseYaw;
    Vec3 dir = Quat::YawQ(frameYaw).Rotate(Vec3{mi.x, 0.f, mi.y}); dir.y = 0.f;
    if (dir.Length() > 1e-4f) dir = dir.Normalized();
    g_locoPos = g_locoPos + dir * (speed * dt);
    // resync if the game moved us a long way (fall, teleport, ladder) so we never drag the character back
    Vec3 gp{p[0], p[1], p[2]};
    if ((Vec3{gp.x - g_locoPos.x, 0.f, gp.z - g_locoPos.z}).Length() > 1.5f) g_locoPos = gp;
    p[0] = g_locoPos.x; p[2] = g_locoPos.z; g_locoPos.y = p[1];
    SafeWrite(ph + off::Phys_Position, p, 16);
    // face the view (physics forward is local -Z); also pin the interpolation target or the game turns it back
    Quat q = Quat::YawQ(WrapPi(f.bodyYaw + 3.14159265f));
    float r[4] = {q.x, q.y, q.z, q.w};
    SafeWrite(ph + off::Phys_Orientation, r, 16);
    SafeWrite(ph + off::Phys_InterpOrient, r, 16);
}

void HkBody() {
    // The Havok update fires several times per rendered frame. Solving on every call is what keeps the arms
    // from being lost to a later animation evaluation (most visible while running); ArmSolveEveryCall=false
    // goes back to solving once per frame, which is cheaper but can drop a write.
    if (!Cfg().fpArmSolveEveryCall && g_status.cameraHooked && g_lastArmFrame == g_frame) return;
    g_lastArmFrame = g_frame;
    InterlockedIncrement(&g_hkSolves);
    ScopedTicks st(&g_hkTicks);
    uint8_t* player = MainPlayer(); if (!player) return;
    FrameInfo f; if (!BuildFrame(player, f)) return;
    // Re-assert the strafe flags here too: the camera hook runs once, at one point in the frame, and the game
    // is free to put the character back into free-turn before it reads them.
    ApplyFaceCamera(player, f.root, f.bodyYaw);
    // Full-frame pass 2 re-runs the behavior/pose machinery at dt~0: re-applying the ARMS
    // is wanted (pass 2's render must show the VR pose too), but movement and combat are
    // once-per-SIM-frame actions and must not double-fire.
    if (FramePumpCurrentPass() != 2) DirectLocomotion(player, f);
    UpdateArms(player, f);
}
void HkHandler(MidHookRegs*, void*) {
    InterlockedIncrement(&g_hkCalls);
    if (g_tripped || !Cfg().fpEnabled) return;
    if (Cfg().handTracking) {
        __try { HkBody(); }
        __except (SehFilter(GetExceptionCode())) { g_tripped = true; }
    }
    // Physical combat: inject the AttackBehavior event once per frame after the arm pose is written.
    if (g_status.cameraHooked && FramePumpCurrentPass() != 2) CombatGameFrame(MainPlayer(), g_frame);
}

// ---------------------------------------------------------------- init
bool VersionIs(const wchar_t* want) {
    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
    DWORD h; DWORD sz = GetFileVersionInfoSizeW(path, &h); if (!sz) return false;
    std::vector<uint8_t> buf(sz); if (!GetFileVersionInfoW(path, 0, sz, buf.data())) return false;
    VS_FIXEDFILEINFO* ffi; UINT len; if (!VerQueryValueW(buf.data(), L"\\", (void**)&ffi, &len)) return false;
    wchar_t v[64]; swprintf_s(v, L"%u.%u.%u.%u", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS), HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
    LOGI("[game] eldenring.exe version %S (want %S)", v, want);
    return wcscmp(v, want) == 0;
}

// erfps2 binary-mapper patterns: '$' marks a rel32 whose target is the function.
uint8_t* ResolveCallTarget(const char* pattern, const char* name) {
    std::string pat(pattern); size_t dollar = pat.find('$');
    if (dollar == std::string::npos) return nullptr;
    // count bytes before '$'
    int idx = 0; { std::string pre = pat.substr(0, dollar); size_t i = 0; while (i < pre.size()) { while (i < pre.size() && pre[i] == ' ') ++i; if (i >= pre.size()) break; ++idx; while (i < pre.size() && pre[i] != ' ') ++i; } }
    std::string scan = pat; scan.replace(dollar, 1, "?? ?? ?? ??");
    uint8_t* hit = AobScan(scan);
    if (!hit) { LOGW("[game] AOB for %s not found", name); return nullptr; }
    int32_t rel; memcpy(&rel, hit + idx, 4);
    return hit + idx + 4 + rel;
}

} // namespace

bool GameERWeaponMotion(int hand, float* tipSpeed, float* handSpeed, float* arc) {
    if (hand != HandLeft && hand != HandRight) return false;
    std::lock_guard<std::mutex> g(g_inertiaMtx);
    const InertiaState& s = g_inertia[hand];
    if (!s.init || NowSec() - s.stamp > 0.1) return false;
    if (tipSpeed) *tipSpeed = s.tipSpeed;
    if (handSpeed) *handSpeed = s.handSpeed;
    if (arc) *arc = s.arc;
    return true;
}

int GameERPlayerLevel() {
    if (!g_gameDataManPtr) return -1;
    uint8_t* gdm = Deref(g_gameDataManPtr); if (!gdm) return -1;
    uint8_t* pgd = Deref(gdm + 0x8); if (!pgd) return -1;
    int level = -1; SafeRead(pgd + 0x68, &level, 4);
    return level;
}

uint8_t* GameERWorldChrMan() { return WorldChrMan(); }

// Full stereo, pass 2: write the OTHER eye's camera into every slot the real pass wrote.
// Called by the frame pump between the two engine passes, on the main-loop thread, while
// every camera writer is quiescent (the camera step is gated for the pass that follows).
// Peek: is a second-eye camera stashed for this frame? Non-consuming - the frame pump uses
// it to tell "gameplay frame" (stash present every frame) from "menu/loading frame" (the
// game never stashes one there, by design). The stash gap IS the menu detector: the map,
// where fast travel is confirmed, produces a multi-second gap ending the instant the
// confirm closes it.
bool GameERSecondEyePending() {
    std::lock_guard<std::mutex> g(g_secondEye.mtx);
    return g_secondEye.valid;
}

// Tick of the last stash - unlike GameERSecondEyePending (consumed between the passes, so
// false through most of every frame) this reads the same from any thread at any point in
// the frame: fresh = gameplay, quiet for ~0.7 s+ = menu/loading/cinema.
unsigned long long GameERLastStashTick() {
    return g_lastStashTick.load(std::memory_order_relaxed);
}

bool GameERApplySecondEyeCamera() {
    bool haveHeadBasis; Quat qHead, rot; Vec3 br, bu, bf, posBase, offset;
    StereoEyeReport rep;
    {
        std::lock_guard<std::mutex> g(g_secondEye.mtx);
        if (!g_secondEye.valid) return false;
        g_secondEye.valid = false;   // consumed; UpdateCamera refills next frame
        haveHeadBasis = g_secondEye.haveHeadBasis;
        qHead = g_secondEye.qHead; rot = g_secondEye.rot;
        br = g_secondEye.r; bu = g_secondEye.u; bf = g_secondEye.fw;
        posBase = g_secondEye.posBase; offset = g_secondEye.offset;
        rep.eye = g_secondEye.eye; rep.pair = g_secondEye.pair;
        rep.fovY = g_secondEye.fovY;
        rep.headPos = g_secondEye.headPos; rep.headRot = g_secondEye.headRot;
    }
    Vec3 r, u, fw, pos;
    if (haveHeadBasis) {
        Quat q = qHead * rot;   // per-eye cant, identity on most headsets
        // Diagnostic: tilt ONLY the second pass's camera. Whichever headset eye shows the
        // tilted world is the eye that receives pass 2's CONTENT - the 30-second test for
        // whether presents carry their own pass's render or lag one behind ([VR] PresentLag).
        if (DebugPass2PitchDeg() != 0.f)
            q = q * Quat::AxisAngle({1, 0, 0}, Deg2Rad(DebugPass2PitchDeg()));
        r = q.Rotate({1, 0, 0}); u = q.Rotate({0, 1, 0}); fw = q.Rotate({0, 0, 1});
        pos = posBase + qHead.Rotate(offset);   // offset lives in the HEAD frame, not the canted one
    } else {
        r = br; u = bu; fw = bf;
        if (DebugPass2PitchDeg() != 0.f) {
            const Quat pq = Quat::AxisAngle(r, Deg2Rad(DebugPass2PitchDeg()));
            u = pq.Rotate(u); fw = pq.Rotate(fw);
        }
        pos = posBase + r * offset.x + u * offset.y + fw * offset.z;
    }
    uint8_t* pcs[4]; const int npc = AllPersCams(pcs);
    for (int i = 0; i < npc; ++i) WriteCamMatrix(pcs[i] + off::Cam_Matrix, r, u, fw, pos);
    if (uint8_t* cc = ChrCam()) WriteCamMatrix(cc + off::Cam_Matrix, r, u, fw, pos);
    if (uint8_t* fc = (uint8_t*)g_followCamPtr.load(std::memory_order_relaxed)) WriteCamMatrix(fc + off::Cam_Matrix, r, u, fw, pos);
    // Retain the finished basis: UpdateCamera re-applies it on every camera-step call during
    // pass 2 (the write above, alone, never reached the renderer - see SecondEyeStash).
    {
        std::lock_guard<std::mutex> g(g_secondEye.mtx);
        g_secondEye.camValid = true;
        g_secondEye.camR = r; g_secondEye.camU = u;
        g_secondEye.camF = fw; g_secondEye.camPos = pos;
    }
    // Held back until pass 2 ends - see PendingReport. The pump finalizes it right after
    // pass 2's queue_tasks returns, before the present that consumes it.
    StashPendingReport(rep, Quat::FromBasis(r, u, fw), pos);
    return true;
}

// Complete the pass's stashed report with the camera the renderer REALLY used and push it.
// Called by the frame pump on the frame thread right after each pass's queue_tasks returns
// (all camera writers and the smoother are done, the pass's Flip has presented, and the
// present that will consume this report has not run yet).
void GameERFinalizePassReport() {
    PendingReport pr;
    {
        std::lock_guard<std::mutex> g(g_pendingRepMtx);
        if (!g_pendingRep.valid) return;
        pr = g_pendingRep;
        g_pendingRep.valid = false;
    }
    static long s_corr = 0, s_skipped = 0; static float s_sumYawDeg = 0.f, s_sumPosCm = 0.f;
    if (Cfg().vrReportFinalCamera) {
        float m[16];
        uint8_t* pc = PersCam1();
        if (pc && SafeRead(pc + off::Cam_Matrix, m, sizeof(m))) {
            const Quat qFinal = Quat::FromBasis({m[0], m[1], m[2]}, {m[4], m[5], m[6]},
                                                {m[8], m[9], m[10]}).Normalized();
            const Vec3 pFinal{m[12], m[13], m[14]};
            Quat dq = (qFinal * pr.qWritten.Conj()).Normalized();   // world-frame delta, game space
            if (dq.w < 0.f) dq = {-dq.x, -dq.y, -dq.z, -dq.w};
            const Vec3 dp = pFinal - pr.pWritten;
            const float angDeg = Rad2Deg(2.f * std::acos(std::fmin(1.f, dq.w)));
            // A cut/rebase/teleport moves the camera wildly WITH the world - folding that
            // into the head pose would slam the layer. Correct only plausible smoothing.
            if (angDeg < 25.f && dp.Length() < 2.f) {
                const float ws = Clamp(Cfg().vrWorldScale, 0.25f, 4.f);
                const Quat dqXr = ToGame(dq).Normalized();
                const Vec3 dpXr = ToGame(dp) * (1.f / ws);
                pr.rep.headRot = (dqXr * pr.rep.headRot).Normalized();
                pr.rep.headPos = pr.rep.headPos + dpXr;
                ++s_corr; s_sumYawDeg += angDeg; s_sumPosCm += dp.Length() * 100.f;
            } else {
                ++s_skipped;
            }
        }
    }
    // 5 s digest: how far the game's smoother really moves the render camera off our write.
    static double s_win = 0; const double nw = NowSec();
    if (nw - s_win > 5.0) {
        if (s_corr || s_skipped)
            LOGD("[cam] report pose corrected to the FINAL render camera in %ld passes "
                 "(avg delta %.2f deg / %.1f cm), %ld skipped as cuts (5 s)",
                 s_corr, s_corr ? s_sumYawDeg / s_corr : 0.f, s_corr ? s_sumPosCm / s_corr : 0.f,
                 s_skipped);
        s_corr = s_skipped = 0; s_sumYawDeg = s_sumPosCm = 0.f; s_win = nw;
    }
    StereoReportEye(pr.rep);
}

// Content-probe experiment: rewrite every camera slot as the LAST written camera pitched by
// `pitchDeg` about its own right axis. Called between engine passes (same safe point as the
// second-eye write) to make pass 2's render visually distinguishable from pass 1's without
// any VR session - the flat harness then identifies which Present carries which pass.
bool GameERDebugPitchCamera(float pitchDeg) {
    if (!g_haveCamMtx.load(std::memory_order_relaxed)) return false;
    float m[16];
    { std::lock_guard<std::mutex> g(g_camMtxLock); memcpy(m, g_lastCamMtx, sizeof(m)); }
    Vec3 r{m[0], m[1], m[2]}, u{m[4], m[5], m[6]}, fw{m[8], m[9], m[10]}, pos{m[12], m[13], m[14]};
    const Quat pq = Quat::AxisAngle(r, Deg2Rad(pitchDeg));
    u = pq.Rotate(u); fw = pq.Rotate(fw);
    uint8_t* pcs[4]; const int npc = AllPersCams(pcs);
    if (npc == 0) return false;
    for (int i = 0; i < npc; ++i) WriteCamMatrix(pcs[i] + off::Cam_Matrix, r, u, fw, pos);
    if (uint8_t* cc = ChrCam()) WriteCamMatrix(cc + off::Cam_Matrix, r, u, fw, pos);
    if (uint8_t* fc = (uint8_t*)g_followCamPtr.load(std::memory_order_relaxed)) WriteCamMatrix(fc + off::Cam_Matrix, r, u, fw, pos);
    return true;
}

// CamWatch target: the first perspective camera - the chain TGA's freecam writes and the one
// whose matrix provably steers the render each frame, matrix through fov (0x10..0x54).
uint8_t* GameERCamWatchTarget(uint32_t* outLen) {
    uint8_t* pc = PersCam1();
    if (!pc) return nullptr;
    if (outLen) *outLen = (uint32_t)(off::Cam_Fov + 4 - off::Cam_Matrix);
    return pc + off::Cam_Matrix;
}

bool GameERInit() {
    g_base = (uint8_t*)GetModuleHandleW(nullptr);
    g_status.versionOk = VersionIs(L"2.6.2.0");
    posescan::Install();

    // Globals via classic AOBs.
    if (uint8_t* hit = AobScan("48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88")) g_worldChrManPtr = (uint8_t**)ResolveRipRelative(hit, 3, 7);
    if (uint8_t* hit = AobScan("48 8B 3D ?? ?? ?? ?? 49 8B D8 48 8B F2 4C 8B F1 48 85 FF")) g_fieldAreaPtr = (uint8_t**)ResolveRipRelative(hit, 3, 7);
    if (uint8_t* hit = AobScan("48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3")) g_gameDataManPtr = (uint8_t**)ResolveRipRelative(hit, 3, 7);
    // GameMan + CSEventFlagMan: the transition-authority pair (2026-08-23). GameMan holds
    // warp_requested (+0x10, fromsoftware-rs GameMan - the byte EVERY warp source sets on
    // its request frame); CSEventFlagMan's virtual-memory-flag block holds the community
    // "loading screen up" byte ([+0x28]+0x113, the one TGA's loading-screen tools poll).
    // AOBs are johndisandonato's practice-tool scans (aob_indirect_twice, rip at +3 len 7),
    // same shape as the WorldChrMan classic above.
    if (uint8_t* hit = AobScan("48 8B 1D ?? ?? ?? ?? 48 8B F8 48 85 DB 74 18 4C 8B 03")) g_gameManPtr = (uint8_t**)ResolveRipRelative(hit, 3, 7);
    if (uint8_t* hit = AobScan("48 8B 3D ?? ?? ?? ?? 48 85 FF 74 ?? 48 8B 49")) g_eventFlagManPtr = (uint8_t**)ResolveRipRelative(hit, 3, 7);
    LOGI("[game] WorldChrMan* @ %p, FieldArea* @ %p, GameMan* @ %p, CSEventFlagMan* @ %p",
         (void*)g_worldChrManPtr, (void*)g_fieldAreaPtr, (void*)g_gameManPtr, (void*)g_eventFlagManPtr);
    if (!g_gameManPtr) LOGW("[game] GameMan AOB missed - warp-request guard degraded to heuristics (consider Pass2SafeOnA)");
    if (!g_worldChrManPtr || !g_fieldAreaPtr) { LOGW("[game] core globals not found; first person disabled"); return false; }

    // Functions: RVA when the version matches, else erfps2's AOBs.
    uint8_t* camStep = nullptr; uint8_t* getDmy = nullptr;
    if (g_status.versionOk) { camStep = g_base + RVA_CameraStepUpdate; getDmy = g_base + RVA_GetDmyPos; }
    if (!camStep) camStep = ResolveCallTarget("48 8d 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 8d 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 8d 05 $", "CameraStepUpdate");
    if (!getDmy)  getDmy  = ResolveCallTarget("48 8d 44 24 40 48 89 44 24 28 41 8b 00 89 44 24 40 4c 8d 44 24 40 49 8b ca e8 $", "GetDmyPos");
    g_getDmyPos = (FnGetDmyPos)getDmy;
    LOGI("[game] CameraStepUpdate @ %p, GetDmyPos @ %p", (void*)camStep, (void*)getDmy);

    if (camStep) {
        if (MH_CreateHook(camStep, (void*)&Hook_CameraStep, (void**)&g_origCameraStep) == MH_OK && MH_EnableHook(camStep) == MH_OK) {
            g_status.cameraHooked = true; LOGI("[game] camera step hooked");
        } else LOGW("[game] failed to hook camera step");
    }
    // Camera wall recovery: a global bool the follow cam consults to push the camera out of geometry. The game
    // refreshes it every frame, so the hooks above clear it per frame while first person runs (erfps2 pattern:
    // "0F B6 43 0D 88 05 <rel32>" = the instruction pair that refreshes it).
    if (g_status.versionOk) g_camWallRecovery = g_base + RVA_CamWallRecovery;
    else if (uint8_t* hit = AobScan("0F B6 43 0D 88 05 ?? ?? ?? ?? 0F B6 43 10")) g_camWallRecovery = ResolveRipRelative(hit + 4, 2, 6);
    LOGI("[game] cam wall-recovery flag @ %p", (void*)g_camWallRecovery);

    uint8_t* mms = g_status.versionOk ? g_base + RVA_MmsUpdateChrCam : ResolveCallTarget("48 8b cb e8 $ f3 0f 10 4f 08 48 8b cb e8 ?? ?? ?? ?? f3 0f 10 4f 08 48 8b cb e8 ?? ?? ?? ?? 48 8b cb e8", "MmsUpdateChrCam");
    uint8_t* fol = g_status.versionOk ? g_base + RVA_FollowCamFollow : ResolveCallTarget("4d 8b c6 41 0f 28 cf 48 8b ce e8 $ 0f b6 86 10 03 00 00", "FollowCamFollow");
    if (mms && MH_CreateHook(mms, (void*)&Hook_MmsUpdateChrCam, (void**)&g_origMms) == MH_OK && MH_EnableHook(mms) == MH_OK) LOGI("[game] chr-cam update hooked @ %p", (void*)mms); else LOGW("[game] chr-cam update hook unavailable");
    if (fol && MH_CreateHook(fol, (void*)&Hook_FollowCamFollow, (void**)&g_origFollow) == MH_OK && MH_EnableHook(fol) == MH_OK) LOGI("[game] follow-cam hooked @ %p", (void*)fol); else LOGW("[game] follow-cam hook unavailable");
    uint8_t* ufol = g_status.versionOk ? g_base + RVA_UpdateFollowCam : nullptr;
    if (ufol && MH_CreateHook(ufol, (void*)&Hook_UpdateFollowCam, (void**)&g_origUpdFollow) == MH_OK && MH_EnableHook(ufol) == MH_OK) LOGI("[game] follow-cam outer update hooked @ %p", (void*)ufol); else LOGW("[game] follow-cam outer update hook unavailable");
    uint8_t* umod = g_status.versionOk ? g_base + RVA_UpdateChrModelPos : nullptr;
    if (umod && MH_CreateHook(umod, (void*)&Hook_UpdateChrModelPos, (void**)&g_origUpdModelPos) == MH_OK && MH_EnableHook(umod) == MH_OK) LOGI("[game] chr model-pos hooked @ %p (final pose write point)", (void*)umod); else LOGW("[game] chr model-pos hook unavailable");
    uint8_t* cfin = (g_status.versionOk && Cfg().fpCameraFinalWrite) ? g_base + RVA_CamFinalize : nullptr;
    if (cfin && MH_CreateHook(cfin, (void*)&Hook_CamFinalize, (void**)&g_origCamFinalize) == MH_OK && MH_EnableHook(cfin) == MH_OK) LOGI("[game] camera finalizer hooked @ %p (our matrix goes back after it)", (void*)cfin); else if (Cfg().fpCameraFinalWrite) LOGW("[game] camera finalizer hook unavailable");
    uint8_t* peval = (g_status.versionOk && Cfg().fpPoseAfterHavok) ? g_base + RVA_PoseEval : nullptr;
    if (peval && MH_CreateHook(peval, (void*)&Hook_PoseEval, (void**)&g_origPoseEval) == MH_OK && MH_EnableHook(peval) == MH_OK) LOGI("[game] havok pose eval hooked @ %p (post-animation pose reassert)", (void*)peval); else if (Cfg().fpPoseAfterHavok) LOGW("[game] havok pose eval hook unavailable");

    // Near-camera dither lookup: RVA on 2.6.2.0, else the function's own bytes (the hit IS the function).
    uint8_t* udit = g_status.versionOk ? g_base + RVA_UsesDithering
                  : AobScan("48 8B 81 60 10 00 00 48 8B 89 68 10 00 00 48 3B C1 74 0E 44 39 00 74 0C 48 83 C0 18 48 3B C1 75 F2 32 C0 C3");
    if (udit && MH_CreateHook(udit, (void*)&Hook_UsesDithering, (void**)&g_origUsesDithering) == MH_OK && MH_EnableHook(udit) == MH_OK)
        LOGI("[game] near-camera dither lookup hooked @ %p (NearDither=%.2f)", (void*)udit, Cfg().fpNearDither);
    else LOGW("[game] near-camera dither hook unavailable - NearDither has no effect");

    // Per-frame pose hook: CS::NoUpdateInterface vft[10] (ERSkeletonMan's timing point).
    if (void** vt = FindVTable("CS::NoUpdateInterface")) {
        void* fn = vt[10];
        void* stub = MidHookMakeStub(fn, &HkHandler, nullptr);
        if (stub) {
            DWORD old;
            if (VirtualProtect(&vt[10], sizeof(void*), PAGE_READWRITE, &old)) {
                g_hkOrig = fn; g_hkSlot = &vt[10];
                vt[10] = stub;
                VirtualProtect(&vt[10], sizeof(void*), old, &old);
                g_status.skeletonHooked = true;
            }
        }
        LOGI("[game] NoUpdateInterface[10] = %p, vtable hook %s", fn, g_status.skeletonHooked ? "installed" : "FAILED");
    }
    if (Cfg().combatEnabled) CombatInit(g_base, g_status.versionOk);
    return g_status.cameraHooked;
}

// [Cheats] SetAllStats: PlayerGameData (GameDataMan+0x8) stats at +0x3C..+0x58 (vigor..arcane), level +0x68
// (fromsoftware-rs PlayerGameData). Applied once per configured value, only raising stats; level = sum - 79.
void ApplyStatCheat() {
    static int applied = 0;
    int want = Cfg().cheatSetAllStats;
    if (want <= 0 || want == applied || !g_gameDataManPtr || !MainPlayer()) return;
    uint8_t* gdm = Deref(g_gameDataManPtr); if (!gdm) return;
    uint8_t* pgd = Deref(gdm + 0x8); if (!pgd) return;
    int stats[8]; if (!SafeRead(pgd + 0x3C, stats, sizeof(stats))) return;
    for (int& s : stats) if (s < 1 || s > 99) return;   // sanity: not a PlayerGameData we understand
    // Sets every stat to EXACTLY the value (2026-08-24: it used to only raise, which left a
    // test character at 50-everything with no way back for balance testing).
    int sum = 0, changed = 0;
    for (int& s : stats) { if (s != want) { s = want; ++changed; } sum += s; }
    if (changed) {
        SafeWrite(pgd + 0x3C, stats, sizeof(stats));
        int level = sum - 79; SafeWrite(pgd + 0x68, &level, 4);
        LOGI("[cheat] set %d stats to %d (level -> %d)", changed, want, level);
    }
    applied = want;
}

// In-game time in ms (GameDataMan+0xA0, the community IGT offset speedrun timers use). IGT
// advances by the frame dt each sim frame, which makes it the perfect probe for the pass-2
// dt swap: a full-dt second pass doubles its rate, a dt~0 one leaves it at 1.0x realtime.
uint32_t GameERIgtMs() {
    if (!g_gameDataManPtr) return 0;
    uint8_t* gdm = Deref(g_gameDataManPtr);
    if (!gdm) return 0;
    uint32_t igt = 0;
    if (!SafeRead(gdm + 0xA0, &igt, 4)) return 0;
    return igt;
}

bool GameERForeignCamActive() {
    const ULONGLONG t = g_foreignFovTick.load(std::memory_order_relaxed);
    // 1.2 s tail (2026-08-24, was 3 s): the tail's old job - bridging "map closed by the
    // travel confirm" into the teardown - belongs to GameMan.warp_requested now, which
    // fires on the confirm frame itself. The tail only needs to outlive the detector's
    // own sampling window while a takeover camera is still fading out.
    return t != 0 && GetTickCount64() - t < 1200;
}

// The game's OWN warp commitment: GameMan.warp_requested (+0x10). Set on the request frame
// of every warp - map-screen travel confirm, death respawn, stake of Marika, emevd
// WarpPlayer, coop return - and never by an ordinary interact/jump A press. This is the
// authoritative signal the Pass2SafeOnA heuristic approximated: the 2026-08-23 fast-travel
// crash fired 160 ms after the confirm with no guard holding; this byte is already set on
// that frame. Offset from fromsoftware-rs's GameMan (erfps2 branch, ER 1.16.2 - our exe).
bool GameERWarpRequested() {
    if (!g_gameManPtr) return false;
    uint8_t* gm = Deref(g_gameManPtr); if (!gm) return false;
    uint8_t b = 0;
    return RD(gm + 0x10, b) && b != 0;
}

// The community "loading screen up" byte: [[CSEventFlagMan]+0x28] + 0x113 (flag_blocks of
// the CSFD4VirtualMemoryFlag that heads CSEventFlagMan; the exact read TGA's Seamless-Coop
// loading-screen fix polls at 100 ms until it clears). TRUE for the whole teardown/stream/
// rebuild window between a warp request and arrival - state, not a timer.
bool GameERLoadingScreen() {
    if (!g_eventFlagManPtr) return false;
    uint8_t* efm = Deref(g_eventFlagManPtr); if (!efm) return false;
    uint8_t* blocks = Deref(efm + 0x28); if (!blocks) return false;
    uint8_t b = 0;
    return RD(blocks + 0x113, b) && b != 0;
}

// Latched warp guard for the frame pump: TRUE from the first frame warp_requested reads set
// until Pass2WarpGraceSeconds after it last read set. The tail bridges the gap between the
// flag being consumed by the transfer machinery and the player-null/loading-screen guards
// picking the hold up - without it a one-frame flag would let pass 2 back in mid-teardown.
// Also logs both authoritative signals' edges so any future transition crash can be timed
// against them from the log alone.
bool GameERWarpGuardActive() {
    const ULONGLONG now = GetTickCount64();
    static bool s_warpWas = false, s_loadWas = false;
    const bool warp = GameERWarpRequested();
    const bool load = GameERLoadingScreen();
    if (warp) g_warpReqTick.store(now, std::memory_order_relaxed);
    if (load) g_loadingTick.store(now, std::memory_order_relaxed);
    if (warp != s_warpWas) { LOGI("[guard] GameMan.warp_requested -> %d", (int)warp); s_warpWas = warp; }
    if (load != s_loadWas) { LOGI("[guard] loading-screen byte -> %d", (int)load); s_loadWas = load; }
    const ULONGLONG t = g_warpReqTick.load(std::memory_order_relaxed);
    return t != 0 && now - t < (ULONGLONG)(Cfg().vrPass2WarpGraceSeconds * 1000.f);
}

// "The world is being torn down or is about to be": player dead (death anim + death warp
// follow), warp latched, or the loading screen up. This is the write-side hazard window -
// while it holds, ERVR must not write into game objects or execute game code, because
// teardown frees and reuses the memory our chains and caches point at. Distinct from
// GameERTransitionActive below only in that it has NO arrival settle: writes into a built
// world (player active, load done) are legitimate the moment the world exists.
// Self-sampling (warp flag read here too) so it works in sessions without the frame pump.
bool GameERWorldDying() {
    if (GameERPlayerDead()) return true;
    if (GameERLoadingScreen()) return true;
    const ULONGLONG now = GetTickCount64();
    if (GameERWarpRequested()) g_warpReqTick.store(now, std::memory_order_relaxed);
    const ULONGLONG w = g_warpReqTick.load(std::memory_order_relaxed);
    return w != 0 && now - w < (ULONGLONG)(Cfg().vrPass2WarpGraceSeconds * 1000.f);
}

// The transition authority for every ERVR path that EXECUTES GAME CODE or mutates game
// state outside our own pass-2 pump - combat TAE injection above all. GameERWorldDying
// plus player-null and an arrival settle (the world keeps streaming in for seconds after
// the player appears; injecting behavior events into that is the suspected death-arrival
// crash class).
//
// Why this exists (2026-08-23 evening): both post-warp-guard crashes correlate with the
// physical-combat swing detector firing DURING a transition window - the arm motion of
// pressing A on the travel confirm reads as a ~4 m/s swing, so EVERY fast travel injected
// an attack-behavior event into the player 200-400 ms into world teardown (travel 1
// survived the race, travel 2 crashed 238 ms after the injection, CS::ChrIns + garbage
// index in the fault). Executing the game's behavior dispatch mid-teardown is a coin flip;
// the erfps2/practice-tool rule is that EVERYTHING gates on world validity, not just the
// renderer.
bool GameERTransitionActive() {
    const ULONGLONG now = GetTickCount64();
    static ULONGLONG s_lastInactive = now;
    if (!GameERPlayerActive()) { s_lastInactive = now; return true; }
    // Spawn settle: the world streams in around the player for seconds after arrival.
    if (now - s_lastInactive < (ULONGLONG)(Cfg().vrPass2SpawnGraceSeconds * 1000.f)) return true;
    return GameERWorldDying();
}

bool GameERTeleportGraceActive() {
    const ULONGLONG t = g_bigJumpTick.load(std::memory_order_relaxed);
    // Fixed 1 s tail since 2026-08-24: the >100 m jump was the ARRIVAL heuristic of the
    // pre-signal era, and the last speed pass showed it had become the slowest link in
    // the resume chain (the jump fires as the destination places the camera, so its tail
    // ran on after the loading byte cleared). Warp latch + loading byte + spawn grace now
    // cover arrival authoritatively; this stays only as a last-resort catch for a
    // world-moving jump that arrives with no warp flag and no load screen.
    return t != 0 && GetTickCount64() - t < 1000;
}

uint8_t* GameERIgtWatchTarget(uint32_t* outLen) {
    if (!g_gameDataManPtr) return nullptr;
    uint8_t* gdm = Deref(g_gameDataManPtr);
    if (!gdm) return nullptr;
    if (outLen) *outLen = 4;
    return gdm + 0xA0;
}

void GameERTick() {
    VRTracking tr; VRInputState in;
    bool ok = GetLatestVRState(tr, in);
    { std::lock_guard<std::mutex> g(g_vrMtx); g_vrValid = ok; if (ok) { g_vrTr = tr; g_vrIn = in; } }
    if (!MainPlayer() || !Cfg().fpEnabled) { g_moveActive.store(false); g_camYawInit = false; g_bodyYawInit = false; }
    ApplyStatCheat();
}

void GameERAddCameraYaw(float radians) {
    if (radians == 0.f || !g_camYawInit) return;
    if (g_moveLocked.load()) return;   // locked on: the game aims the view, our turn input would fight it
    g_camYaw.store(WrapPi(g_camYaw.load() + radians));
    float e = g_pendingYawPivot.load();
    while (!g_pendingYawPivot.compare_exchange_weak(e, e + radians)) {}
}

void GameERAddBodyYaw(float radians) {
    if (radians == 0.f || !g_bodyYawInit) return;
    if (g_moveLocked.load()) return;   // locked on: the game aims the body
    g_bodyStickYaw.store(WrapPi(g_bodyStickYaw.load() + radians));
}

MoveBasis GameERGetMoveBasis() {
    MoveBasis m;
    m.active = g_moveActive.load() && Cfg().fpEnabled && Cfg().fpCameraYawSource != "game";
    m.lockedOn = g_moveLocked.load();
    m.camYaw = g_camYaw.load();
    m.chrYaw = g_chrYaw.load();
    m.bodyYaw = g_bodyYaw.load();
    m.headYaw = g_headYaw.load();
    m.headValid = g_headYawValid.load();
    // With CameraRotation=hmd the matrix we write into ChrCam is YawQ(camYaw) * headQ (UpdateCamera), and the game
    // derives its stick frame from that matrix - so physically turning your head already turns "stick forward".
    m.viewYaw = (m.headValid && Cfg().fpCameraRotation == "hmd") ? WrapPi(m.camYaw + m.headYaw) : m.camYaw;
    return m;
}

GameERStatus GameERGetStatus() {
    GameERStatus s = g_status; s.boneCount = g_skel.boneCount;
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    s.hkCalls = InterlockedExchange(&g_hkCalls, 0); s.hkSolves = InterlockedExchange(&g_hkSolves, 0); s.camCalls = InterlockedExchange(&g_camCalls, 0);
    s.hkMs = (double)InterlockedExchange64(&g_hkTicks, 0) * 1000.0 / (double)f.QuadPart;
    s.camMs = (double)InterlockedExchange64(&g_camTicks, 0) * 1000.0 / (double)f.QuadPart;
    return s;
}

// Live check, NOT the g_status latch: playerFound is set once when the player is first
// resolved and never cleared, so keying the pass-2 grace off it left that guard permanently
// TRUE - i.e. dead - for the rest of the session. WorldChrMan->MainPlayer goes null while a
// warp (fast travel, death) tears the world down, and stays null until the destination map's
// player is built: exactly the window the inactive-grace exists to cover.
// TEST HARNESS ONLY: drop the player to 0 HP to fire the DEATH WARP. That warp is one of the
// two crash paths, and it is the only one reachable unattended - a fast travel needs map-screen
// navigation and would move the character to another site of grace. Dying costs the held runes
// (recoverable at the death spot) and respawns at the last grace; nothing else in the save moves.
//
// CSChrDataModule layout is anchored by the stamina pair combat.cpp already relies on
// (0x154/0x158); HP/MaxHP sit at 0x138/0x13C in the same block. Both are range-checked before
// the write, so a wrong offset logs and refuses instead of corrupting whatever it landed on.
// Same-frame death detector. HP reaches 0 on the REQUEST frame of a death warp - before
// the fade, before any teardown, before every other signal this file has. The 2026-08-22
// crash hunt showed the existing guards all fire too late (the real crash landed 200 ms
// after the warp was requested, with no guard holding); this one cannot be late, because
// the thing that starts the warp IS the value it reads. Chain and offsets are the ones
// GameERKillPlayer validated in-game (hp 1878/1878 next to the known stamina pair).
bool GameERPlayerDead() {
    uint8_t* player = MainPlayer(); if (!player) return false;
    uint8_t* mods = Deref(player + off::Chr_Modules); if (!mods) return false;
    uint8_t* data = Deref(mods + 0x00); if (!data) return false;   // CSChrDataModule
    int32_t hp = -1, maxHp = -1;
    if (!RD(data + 0x138, hp) || !RD(data + 0x13C, maxHp)) return false;
    return maxHp > 0 && maxHp < 100000 && hp <= 0;
}

bool GameERKillPlayer() {
    uint8_t* player = MainPlayer();
    if (!player) { LOGW("[kill] no player"); return false; }
    uint8_t* mods = Deref(player + off::Chr_Modules);
    if (!mods) { LOGW("[kill] no module container"); return false; }
    uint8_t* data = Deref(mods + 0x00);            // CSChrDataModule
    if (!data) { LOGW("[kill] no data module"); return false; }
    int32_t hp = -1, maxHp = -1, stam = -1, maxStam = -1;
    RD(data + 0x138, hp); RD(data + 0x13C, maxHp);
    RD(data + 0x154, stam); RD(data + 0x158, maxStam);
    // Sanity: the stamina pair is KNOWN good, so it validates that we are looking at the right
    // struct at all; the HP pair then has to look like HP.
    const bool stamOk = maxStam > 0 && maxStam < 100000 && stam >= 0 && stam <= maxStam;
    const bool hpOk = maxHp > 0 && maxHp < 100000 && hp > 0 && hp <= maxHp;
    LOGI("[kill] data module %p: hp %d/%d, stamina %d/%d", (void*)data, hp, maxHp, stam, maxStam);
    if (!stamOk || !hpOk) {
        LOGE("[kill] REFUSING to write - values do not look like HP/stamina (stamOk %d hpOk %d); "
             "the CSChrDataModule offsets are wrong for this build",
             (int)stamOk, (int)hpOk);
        return false;
    }
    const int32_t zero = 0;
    if (!WR(data + 0x138, zero)) { LOGE("[kill] HP write faulted"); return false; }
    LOGW("[kill] player HP %d -> 0; death warp incoming (test harness)", hp);
    return true;
}

// Exe version check result (2.6.2.0 pinned at startup). The crash-fix VEH gates its
// RVA-pinned instruction skips on this when Arxan's code-page scrambling makes byte
// verification unreliable inside the exception dispatch (measured 16:52: raw AND guarded
// reads of the faulting code page can disagree with the executing bytes).
bool GameERVersionOk() { return g_status.versionOk; }

bool GameERPlayerActive() { return g_status.playerFound && MainPlayer() != nullptr; }

void GameERShutdown() {
    if (g_hkSlot && g_hkOrig) {
        DWORD old;
        if (VirtualProtect(g_hkSlot, sizeof(void*), PAGE_READWRITE, &old)) { *g_hkSlot = g_hkOrig; VirtualProtect(g_hkSlot, sizeof(void*), old, &old); }
        g_hkSlot = nullptr;
    }
}
}
