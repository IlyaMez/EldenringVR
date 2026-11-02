#include "body_haptics_er.h"
#include "bhaptics_output.h"
#include "config.h"
#include "game_er.h"
#include "game_memory.h"
#include "game_version.h"
#include "log.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace ervr {
namespace {
// ChrIns (fromsoftware-rs chr_ins.rs; the layout combat.cpp and game_er.cpp already read).
constexpr size_t Chr_Handle    = 0x08;    // FieldInsHandle {u32 selector, i32 block}: the character itself
constexpr size_t Chr_LastHitBy = 0x180;   // FieldInsHandle of whoever hit it last (selector ~0 = none)
constexpr size_t Chr_Modules   = 0x190;
constexpr size_t Mod_Data      = 0x00;    // CSChrDataModule: hp 0x138, max 0x13C
constexpr size_t Mod_Physics   = 0x68;    // CSChrPhysicsModule
constexpr size_t Data_Hp       = 0x138;
constexpr size_t Data_MaxHp    = 0x13C;
constexpr size_t Phys_Orientation = 0x50; // quat xyzw, forward local -Z
constexpr size_t Phys_Position = 0x70;    // xyzw
// is_falling, then is_touching_ground at +0x1D1 (fromsoftware-rs chr_ins/module/physics.rs: the field
// after gravity_multiplier, named unk1d2 next). Not yet seen in-game: a value other than 0 / 1 means the
// layout differs on this build, and falls go unfelt (logged once).
constexpr size_t Phys_IsFalling = 0x1D0;
// WorldChrMan's by-distance list of the characters near the player (combat.cpp's enemy watcher).
constexpr size_t WCM_ChrByDistFirst = 0x1F1D8;
constexpr size_t WCM_ChrByDistLast  = 0x1F1E0;
constexpr size_t ChrDist_Stride = 0x10;   // {ChrIns* @0, f32 distance @8, pad}

// Never deleted: a static destructor at process exit would wait on a thread the exit already ended.
// Published once Start() has returned, so the other threads only ever see a started one.
std::atomic<BhapticsOutput*> g_out{nullptr};
std::atomic<float> g_rumble[2] = {0.f, 0.f};
// Where the torso faces in the world (game yaw, + right), from the pad worker for the game thread.
std::atomic<float> g_torsoYaw{0.f};
std::atomic<bool> g_torsoValid{false};
// What the game thread saw last, for the pad worker's state.
struct GameSnapshot {
    double at = -1;
    float health = 1.f, chrYaw = 0.f;
    bool airborne = false;
    Vec3 velocity{};   // world, m/s
};
std::mutex g_snapMtx;
GameSnapshot g_snap;
bool g_tripped = false;

double NowSec() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart;
}
uint8_t* Deref(const void* addr) { uint8_t* p = nullptr; return SafeRead(addr, &p, sizeof(p)) ? p : nullptr; }
uint8_t* Module(uint8_t* chr, size_t slot) { uint8_t* m = Deref(chr + Chr_Modules); return m ? Deref(m + slot) : nullptr; }
bool ChrPosition(uint8_t* chr, Vec3& pos) {
    uint8_t* ph = Module(chr, Mod_Physics); float p[4];
    if (!ph || !SafeRead(ph + Phys_Position, p, 16) || !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
    pos = {p[0], p[1], p[2]};
    return true;
}
const char* HandText(int hand) { return hand == 1 ? "right" : hand == 0 ? "left" : "both"; }

void Post(const BodyEvent& e) {
    BhapticsOutput* out = g_out.load();
    if (!out) return;
    out->Post(e);
    if (e.kind == BodyKind::Hit)
        LOGD("[bhaptics] hit %.2f %s", e.strength, e.placed ? "" : "(from nowhere known: chest and back)");
    else LOGD("[bhaptics] %s %s %.2f", BodyKindName(e.kind), HandText(e.hand), e.strength);
}

// The character that hit the player last, as the game recorded it (ChrIns +0x180), if it is still
// among those near: where it stands from the player, round the torso and up the body.
bool AttackerPlace(uint8_t* player, const Vec3& at, float torsoYaw, float& azimuth, float& height, int& npc) {
    uint64_t by = 0, self = 0;
    if (!SafeRead(player + Chr_LastHitBy, &by, 8) || (uint32_t)by == UINT32_MAX) return false;
    if (SafeRead(player + Chr_Handle, &self, 8) && by == self) return false;
    uint8_t* wcm = GameERWorldChrMan();
    uint8_t *first = nullptr, *last = nullptr;
    if (!wcm || !SafeRead(wcm + GameWorldChrManOffset(WCM_ChrByDistFirst), &first, 8) ||
        !SafeRead(wcm + GameWorldChrManOffset(WCM_ChrByDistLast), &last, 8) || !first || last < first) return false;
    const size_t count = (size_t)(last - first) / ChrDist_Stride;
    if (count > 512) return false;   // torn read / not a vector
    for (size_t i = 0; i < count; ++i) {
        uint8_t* chr = nullptr; uint64_t h = 0;
        if (!SafeRead(first + i * ChrDist_Stride, &chr, 8) || !chr || chr == player) continue;
        if (!SafeRead(chr + Chr_Handle, &h, 8) || h != by) continue;
        Vec3 p;
        if (!ChrPosition(chr, p)) return false;
        const float dx = p.x - at.x, dz = p.z - at.z, flat = std::sqrt(dx * dx + dz * dz);
        // A blow from further than this came along a bullet or from an attacker long gone: the side it
        // stands on says nothing.
        if (flat > 40.f || flat < 0.05f) return false;
        azimuth = WrapPi(std::atan2(dx, dz) - torsoYaw);
        height = BodyHitHeight(p.y - at.y, flat);
        npc = 0; SafeRead(chr + 0x64, &npc, 4);   // Chr_NpcId, for the log
        return true;
    }
    return false;
}

void GameFrameBody(uint8_t* player) {
    const double now = NowSec();
    static uint8_t* s_player = nullptr;
    static int s_hp = -1, s_maxHp = -1;
    static bool s_havePos = false, s_fallOk = true;
    static Vec3 s_pos{}, s_vel{};
    static double s_t = 0, s_landedAt = -1e9;
    static BodyLandingWatch s_landing;
    if (player != s_player) {   // another world (a warp builds a new player): what came before is no measure
        s_player = player; s_hp = s_maxHp = -1; s_havePos = false; s_vel = {}; s_landing.Reset();
    }
    uint8_t* data = Module(player, Mod_Data);
    int hp = -1, maxHp = -1;
    const bool hpOk = data && SafeRead(data + Data_Hp, &hp, 4) && SafeRead(data + Data_MaxHp, &maxHp, 4) &&
                      maxHp > 0 && maxHp < 100000 && hp >= 0 && hp <= maxHp * 2;
    Vec3 pos{};
    const bool posOk = ChrPosition(player, pos);
    float chrYaw = 0.f;
    {
        uint8_t* ph = Module(player, Mod_Physics); float q[4];
        if (ph && SafeRead(ph + Phys_Orientation, q, 16)) { const Vec3 f = Quat{q[0], q[1], q[2], q[3]}.Rotate({0, 0, -1}); chrYaw = std::atan2(f.x, f.z); }
    }
    const float torsoYaw = g_torsoValid.load() ? g_torsoYaw.load() : chrYaw;
    // Death first: HP reaches 0 on the frame the death warp is requested, which ends everything below.
    if (hpOk && s_hp > 0 && hp <= 0 && maxHp == s_maxHp) {
        float az = 0, height = .4f; int npc = 0;
        const bool placed = posOk && AttackerPlace(player, pos, torsoYaw, az, height, npc);
        Post({BodyKind::Hit, -1, 1.f, az, height, placed});
        Post({BodyKind::Death});
    }
    if (GameERWorldDying()) {   // dead, warping or loading: no falls, no heals (a respawn fills the HP)
        s_havePos = false; s_vel = {}; s_landing.Reset();
        if (hpOk) { s_hp = hp; s_maxHp = maxHp; }
        std::lock_guard<std::mutex> g(g_snapMtx);
        g_snap = {now, hpOk ? (float)hp / (float)maxHp : 1.f, chrYaw, false, {}};
        return;
    }
    // ---- falls: the game's own is_falling, the speed from the position ---------------------------
    bool airborne = false;
    if (posOk) {
        const float dt = (float)std::clamp(now - s_t, 0.001, 0.25);
        if (s_havePos) {
            const Vec3 d = pos - s_pos;
            if (d.Length() > 25.f) { s_vel = {}; s_landing.Reset(); }   // a teleport, not a speed
            else s_vel = s_vel + (d * (1.f / dt) - s_vel) * std::min(1.f, dt / .05f);
        }
        s_pos = pos; s_havePos = true;
        uint8_t* ph = Module(player, Mod_Physics); uint8_t falling = 0;
        if (s_fallOk && ph && SafeRead(ph + Phys_IsFalling, &falling, 1)) {
            if (falling > 1) { s_fallOk = false; LOGW("[bhaptics] physics is_falling (+0x%zX) reads %u, not a bool: this game build lays the module out differently - falls and landings go unfelt", Phys_IsFalling, falling); }
            else airborne = falling == 1;
        }
        if (const float landed = s_landing.Update(dt, airborne, std::max(0.f, -s_vel.y)); landed > 0.f) {
            s_landedAt = now;
            Post({BodyKind::Landing, -1, landed});
        }
    }
    s_t = now;
    // ---- HP: blows taken, heals ---------------------------------------------------------------------
    if (hpOk && s_hp > 0 && hp > 0 && maxHp == s_maxHp) {   // a changed max (Vigor, a talisman) moves HP too
        const float share = (float)(s_hp - hp) / (float)maxHp;
        if (share > .005f) {
            float az = 0, height = .4f; int npc = 0;
            const bool placed = posOk && AttackerPlace(player, pos, torsoYaw, az, height, npc);
            // Unplaced: fall damage (the landing already said it), or a poison / rot tick - one of those a
            // second would be a thump in the chest every second.
            if (placed || (now - s_landedAt > .35 && share >= .02f)) {
                Post({BodyKind::Hit, -1, std::min(1.f, share * 2.5f), az, height, placed});
                if (placed) LOGD("[bhaptics] ... -%d HP (%.0f%%) from npc %d, %.0f deg %s, height %.2f", s_hp - hp, share * 100, npc, std::fabs(az) * 57.2958f, az >= 0 ? "right" : "left", height);
            }
        } else if (-share >= .04f) {
            Post({BodyKind::Heal, -1, std::min(1.f, -share * 2.f)});
        }
    }
    if (hpOk) { s_hp = hp; s_maxHp = maxHp; }
    std::lock_guard<std::mutex> g(g_snapMtx);
    g_snap = {now, hpOk ? (float)hp / (float)maxHp : 1.f, chrYaw, airborne, s_vel};
}
int GameFrameFilter(unsigned code) { LOGE("[bhaptics] exception 0x%08X on the game thread; body haptics stop reading the game", code); return EXCEPTION_EXECUTE_HANDLER; }

void LogStatus(const BhapticsCounters& k) {
    switch (k.status) {
    case BhapticsStatus::NoLibrary: LOGW("[bhaptics] bhaptics_library.dll is missing beside ERVR.dll (or is not the bHaptics SDK's): no body haptics"); break;
    case BhapticsStatus::NoApp: LOGI("[bhaptics] this ERVR build has no bHaptics app (built without ERVR_BHAPTICS_APP_ID / _API_KEY): no body haptics"); break;
    case BhapticsStatus::NoPlayer: LOGI("[bhaptics] the bHaptics Player is not installed: no body haptics"); break;
    case BhapticsStatus::Waiting: LOGI("[bhaptics] waiting for the bHaptics Player (start it to feel the game; it is found by itself)"); break;
    case BhapticsStatus::Connected: LOGI("[bhaptics] connected to the bHaptics Player, no device on: pair them in the Player"); break;
    case BhapticsStatus::Playing: LOGI("[bhaptics] playing on %s", BhapticsDeviceNames(k.devices).c_str()); break;
    case BhapticsStatus::Failed: LOGW("[bhaptics] the bHaptics SDK would not start: no body haptics"); break;
    case BhapticsStatus::Rejected: LOGW("[bhaptics] the bHaptics Player did not take ERVR's app (no event list within 5 s): wrong workspace ID / API key in this build, or the app has no event deployed"); break;
    default: break;
    }
}
} // namespace

