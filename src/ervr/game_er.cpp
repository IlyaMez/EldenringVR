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
#include "log.h"
#include "vr_backend.h"
#include "midhook.h"
#include "rtti.h"
#include <windows.h>
#include <MinHook.h>
#pragma comment(lib, "version.lib")
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
    constexpr size_t GR_PersCam1      = 0x20;
}
// RVAs for eldenring.exe 2.6.2.0 (erfps2 src/rva/ww.rs), with AOB fallbacks below.
constexpr uint32_t RVA_CameraStepUpdate = 0x3bc5e0;
constexpr uint32_t RVA_GetDmyPos        = 0x3e94d0;
constexpr uint32_t RVA_MmsUpdateChrCam  = 0xb02390;   // per-frame ChrCam update (erfps2 MMS_UPDATE_CHR_CAM)
constexpr uint32_t RVA_FollowCamFollow  = 0x3b6e20;   // ChrExFollowCam::follow (erfps2 FOLLOW_CAM_FOLLOW)
constexpr size_t   OFF_FollowCam_ResetCamera = 0x315;

// ---------------------------------------------------------------- game types
struct HkQsTransform { float t[4]; float q[4]; float s[4]; };   // hkQsTransform (parent-relative)

using FnCameraStep = void (__fastcall*)(void* cscamera, void* fd4time);
using FnGetDmyPos  = float* (__fastcall*)(void* chrIns, float* outMtx4x4, const uint32_t* dmyId, int one);

// ---------------------------------------------------------------- state
uint8_t* g_base = nullptr;
uint8_t** g_worldChrManPtr = nullptr;    // address of the WorldChrMan* global
uint8_t** g_fieldAreaPtr = nullptr;      // address of the FieldArea* global
FnCameraStep g_origCameraStep = nullptr;
using FnMmsUpdate = void (__fastcall*)(void*);
using FnFollow = void (__fastcall*)(void* followCam, float dt, void* c);
FnMmsUpdate g_origMms = nullptr; FnFollow g_origFollow = nullptr;
FnGetDmyPos  g_getDmyPos = nullptr;
void* g_hkOrig = nullptr; void** g_hkSlot = nullptr;
GameERStatus g_status;
std::mutex g_vrMtx;
VRTracking g_vrTr; VRInputState g_vrIn; bool g_vrValid = false;
double g_lastDebug = 0;
uint32_t g_frame = 0;
// perf counters (reset by GameERGetStatus)
volatile LONG g_hkCalls = 0, g_hkSolves = 0, g_camCalls = 0;
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
Vec3 ToGame(const Vec3& v) { return {v.x, v.y, -v.z}; }
Quat ToGame(const Quat& q) { return {-q.x, -q.y, q.z, q.w}; }

