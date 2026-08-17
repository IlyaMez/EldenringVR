// Physical melee combat via the game's own TAE AttackBehavior event.
//
// Sources: fromsoftware-rs (CSChrTaeAnimEvent / CSTaeAnimEventArgs layouts, event ids,
// ChrIns module container + CSChrDataModule stamina offsets), erfps2 (hooking
// CSChrTaeAnimEvent vtable RVA 0x2a34280 for 2.6.2.0), and our own disassembly of that
// vtable: slot 1 = run slot 2 then slot 3; slot 2 handles the "generic" events and is a NO-OP
// for events 1/2/5; slot 3 dispatches the behavior events, event 1 -> handler at RVA 0x4266d0.
// That handler reads args+0x1a (u16, must be >= 0x1a for the Source byte to be honoured),
// args+0x20 (float* animation time, dereferenced on first execution) and passes the
// AttackBehaviorArgs pointer into the damage module (so it must stay stable per attack).
// DSAnimStudio TAE.Template.ER.xml (AttackBehavior args: AttackType s32, AttackIndex s32,
// BehaviorJudgeID s32, DirectionType u8, Source u8 (0 default/1 right/2 left), StateInfo s16),
// Muffin's DS3 moveset notes (BehaviorParam_PC row = 1_0000_0000 + variation*1000 + judgeId).
#include "combat.h"
#include "config.h"
#include "game_memory.h"
#include "log.h"
#include "rtti.h"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <mutex>
#include <cmath>
#include <cstring>