void BodyHapticsTick(double now) {
    const Config& c = Cfg();
    const int level = std::clamp(c.bhapticsLevel, 0, 100);
    BhapticsOutput* out = g_out.load();
    if (!out) {
        if (level <= 0) return;
        BhapticsConfig bc;
        bc.library = BhapticsLibraryBeside();
        bc.appId = BhapticsBuiltAppId();
        bc.apiKey = BhapticsBuiltApiKey();
        LOGI("[bhaptics] body haptics on at %d%%: %S, app %s", level, bc.library.c_str(), bc.appId.empty() ? "(none in this build)" : bc.appId.c_str());
        out = new BhapticsOutput;
        out->Start(bc);
        g_out.store(out);
    }
    static int s_level = -1;
    if (level != s_level) { s_level = level; out->Level(level); }

    // Where the torso faces: it turns with the head past a look over the shoulder (BodyFacing), in the
    // room; the room turns in the world with ERVR's own yaw (stick turns). Without ERVR's yaw (CameraYawSource
    // =game, first person off) the character's facing stands in.
    static BodyFacing s_facing;
    static double s_last = 0;
    const float dt = s_last > 0 ? (float)std::clamp(now - s_last, 0.0, 0.25) : 0.f;
    s_last = now;
    const MoveBasis mb = GameERGetMoveBasis();
    if (mb.active && mb.headValid) {
        g_torsoYaw.store(WrapPi(mb.camYaw + s_facing.Update(mb.headYaw, dt)));
        g_torsoValid.store(true);
    } else { s_facing.Reset(); g_torsoValid.store(false); }

    BodyState s;
    s.live = GameERPlayerActive() && !GameERLoadingScreen();
    GameSnapshot snap;
    { std::lock_guard<std::mutex> g(g_snapMtx); snap = g_snap; }
    if (s.live && snap.at > 0 && NowSec() - snap.at < 0.5) {
        s.health = snap.health;
        s.airborne = snap.airborne;
        // The velocity in the torso's frame: x right, y up, z ahead (game yaw = atan2(x, z), + right).
        const float yaw = g_torsoValid.load() ? g_torsoYaw.load() : snap.chrYaw;
        const float sy = std::sin(yaw), cy = std::cos(yaw);
        s.velocity = {snap.velocity.x * cy - snap.velocity.z * sy, snap.velocity.y, snap.velocity.x * sy + snap.velocity.z * cy};
    }
    s.rumble = {g_rumble[0].load(), g_rumble[1].load()};
    s.groups = (c.bhapticsCombat ? 1u << (unsigned)BodyGroup::Combat : 0u) | (c.bhapticsDamage ? 1u << (unsigned)BodyGroup::Damage : 0u) |
               (c.bhapticsMovement ? 1u << (unsigned)BodyGroup::Movement : 0u) | (c.bhapticsRumble ? 1u << (unsigned)BodyGroup::Rumble : 0u);
    out->Set(s);

    // What the Player and its devices do, logged as it changes; the devices the first time each is on
    // say so themselves with a hello.
    static double s_lastStatus = 0;
    if (now - s_lastStatus < 0.25) return;
    s_lastStatus = now;
    const BhapticsCounters k = out->Counters();
    static BhapticsStatus s_status = BhapticsStatus::Off;
    static uint32_t s_devices = 0, s_greeted = 0;
    if (k.status != s_status || (k.status == BhapticsStatus::Playing && k.devices != s_devices)) LogStatus(k);
    s_status = k.status; s_devices = k.devices;
    if (k.status == BhapticsStatus::Playing && (k.devices & ~s_greeted)) {
        s_greeted |= k.devices;
        out->Post({BodyKind::Hello});
    }
}