// Camera-style rotation from yaw (about +Y, positive = turn right) and pitch (positive = look down).
void CamBasis(float yaw, float pitch, Vec3& right, Vec3& up, Vec3& fwd) {
    right = {std::cos(yaw), 0.f, -std::sin(yaw)};
    fwd   = {std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    up    = fwd.Cross(right);
}
void WriteCamMatrix(uint8_t* cam, const Vec3& r, const Vec3& u, const Vec3& f, const Vec3& p) {
    float m[16] = { r.x, r.y, r.z, 0.f, u.x, u.y, u.z, 0.f, f.x, f.y, f.z, 0.f, p.x, p.y, p.z, 1.f };
    SafeWrite(cam, m, sizeof(m));
}
Vec3 QuatRotate(const Quat& q, const Vec3& v) { return q.Rotate(v); }

// ---------------------------------------------------------------- game object access
uint8_t* WorldChrMan() { return g_worldChrManPtr ? Deref(g_worldChrManPtr) : nullptr; }
uint8_t* MainPlayer() { uint8_t* w = WorldChrMan(); return w ? Deref(w + off::WCM_MainPlayer) : nullptr; }
uint8_t* ChrCam() { uint8_t* w = WorldChrMan(); return w ? Deref(w + off::WCM_ChrCam) : nullptr; }
uint8_t* PersCam1() {
    uint8_t* fa = g_fieldAreaPtr ? Deref(g_fieldAreaPtr) : nullptr; if (!fa) return nullptr;
    uint8_t* gr = Deref(fa + off::FA_GameRend); if (!gr) return nullptr;
    return Deref(gr + off::GR_PersCam1);
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

// ---------------------------------------------------------------- head hiding
void SetPartVisible(uint8_t* asmIns, int idx, bool visible) {
    uint8_t* part = Deref(asmIns + off::Asm_Parts + idx * 8); if (!part) return;
    uint8_t* disp = Deref(part + off::MI_DispEntity); if (!disp) return;
    uint32_t f; if (!RD(disp + off::Disp_Flags1, f)) return;
    uint32_t nf = (f & ~1u) | (visible ? 1u : 0u);
    if (nf != f) WR(disp + off::Disp_Flags1, nf);
}
void ApplyHeadVisibility(uint8_t* player, bool hideHead, bool hideSheathed) {
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
    int torso = -1, L_clav = -1, R_clav = -1;
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
    s.torso = s.Find(c.boneTorso); s.L_clav = s.Find("L_Clavicle"); s.R_clav = s.Find("R_Clavicle");
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
    return true;
}

// World transforms for all bones from the live pose (parent-relative hkQsTransform) + character root.
struct WorldPose { std::vector<Vec3> pos; std::vector<Quat> q; };
bool ComputeWorld(const Skeleton& s, HkQsTransform* pose, const Vec3& rootPos, const Quat& rootQ, WorldPose& out, std::vector<HkQsTransform>& local) {
    out.pos.assign(s.boneCount, {}); out.q.assign(s.boneCount, {});
    local.resize(s.boneCount);
    if (!SafeRead(pose, local.data(), sizeof(HkQsTransform) * s.boneCount)) return false;
    for (int i = 0; i < s.boneCount; ++i) {
        const HkQsTransform& t = local[i];
        Vec3 lt{t.t[0], t.t[1], t.t[2]}; Quat lq{t.q[0], t.q[1], t.q[2], t.q[3]};
        int p = s.parents[i];
        if (p < 0 || p >= i) { out.q[i] = rootQ * lq; out.pos[i] = rootPos + rootQ.Rotate(lt); }
        else { out.q[i] = out.q[p] * lq; out.pos[i] = out.pos[p] + out.q[p].Rotate(lt); }
    }
    return true;
}

// ---------------------------------------------------------------- VR -> game frame
struct FrameInfo {
    bool valid = false;
    Vec3 anchor;         // eye anchor in game world (no HMD offset)
    float baseYaw = 0;   // game camera yaw the HMD is relative to
    float basePitch = 0; // game camera pitch (from chr cam)
    Quat  headQ;         // HMD orientation in game frame (relative to tracking origin)
    Vec3  headP;         // HMD position in game frame (relative to tracking origin)
    Vec3  handP[2]; Quat handQ[2]; bool handValid[2]{};
};

bool BuildFrame(uint8_t* player, FrameInfo& f) {
    const Config& c = Cfg();
    Vec3 root; Quat rootQ;
    if (!ModelMatrixPos(player, root)) { if (!PlayerRoot(player, root, rootQ)) return false; }
    f.anchor = root + Vec3{0.f, c.fpEyeHeight, 0.f};

    // Base yaw/pitch from the game's own character camera (stick / lock-on driven).
    uint8_t* cc = ChrCam();
    float fwd[4] = {0, 0, 1, 0};
    if (cc) SafeRead(cc + off::Cam_Matrix + 32, fwd, 16);
    f.baseYaw = std::atan2(fwd[0], fwd[2]);
    f.basePitch = std::asin(Clamp(-fwd[1], -1.f, 1.f));

    VRTracking tr; VRInputState in; bool vr;
    { std::lock_guard<std::mutex> g(g_vrMtx); tr = g_vrTr; in = g_vrIn; vr = g_vrValid; }
    if (vr && tr.head.valid) {
        Quat hq = ToGame(tr.head.rot); Vec3 hp = ToGame(tr.head.pos);
        if (c.fpHmdYawInGameCamera) { float y = -tr.head.rot.Yaw(); hq = Quat::YawQ(-y) * hq; hp = Quat::YawQ(-y).Rotate(hp); }
        f.headQ = hq; f.headP = hp;
        for (int h = 0; h < 2; ++h) {
            const Pose& hand = tr.hand[h];
            f.handValid[h] = hand.valid && in.controllerConnected[h];
            if (!f.handValid[h]) continue;
            Vec3 p = ToGame(hand.pos); Quat q = ToGame(hand.rot);
            if (c.fpHmdYawInGameCamera) { float y = -tr.head.rot.Yaw(); p = Quat::YawQ(-y).Rotate(p); q = Quat::YawQ(-y) * q; }
            if (c.handOrigin == "head") p = p - hp;
            else if (c.handOrigin == "floor") p = p - Vec3{0.f, c.fpEyeHeight, 0.f};
            f.handP[h] = p; f.handQ[h] = q;
        }
    }
    f.valid = true;
    return true;
}

// ---------------------------------------------------------------- camera
void UpdateCamera(uint8_t* player, const FrameInfo& f) {
    const Config& c = Cfg();
    float yaw = f.baseYaw, pitch = 0.f;
    Vec3 r, u, fw;
    if (c.fpCameraRotation == "game") pitch = f.basePitch;
    if (c.fpCameraRotation == "hmd") {
        // full HMD orientation composed onto the base yaw
        Quat q = Quat::YawQ(f.baseYaw) * f.headQ;
        fw = q.Rotate({0, 0, 1}); r = q.Rotate({1, 0, 0}); u = q.Rotate({0, 1, 0});
    } else CamBasis(yaw, pitch, r, u, fw);
    Vec3 pos = f.anchor;
    if (c.fpAddHmdPosition) pos = pos + Quat::YawQ(f.baseYaw).Rotate(f.headP);

    if (uint8_t* pc = PersCam1()) WriteCamMatrix(pc + off::Cam_Matrix, r, u, fw, pos);
    if (uint8_t* cc = ChrCam()) WriteCamMatrix(cc + off::Cam_Matrix, r, u, fw, pos);
    // Keep the body facing the camera direction (strafe/lock-on mode) unless a real enemy is locked on,
    // otherwise walking backwards/sideways turns the torso away and the arms cannot reach the hands.
    if (c.fpFaceCamera) {
        int64_t enemy = -1; RD(player + off::PI_LockedEnemy, enemy);
        bool realLock = enemy != -1 && enemy != 0;
        WR(player + off::Chr_IsLockedOn, (uint8_t)1);
        if (uint8_t* ctrl = Deref(player + off::Chr_ChrCtrl)) WR(ctrl + off::Ctrl_IsUnlocked, (uint8_t)0);
        if (!realLock) {
            Vec3 tgt = pos + fw * 10.f;
            float t4[4] = {tgt.x, tgt.y, tgt.z, 1.f};
            SafeWrite(player + off::Chr_LockOnTarget, t4, 16);
        }
    }
    if (c.fpWriteAim) {
        Quat q = Quat::YawQ(f.baseYaw) * f.headQ;
        Vec3 ar = q.Rotate({1, 0, 0}), au = q.Rotate({0, 1, 0}), af = q.Rotate({0, 0, 1});
        WriteCamMatrix(player + off::Chr_AimViewMtx, ar, au, af, pos);
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

void UpdateArms(uint8_t* player, const FrameInfo& f) {
    const Config& c = Cfg();
    if (g_skel.chr != player || !g_skel.hkb) { if (!BuildSkeleton(player)) return; }
    HkQsTransform* pose = g_skel.LivePose(); if (!pose) return;
    Vec3 rootPos; Quat rootQ; if (!PlayerRoot(player, rootPos, rootQ)) return;
    std::vector<HkQsTransform> local;
    WorldPose w; if (!ComputeWorld(g_skel, pose, rootPos, rootQ, w, local)) return;

    Quat baseQ = Quat::YawQ(f.baseYaw);
    // Torso twist: rotate the Spine bone about world Y so the shoulder line faces the camera yaw.
    if (c.torsoFaceCamera && g_skel.torso >= 0 && g_skel.L_clav >= 0 && g_skel.R_clav >= 0 && (f.handValid[0] || f.handValid[1])) {
        Vec3 right = w.pos[g_skel.R_clav] - w.pos[g_skel.L_clav]; right.y = 0.f;
        if (right.Length() > 1e-3f) {
            float bodyYaw = std::atan2(-right.z, right.x);           // right vector at yaw t = (cos t, 0, -sin t)
            float delta = WrapPi(f.baseYaw - bodyYaw);
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
    Quat calib = Quat::FromEulerYXZ(Deg2Rad(c.handRotOffY), Deg2Rad(c.handRotOffX), Deg2Rad(c.handRotOffZ));
    bool any = false;
    for (int h = 0; h < 2; ++h) {
        if (!f.handValid[h]) continue;
        Quat cq = (baseQ * f.handQ[h] * calib).Normalized();          // controller orientation in game world
        Vec3 cf = cq.Rotate({0, 0, 1}), cu = cq.Rotate({0, 1, 0});     // controller forward / up (game frame)
        Quat q = cq;
        if (g_skel.handAxesOk[h]) q = Quat::MapPairs(g_skel.handFwd[h], g_skel.handThumb[h], cf, cu);   // hand-local -> world
        Vec3 p = f.anchor + baseQ.Rotate(f.handP[h] * c.handScale) - cf * c.handBackOffset;
        float side = h == HandLeft ? -1.f : 1.f;
        Vec3 pole = baseQ.Rotate(Vec3{side * c.elbowPoleOut, -c.elbowPoleDown, -c.elbowPoleBack});
        if (h == HandLeft) SolveArm(g_skel, pose, local, w, g_skel.L_ua, g_skel.L_fa, g_skel.L_hand, p, q, pole);
        else               SolveArm(g_skel, pose, local, w, g_skel.R_ua, g_skel.R_fa, g_skel.R_hand, p, q, pole);
        any = true;
    }
    g_status.armsActive = any;
    if (c.fpDebug) {
        double now = NowSec();
        if (now - g_lastDebug > 1.0) {
            g_lastDebug = now;
            int hb = g_skel.Find("Head");
            Vec3 headW = hb >= 0 ? w.pos[hb] : Vec3{};
            float dmy[16]{}; Vec3 dmyP{};
            if (g_getDmyPos) { uint32_t id = 907; g_getDmyPos(player, dmy, &id, 1); dmyP = {dmy[12], dmy[13], dmy[14]}; }
            LOGD("[fp] root(%.2f,%.2f,%.2f) anchor(%.2f,%.2f,%.2f) headBone(%.2f,%.2f,%.2f) dmy907(%.2f,%.2f,%.2f) baseYaw %.1f | R hand tgt(%.2f,%.2f,%.2f) valid L%d R%d",
                 rootPos.x, rootPos.y, rootPos.z, f.anchor.x, f.anchor.y, f.anchor.z, headW.x, headW.y, headW.z, dmyP.x, dmyP.y, dmyP.z,
                 Rad2Deg(f.baseYaw), (f.anchor + baseQ.Rotate(f.handP[1])).x, (f.anchor + baseQ.Rotate(f.handP[1])).y, (f.anchor + baseQ.Rotate(f.handP[1])).z,
                 (int)f.handValid[0], (int)f.handValid[1]);
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
void CameraStepBody(bool& skipOriginal, bool afterOriginal) {
    const Config& c = Cfg();
    uint8_t* player = MainPlayer(); if (!player) return;
    FrameInfo f; if (!BuildFrame(player, f)) return;
    bool dump = c.fpDebug && (NowSec() - g_lastCamDump > 3.0);
    if (dump && !afterOriginal) { g_lastCamDump = NowSec(); DumpMatrix("game chr_cam (before write)", ChrCam()); DumpMatrix("game pers_cam_1 (before write)", PersCam1()); }
    ApplyHeadVisibility(player, c.fpHideHead, c.fpHideSheathed);
    if (c.fpCameraMode == "passthrough") { skipOriginal = false; return; }
    if (c.fpCameraMode == "post" && !afterOriginal) { skipOriginal = false; return; }   // come back after the original
    if (afterOriginal) { UpdateCamera(player, f); return; }                                // post pass: just re-assert
    UpdateCamera(player, f);
    if (dump) { DumpMatrix("ervr chr_cam (after write)", ChrCam()); DumpMatrix("ervr pers_cam_1 (after write)", PersCam1()); }
    skipOriginal = (c.fpCameraMode == "block");
    g_status.playerFound = true;
}
void __fastcall Hook_MmsUpdateChrCam(void* a) {
    if (!g_tripped && FirstPersonActive() && Cfg().fpCameraMode == "pre") {
        __try { bool d; CameraStepBody(d, false); } __except (SehFilter(GetExceptionCode())) { g_tripped = true; }
    }
    g_origMms(a);
}
void __fastcall Hook_FollowCamFollow(void* fc, float dt, void* c) {
    // Setting reset_camera disables the follow cam's position interpolation (erfps2 trick) so it stops fighting us.
    uint8_t saved = 0; bool have = false;
    if (!g_tripped && FirstPersonActive() && Cfg().fpCameraMode == "pre") { have = RD(((uint8_t*)fc) + OFF_FollowCam_ResetCamera, saved); if (have) WR(((uint8_t*)fc) + OFF_FollowCam_ResetCamera, (uint8_t)1); }
    g_origFollow(fc, dt, c);
    if (have) WR(((uint8_t*)fc) + OFF_FollowCam_ResetCamera, saved);
}
void HookCameraStepImpl(void* a, void* b);
void __fastcall Hook_CameraStep(void* a, void* b) {
    InterlockedIncrement(&g_camCalls);
    ScopedTicks st(&g_camTicks);
    HookCameraStepImpl(a, b);
}
void HookCameraStepImpl(void* a, void* b) {
    ++g_frame;
    bool skip = false;
    if (!g_tripped && FirstPersonActive()) {
        __try { CameraStepBody(skip, false); }
        __except (SehFilter(GetExceptionCode())) { g_tripped = true; skip = false; }
    }
    if (skip) return;
    g_origCameraStep(a, b);
    if (!g_tripped && FirstPersonActive() && (Cfg().fpCameraMode == "post" || Cfg().fpCameraMode == "pre")) {
        __try { bool dummy; CameraStepBody(dummy, true); }
        __except (SehFilter(GetExceptionCode())) { g_tripped = true; }
    }
}

void HkBody() {
    // Solve once per rendered frame (the Havok update fires many times per frame).
    if (g_status.cameraHooked && g_lastArmFrame == g_frame) return;
    g_lastArmFrame = g_frame;
    InterlockedIncrement(&g_hkSolves);
    ScopedTicks st(&g_hkTicks);
    uint8_t* player = MainPlayer(); if (!player) return;
    FrameInfo f; if (!BuildFrame(player, f)) return;
    UpdateArms(player, f);
}
void HkHandler(MidHookRegs*, void*) {
    InterlockedIncrement(&g_hkCalls);
    if (g_tripped || !Cfg().fpEnabled || !Cfg().handTracking) return;
    __try { HkBody(); }
    __except (SehFilter(GetExceptionCode())) { g_tripped = true; }
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

bool GameERInit() {
    g_base = (uint8_t*)GetModuleHandleW(nullptr);
    g_status.versionOk = VersionIs(L"2.6.2.0");

    // Globals via classic AOBs.
    if (uint8_t* hit = AobScan("48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88")) g_worldChrManPtr = (uint8_t**)ResolveRipRelative(hit, 3, 7);
    if (uint8_t* hit = AobScan("48 8B 3D ?? ?? ?? ?? 49 8B D8 48 8B F2 4C 8B F1 48 85 FF")) g_fieldAreaPtr = (uint8_t**)ResolveRipRelative(hit, 3, 7);
    LOGI("[game] WorldChrMan* @ %p, FieldArea* @ %p", (void*)g_worldChrManPtr, (void*)g_fieldAreaPtr);
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
    uint8_t* mms = g_status.versionOk ? g_base + RVA_MmsUpdateChrCam : ResolveCallTarget("48 8b cb e8 $ f3 0f 10 4f 08 48 8b cb e8 ?? ?? ?? ?? f3 0f 10 4f 08 48 8b cb e8 ?? ?? ?? ?? 48 8b cb e8", "MmsUpdateChrCam");
    uint8_t* fol = g_status.versionOk ? g_base + RVA_FollowCamFollow : ResolveCallTarget("4d 8b c6 41 0f 28 cf 48 8b ce e8 $ 0f b6 86 10 03 00 00", "FollowCamFollow");
    if (mms && MH_CreateHook(mms, (void*)&Hook_MmsUpdateChrCam, (void**)&g_origMms) == MH_OK && MH_EnableHook(mms) == MH_OK) LOGI("[game] chr-cam update hooked @ %p", (void*)mms); else LOGW("[game] chr-cam update hook unavailable");
    if (fol && MH_CreateHook(fol, (void*)&Hook_FollowCamFollow, (void**)&g_origFollow) == MH_OK && MH_EnableHook(fol) == MH_OK) LOGI("[game] follow-cam hooked @ %p", (void*)fol); else LOGW("[game] follow-cam hook unavailable");

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
    return g_status.cameraHooked;
}

void GameERTick() {
    VRTracking tr; VRInputState in;
    bool ok = GetLatestVRState(tr, in);
    std::lock_guard<std::mutex> g(g_vrMtx);
    g_vrValid = ok; if (ok) { g_vrTr = tr; g_vrIn = in; }
}

GameERStatus GameERGetStatus() {
    GameERStatus s = g_status; s.boneCount = g_skel.boneCount;
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    s.hkCalls = InterlockedExchange(&g_hkCalls, 0); s.hkSolves = InterlockedExchange(&g_hkSolves, 0); s.camCalls = InterlockedExchange(&g_camCalls, 0);
    s.hkMs = (double)InterlockedExchange64(&g_hkTicks, 0) * 1000.0 / (double)f.QuadPart;
    s.camMs = (double)InterlockedExchange64(&g_camTicks, 0) * 1000.0 / (double)f.QuadPart;
    return s;
}

void GameERShutdown() {
    if (g_hkSlot && g_hkOrig) {
        DWORD old;
        if (VirtualProtect(g_hkSlot, sizeof(void*), PAGE_READWRITE, &old)) { *g_hkSlot = g_hkOrig; VirtualProtect(g_hkSlot, sizeof(void*), old, &old); }
        g_hkSlot = nullptr;
    }
}
}