namespace ervr {
namespace {

// ---------------------------------------------------------------- game layouts
constexpr uint32_t RVA_ChrTaeAnimEventVft = 0x2a34280;   // CS::CSChrTaeAnimEvent vtable (2.6.2.0)
constexpr size_t   Chr_Modules       = 0x190;
constexpr size_t   Mod_Data          = 0x00;   // CSChrDataModule
constexpr size_t   Mod_TimeAct       = 0x18;   // CSChrTimeActModule
constexpr size_t   Data_Stamina      = 0x154;
constexpr size_t   Data_MaxStamina   = 0x158;
constexpr size_t   TA_ChrTaeAnimEvent = 0x18; // CSChrTimeActModule -> CSChrTaeAnimEvent*
constexpr uint32_t EV_AttackBehavior = 1;

struct CSChrTaeAnimEvent { void** vft; uint64_t unk08; uint32_t animId; uint32_t pad; uint8_t* owner; };
struct CSTaeAnimEventArgs {
    uint32_t eventId; uint32_t pad04;
    void* args;
    float from, to;
    uint8_t isFirstExecution;
    uint8_t pad19;
    uint16_t taeVersion;      // +0x1a: handler honours Source only if >= 0x1a
    uint32_t pad1c;
    const float* animTime;    // +0x20: current animation time (read on first execution)
    uint8_t unk28[0x18];
};
static_assert(sizeof(CSTaeAnimEventArgs) == 0x40, "CSTaeAnimEventArgs layout");
struct AttackBehaviorArgs { int32_t attackType; int32_t attackIndex; int32_t judgeId; uint8_t dirType; uint8_t source; int16_t stateInfo; };
static_assert(sizeof(AttackBehaviorArgs) == 16, "AttackBehaviorArgs layout");

using FnExecEvent = void (__fastcall*)(CSChrTaeAnimEvent* self, CSTaeAnimEventArgs* args);
FnExecEvent g_origExec = nullptr;      // slot 3 (behavior events) trampoline: used for injection
constexpr int kExecSlot = 3;
void** g_taeVft = nullptr;

// ---------------------------------------------------------------- state
enum Slot { SlotLight = 0, SlotHeavy = 1, SlotLeft = 2 };
struct Template { bool valid = false; AttackBehaviorArgs a{}; };
std::mutex g_mtx;                 // guards templates + swing commands (worker <-> game thread)
Template g_tpl[3];
double g_lastBtn[3] = {-1e9, -1e9, -1e9};
uint8_t* g_player = nullptr;      // last main player seen by the game thread (for owner match in the hook)

struct SwingCmd {                 // worker -> game thread
    bool active = false;
    bool first = false;           // deliver is_first_execution on the next frame
    int  slot = SlotLight;
    double start = 0;
    uint32_t id = 0;
    bool staminaFail = false;     // game thread -> worker: rejected for lack of stamina
};
SwingCmd g_cmd[2];                // per hand

struct HandSM {
    enum { Idle, Armed, Swinging, Cooldown } st = Idle;
    double t = 0;                 // state entry time
    float peak = 0;
    float prevTip = 0;
    uint32_t nextId = 1;
} g_sm[2];

CombatStatus g_status;
volatile LONG g_injected = 0, g_swings = 0;
std::atomic<uint32_t> g_haptic{0};
uint32_t g_lastFrame = 0xFFFFFFFF;
double g_lastEvLog = 0;
bool g_tripped = false;

double NowSec() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart;
}
uint8_t* Deref(const void* addr) { uint8_t* p = nullptr; return SafeRead(addr, &p, sizeof(p)) ? p : nullptr; }
const char* SlotName(int s) { return s == SlotLight ? "light" : s == SlotHeavy ? "heavy" : "left"; }

int ReadStamina(uint8_t* player, int* maxOut = nullptr) {
    uint8_t* mods = Deref(player + Chr_Modules); if (!mods) return -1;
    uint8_t* data = Deref(mods + Mod_Data); if (!data) return -1;
    int s = -1, m = -1; SafeRead(data + Data_Stamina, &s, 4); SafeRead(data + Data_MaxStamina, &m, 4);
    if (maxOut) *maxOut = m;
    return s;
}
CSChrTaeAnimEvent* PlayerTaeEvent(uint8_t* player) {
    uint8_t* mods = Deref(player + Chr_Modules); if (!mods) return nullptr;
    uint8_t* ta = Deref(mods + Mod_TimeAct); if (!ta) return nullptr;
    auto* ev = (CSChrTaeAnimEvent*)Deref(ta + TA_ChrTaeAnimEvent); if (!ev) return nullptr;
    void** vft = nullptr; if (!SafeRead(&ev->vft, &vft, sizeof(vft))) return nullptr;
    if (g_taeVft && vft != g_taeVft) return nullptr;   // not the object we expect: refuse to call into it
    return ev;
}

// Templates from config unless learned ones exist. Source is always forced to the swinging hand:
// the game's "default" source follows the hand HKS last attacked with, which goes stale after
// weapon swaps / left-hand actions and then resolves the wrong weapon's BehaviorParam row.
void ApplyConfigTemplates() {
    const Config& c = Cfg();
    static int lastCfg[3] = {-2, -2, -2};   // ini hot-reload with new ids overrides learned templates
    int cur[3] = {c.lightJudgeId, c.heavyJudgeId, c.leftJudgeId};
    for (int i = 0; i < 3; ++i) if (cur[i] != lastCfg[i]) { if (lastCfg[i] != -2) g_tpl[i].valid = false; lastCfg[i] = cur[i]; }
    auto set = [&](int slot, int judge, uint8_t source) {
        if (judge < 0 || g_tpl[slot].valid) return;
        Template t; t.valid = true; t.a = {0, 0, judge, (uint8_t)0xFF, source, 0};
        g_tpl[slot] = t;
        LOGI("[combat] %s attack from config: judgeId %d source %u", SlotName(slot), judge, source);
    };
    set(SlotLight, c.lightJudgeId, 1); set(SlotHeavy, c.heavyJudgeId, 1); set(SlotLeft, c.leftJudgeId, 2);
}
void SaveLearned(int slot, int judgeId) {
    const Config& c = Cfg();
    if (!c.combatSaveLearned || c.iniPath.empty()) return;
    const wchar_t* key = slot == SlotLight ? L"LightJudgeId" : slot == SlotHeavy ? L"HeavyJudgeId" : L"LeftJudgeId";
    wchar_t v[16]; swprintf_s(v, L"%d", judgeId);
    if (WritePrivateProfileStringW(L"Combat", key, v, c.iniPath.c_str())) LOGI("[combat] saved [Combat] %S=%d to ERVR.ini", key, judgeId);
}
bool PhysicalActiveLocked() {
    const Config& c = Cfg();
    if (!c.fpEnabled || !c.combatEnabled) return false;
    if (c.combatMode == "buttons") return false;
    if (c.combatMode == "physical") return true;
    return g_tpl[SlotLight].valid;   // auto
}

// ---------------------------------------------------------------- TAE hook (game thread)
void __fastcall Hook_ExecEvent(CSChrTaeAnimEvent* self, CSTaeAnimEventArgs* args) {
    uint32_t id = 0; uint8_t* owner = nullptr; void* a = nullptr; uint8_t first = 0;
    bool isPlayer = false;
    if (!g_tripped && SafeRead(&args->eventId, &id, 4) && id == EV_AttackBehavior && SafeRead(&self->owner, &owner, sizeof(owner)) && owner && owner == g_player) {
        isPlayer = true;
        SafeRead(&args->args, &a, sizeof(a)); SafeRead(&args->isFirstExecution, &first, 1);
        AttackBehaviorArgs ab{};
        if (a && SafeRead(a, &ab, sizeof(ab))) {
            const Config& c = Cfg();
            double now = NowSec();
            if (c.combatDebug && first && now - g_lastEvLog > 0.05) {
                g_lastEvLog = now;
                LOGD("[combat] game AttackBehavior: type %d index %d judgeId %d dir %u source %u state %d anim %u", ab.attackType, ab.attackIndex, ab.judgeId, ab.dirType, ab.source, ab.stateInfo, self->animId);
            }
            // Learn: first execution of the first hit (attackIndex 0) shortly after we pulsed an attack button.
            if (c.combatLearn && first && ab.attackIndex == 0) {
                int learnedSlot = -1, learnedJudge = 0;
                {
                    std::lock_guard<std::mutex> g(g_mtx);
                    int best = -1; double bt = -1e9;
                    for (int s = 0; s < 3; ++s) if (now - g_lastBtn[s] < 1.0 && g_lastBtn[s] > bt) { bt = g_lastBtn[s]; best = s; }
                    if (best >= 0) {
                        AttackBehaviorArgs t = ab; t.source = best == SlotLeft ? 2 : 1; t.attackIndex = 0;
                        bool changed = !g_tpl[best].valid || memcmp(&g_tpl[best].a, &t, sizeof(t)) != 0;
                        g_tpl[best].valid = true; g_tpl[best].a = t; g_lastBtn[best] = -1e9;
                        if (changed) { learnedSlot = best; learnedJudge = ab.judgeId; }
                    }
                }
                if (learnedSlot >= 0) {
                    LOGI("[combat] learned %s attack: judgeId %d (type %d dir %u state %d, anim %u) -> [Combat] %sJudgeId=%d",
                         SlotName(learnedSlot), learnedJudge, ab.attackType, ab.dirType, ab.stateInfo, self->animId,
                         learnedSlot == SlotLight ? "Light" : learnedSlot == SlotHeavy ? "Heavy" : "Left", learnedJudge);
                    SaveLearned(learnedSlot, learnedJudge);
                }
            }
            if (c.combatSuppressAnimAttacks) { std::lock_guard<std::mutex> g(g_mtx); if (PhysicalActiveLocked()) return; }
        }
    }
    (void)isPlayer;
    g_origExec(self, args);
}

int SehFilter(unsigned code) { LOGE("[combat] exception 0x%08X; physical combat disabled", code); return EXCEPTION_EXECUTE_HANDLER; }

void GameFrameBody(uint8_t* player) {
    g_player = player;
    const Config& c = Cfg();
    SwingCmd cmd[2]; bool physical;
    { std::lock_guard<std::mutex> g(g_mtx); ApplyConfigTemplates(); physical = PhysicalActiveLocked(); cmd[0] = g_cmd[0]; cmd[1] = g_cmd[1]; }
    g_status.physical = physical;
    int stam = ReadStamina(player); g_status.stamina = stam;
    if (!physical) return;
    CSChrTaeAnimEvent* ev = PlayerTaeEvent(player);
    if (!ev) { static double lastWarn = 0; double n = NowSec(); if (n - lastWarn > 10) { lastWarn = n; LOGW("[combat] player CSChrTaeAnimEvent not found (vtable mismatch?)"); } return; }

    for (int h = HandRight; h >= HandLeft; --h) {   // right first (main hand)
        SwingCmd& k = cmd[h];
        if (!k.active) continue;
        Template t;
        { std::lock_guard<std::mutex> g(g_mtx); t = g_tpl[k.slot]; if (!t.valid && k.slot == SlotHeavy) t = g_tpl[SlotLight]; if (!t.valid && k.slot == SlotLeft) { t = g_tpl[SlotLight]; t.a.source = 2; } }
        if (!t.valid) continue;
        if (k.first) {
            if (c.combatRequireStamina && stam >= 0 && stam < c.combatMinStamina) {
                std::lock_guard<std::mutex> g(g_mtx);
                if (g_cmd[h].id == k.id) { g_cmd[h].active = false; g_cmd[h].staminaFail = true; }
                g_haptic.fetch_or(4u << h);
                if (c.combatDebug) LOGD("[combat] %s swing rejected: stamina %d", h == HandRight ? "right" : "left", stam);
                continue;
            }
            { std::lock_guard<std::mutex> g(g_mtx); if (g_cmd[h].id == k.id) g_cmd[h].first = false; }
            InterlockedIncrement(&g_swings);
        }
        // Stable per-hand storage: the game keys the attack instance on the args pointer across frames.
        static AttackBehaviorArgs s_ab[2]; static CSTaeAnimEventArgs s_args[2]; static float s_time[2]; static bool s_logNext[2];
        AttackBehaviorArgs& ab = s_ab[h]; CSTaeAnimEventArgs& args = s_args[h];
        if (k.first) {
            ab = t.a;
            ab.source = h == HandLeft ? 2 : 1;                       // always the swinging hand's weapon
            if (c.combatAttackIndexRotate > 0) ab.attackIndex = (int32_t)(k.id % (uint32_t)c.combatAttackIndexRotate);
            memset(&args, 0, sizeof(args));
            args.eventId = EV_AttackBehavior; args.args = &ab; args.from = 0.f; args.to = c.swingMaxSeconds > 0 ? c.swingMaxSeconds : 1.f;
            args.taeVersion = 0x1a; s_time[h] = 0.f; args.animTime = &s_time[h]; s_logNext[h] = c.combatDebug;
            if (c.combatExtraStamina[k.slot] > 0 && stam > 0) {
                int ns = stam - c.combatExtraStamina[k.slot]; if (ns < 0) ns = 0;
                uint8_t* mods = Deref(player + Chr_Modules); uint8_t* data = mods ? Deref(mods + Mod_Data) : nullptr;
                if (data) SafeWrite(data + Data_Stamina, &ns, 4);
            }
        } else s_time[h] = (float)(NowSec() - k.start);
        args.isFirstExecution = k.first ? 1 : 0;
        g_origExec(ev, &args);
        InterlockedIncrement(&g_injected);
        if (c.combatDebug && k.first) LOGD("[combat] injected %s attack (judgeId %d index %d source %u) stamina %d", SlotName(k.slot), ab.judgeId, ab.attackIndex, ab.source, stam);
        if (!k.first && s_logNext[h]) { s_logNext[h] = false; LOGD("[combat] ... next frame: stamina %d", ReadStamina(player)); }
    }
}

// ---------------------------------------------------------------- swing detection (worker thread)
float TipSpeed(const Pose& p, float reach, float* handSpeed) {
    Vec3 fwd = p.rot.Rotate({0.f, 0.f, -1.f});
    Vec3 tip = p.linVel + p.angVel.Cross(fwd * reach);
    if (handSpeed) *handSpeed = p.linVel.Length();
    return tip.Length();
}

void StartSwing(int h, int slot, double now) {
    std::lock_guard<std::mutex> g(g_mtx);
    SwingCmd& k = g_cmd[h];
    k.active = true; k.first = true; k.slot = slot; k.start = now; k.id = g_sm[h].nextId++; k.staminaFail = false;
}
void EndSwing(int h) { std::lock_guard<std::mutex> g(g_mtx); g_cmd[h].active = false; g_cmd[h].first = false; }

} // namespace