void BodyHapticsHeartbeat() {
    BhapticsOutput* out = g_out.load();
    if (!out) return;
    const BhapticsCounters k = out->Counters();
    static uint64_t s_events = 0, s_frames = 0, s_refused = 0;
    LOGD("hb: bhaptics: %s, devices %s, level %u%% | 5s: events %llu, device frames %llu, refused %llu",
         BhapticsStatusName(k.status), BhapticsDeviceNames(k.devices).c_str(), k.level,
         (unsigned long long)(k.events - s_events), (unsigned long long)(k.frames - s_frames), (unsigned long long)(k.refused - s_refused));
    s_events = k.events; s_frames = k.frames; s_refused = k.refused;
}

void BodyHapticsGameFrame(uint8_t* player, uint32_t frame) {
    static uint32_t s_frame = 0xFFFFFFFF;
    if (g_tripped || !player || !g_out.load() || frame == s_frame) return;
    s_frame = frame;
    __try { GameFrameBody(player); } __except (GameFrameFilter(GetExceptionCode())) { g_tripped = true; }
}

void BodyHapticsPost(BodyKind kind, int hand, float strength) {
    BodyEvent e;
    e.kind = kind; e.hand = (int8_t)hand; e.strength = strength;
    Post(e);
}

void BodyHapticsRumble(float left, float right) {
    g_rumble[0].store(std::clamp(left, 0.f, 1.f));
    g_rumble[1].store(std::clamp(right, 0.f, 1.f));
}

}