// ---------------------------------------------------------------- public API
bool CombatInit(uint8_t* base, bool versionOk) {
    void** vft = nullptr;
    if (versionOk) vft = (void**)(base + RVA_ChrTaeAnimEventVft);
    void** rttiVft = FindVTable("CS::CSChrTaeAnimEvent");
    if (vft && rttiVft && vft != rttiVft) { LOGW("[combat] RVA vtable %p != RTTI vtable %p; using RTTI", (void*)vft, (void*)rttiVft); vft = rttiVft; }
    if (!vft) vft = rttiVft;
    if (!vft) { LOGW("[combat] CS::CSChrTaeAnimEvent vtable not found; physical combat unavailable"); return false; }
    g_taeVft = vft;
    void* target = nullptr; SafeRead(&vft[kExecSlot], &target, sizeof(target));
    if (!target) return false;
    if (MH_CreateHook(target, (void*)&Hook_ExecEvent, (void**)&g_origExec) == MH_OK && MH_EnableHook(target) == MH_OK) {
        g_status.taeHooked = true;
        LOGI("[combat] TAE behavior-event hook installed (CSChrTaeAnimEvent vft[%d] @ %p)", kExecSlot, target);
    } else LOGW("[combat] failed to hook TAE event exec @ %p", target);
    { std::lock_guard<std::mutex> g(g_mtx); ApplyConfigTemplates(); }
    return g_status.taeHooked;
}

void CombatShutdown() {}

bool CombatHandlesSwings() { std::lock_guard<std::mutex> g(g_mtx); return g_status.taeHooked && PhysicalActiveLocked(); }

void CombatNotifyButton(uint16_t mask, double now) {
    const Config& c = Cfg();
    std::lock_guard<std::mutex> g(g_mtx);
    if (mask == ParseXInputButton(c.lightAttackButton)) g_lastBtn[SlotLight] = now;
    if (mask == ParseXInputButton(c.heavyAttackButton)) g_lastBtn[SlotHeavy] = now;
    if (mask == ParseXInputButton(c.leftSwingButton))   g_lastBtn[SlotLeft] = now;
}

uint32_t CombatTakeHapticRequests() { return g_haptic.exchange(0); }

uint16_t CombatBlockedButtons() {
    const Config& c = Cfg();
    if (!c.combatBlockAttackButtons || !CombatHandlesSwings()) return 0;
    uint16_t m = ParseXInputButton(c.lightAttackButton) | ParseXInputButton(c.heavyAttackButton);
    return m;
}

void CombatPollHands(const VRTracking& tr, const VRInputState& in, double now) {
    const Config& c = Cfg();
    if (!CombatHandlesSwings()) { for (int h = 0; h < 2; ++h) { if (g_sm[h].st != HandSM::Idle) EndSwing(h); g_sm[h].st = HandSM::Idle; } return; }
    for (int h = 0; h < 2; ++h) {
        HandSM& s = g_sm[h];
        const Pose& p = tr.hand[h];
        bool grip = in.buttons & (h == HandLeft ? VRB_LGrip : VRB_RGrip);
        bool allowed = p.valid && in.controllerConnected[h] && (h == HandRight || c.combatLeftHand) && (!c.gestureRequiresGrip || grip);
        float handSpd = 0.f;
        float tip = allowed ? TipSpeed(p, h == HandRight ? c.weaponReachRight : c.weaponReachLeft, &handSpd) : 0.f;
        bool fail = false;
        { std::lock_guard<std::mutex> g(g_mtx); if (g_cmd[h].staminaFail) { g_cmd[h].staminaFail = false; fail = true; } }
        if (fail && s.st == HandSM::Swinging) { s.st = HandSM::Cooldown; s.t = now; }
        switch (s.st) {
        case HandSM::Idle:
            if (allowed && tip >= c.swingOnSpeed && handSpd >= c.minHandSpeed && s.prevTip < c.swingOnSpeed) { s.st = HandSM::Armed; s.t = now; s.peak = tip; }
            break;
        case HandSM::Armed: {
            if (tip > s.peak) s.peak = tip;
            bool decel = tip < s.prevTip && tip < s.peak * 0.85f;
            if (!allowed || tip < c.swingOffSpeed) { s.st = HandSM::Idle; break; }
            if (now - s.t >= c.swingArmWindow || decel) {
                int slot = h == HandLeft ? SlotLeft : (s.peak >= c.heavySwingSpeed ? SlotHeavy : SlotLight);
                StartSwing(h, slot, now); s.st = HandSM::Swinging; s.t = now;
                g_haptic.fetch_or(1u << h);
                if (c.combatDebug) LOGD("[combat] %s swing start: %s (tip %.2f m/s, hand %.2f m/s)", h == HandRight ? "right" : "left", SlotName(slot), s.peak, handSpd);
            }
            break; }
        case HandSM::Swinging:
            if (tip > s.peak) s.peak = tip;
            if (!allowed || tip < c.swingOffSpeed || now - s.t >= c.swingMaxSeconds) {
                if (c.combatDebug) LOGD("[combat] %s swing end after %.0f ms (peak %.2f m/s)", h == HandRight ? "right" : "left", (now - s.t) * 1000.0, s.peak);
                EndSwing(h); s.st = HandSM::Cooldown; s.t = now;
            }
            break;
        case HandSM::Cooldown:
            if (now - s.t >= c.swingCooldown && tip < c.swingOnSpeed) s.st = HandSM::Idle;
            break;
        }
        s.prevTip = tip;
    }
}

void CombatGameFrame(uint8_t* player, uint32_t frame) {
    if (g_tripped || !g_status.taeHooked || !player) return;
    if (frame == g_lastFrame) return;
    g_lastFrame = frame;
    __try { GameFrameBody(player); } __except (SehFilter(GetExceptionCode())) { g_tripped = true; }
}

CombatStatus CombatGetStatus() {
    CombatStatus s = g_status;
    { std::lock_guard<std::mutex> g(g_mtx); s.learnedLight = g_tpl[SlotLight].valid; s.learnedHeavy = g_tpl[SlotHeavy].valid; s.learnedLeft = g_tpl[SlotLeft].valid; }
    s.injected = InterlockedExchange(&g_injected, 0); s.swings = InterlockedExchange(&g_swings, 0);
    return s;
}
}
