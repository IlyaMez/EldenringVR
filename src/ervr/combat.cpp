// Physical melee combat via the game's own TAE AttackBehavior event.
//
// Sources: fromsoftware-rs (CSChrTaeAnimEvent / CSTaeAnimEventArgs layouts, event ids,
// ChrIns module container + CSChrDataModule stamina offsets, PlayerIns/ChrAsm equipment
// slots, EQUIP_PARAM_WEAPON_ST / BEHAVIOR_PARAM_ST / ATK_PARAM_ST field layouts), erfps2
// (hooking CSChrTaeAnimEvent vtable RVA 0x2a34280 for 2.6.2.0), and our own disassembly of
// that vtable: slot 1 = run slot 2 then slot 3; slot 2 handles the "generic" events and is a
// NO-OP for events 1/2/5; slot 3 dispatches the behavior events, event 1 -> handler at RVA
// 0x4266d0. That handler reads args+0x1a (u16, must be >= 0x1a for the Source byte to be
// honoured), args+0x20 (float* animation time, dereferenced on first execution) and passes
// the AttackBehaviorArgs pointer into the damage module (so it must stay stable per attack).
// DSAnimStudio TAE.Template.ER.xml (AttackBehavior args: AttackType s32, AttackIndex s32,
// BehaviorJudgeID s32, DirectionType u8, Source u8 (0 default/1 right/2 left), StateInfo s16),
// Muffin's DS3 moveset notes (BehaviorParam_PC row = 1_0000_0000 + variation*1000 + judgeId).
//
// Kinetic layer ("damage is physics"): a strike's quality - tip speed at impact relative to a
// reference speed, squared like kinetic energy, times how much arc the tip travelled - scales
// the motion values of the AtkParam_Pc row the strike resolves to (BehaviorParam_PC row ->
// refId), live in the loaded params, and is restored when the strike ends. Stamina is fatigue:
// each strike drains by energy spent (scaled by the weapon's weight), and low stamina weakens
// strikes. Shields: the left hand's pose against the head yields a block quality that holds
// the guard button and scales the shield's guard cut rates / stability the same way; a hit
// taken while the shield moves INTO the attack is a deflect (stamina refunded).
#include "combat.h"
#include "config.h"
#include "game_er.h"
#include "game_memory.h"
#include "params_er.h"
#include "log.h"
#include "rtti.h"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ervr {
namespace {

// ---------------------------------------------------------------- game layouts
constexpr uint32_t RVA_ChrTaeAnimEventVft = 0x2a34280;   // CS::CSChrTaeAnimEvent vtable (2.6.2.0)
constexpr size_t Chr_Modules       = 0x190;
constexpr size_t Mod_Data          = 0x00;   // CSChrDataModule
constexpr size_t Mod_TimeAct       = 0x18;   // CSChrTimeActModule
constexpr size_t Data_Stamina      = 0x154;
constexpr size_t Data_MaxStamina   = 0x158;
constexpr size_t TA_ChrTaeAnimEvent = 0x18; // CSChrTimeActModule -> CSChrTaeAnimEvent*
constexpr uint32_t EV_AttackBehavior = 1;

// PlayerIns -> ChrAsm (fromsoftware-rs PlayerIns @0x638; ChrAsm: arm_style @8, selected slots
// left/right @0xC/0x10, equipment_param_ids[22] @0x7C in ChrAsmEquipEntries order:
// 0 primary-left, 1 primary-right, 2 secondary-left, 3 secondary-right, 4/5 tertiary).
constexpr size_t Player_ChrAsm   = 0x638;
constexpr size_t Asm_ArmStyle    = 0x08;   // 0 empty, 1 one-handed, 2 left both hands, 3 right both hands
constexpr size_t Asm_LeftSlot    = 0x0C;
constexpr size_t Asm_RightSlot   = 0x10;
constexpr size_t Asm_ParamIds    = 0x7C;
// EQUIP_PARAM_WEAPON_ST
constexpr size_t   Wep_VariationId   = 0x04;   // i32 behaviorVariationId
constexpr size_t   Wep_Weight        = 0x10;   // f32
constexpr size_t   Wep_GuardCut      = 0x34;   // f32 x4: phys, mag, fire, thun (% of damage blocked)
constexpr size_t   Wep_StaminaGuardDef = 0xD8; // i16 stability
constexpr size_t   Wep_DarkGuardCut  = 0x188;  // f32
constexpr size_t   Wep_Type          = 0x1A6;  // u16 wepType (65 small / 67 medium / 69 great shield)
constexpr uint32_t Wep_RowSize       = 0x298;
// BEHAVIOR_PARAM_ST
constexpr size_t   Bhv_VariationId = 0x00, Bhv_JudgeId = 0x04, Bhv_RefType = 0x09, Bhv_RefId = 0x0C, Bhv_Stamina = 0x14;
constexpr uint32_t Bhv_RowSize     = 0x20;
// ATK_PARAM_ST (player rows: the *_correction u16s are the motion values, in percent)
constexpr size_t   Atk_PhysCorr = 0x3E, Atk_MagCorr = 0x40, Atk_FireCorr = 0x42, Atk_ThunCorr = 0x44, Atk_StamCorr = 0x46;
constexpr size_t   Atk_DarkCorr = 0x186;
constexpr size_t   Atk_SuperArmorCorr = 0x1B0;   // f32 poise-damage motion value
constexpr uint32_t Atk_RowSize  = 0x1C8;
constexpr uint32_t BehaviorPcBase = 100000000u;

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
std::mutex g_mtx;                 // guards templates + swing commands + block state (worker <-> game thread)
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
    float quality = 1.f;          // kinetic: live strike quality (1 = vanilla)
    float qualityCommit = 1.f;    // quality when the strike committed (fatigue drain)
    float arc = 0.f, tipSpeed = 0.f;
    float tipLive = 0.f;          // the weapon tip's speed right now (continuous model), no peak floor
};
SwingCmd g_cmd[2];                // per hand

struct HandSM {
    enum { Idle, Armed, Swinging, Cooldown } st = Idle;
    double t = 0;                 // state entry time
    float peak = 0;
    float prevTip = 0;
    uint32_t nextId = 1;
    // kinetic tracking
    Vec3 lastTip; bool tipValid = false;
    float arc = 0;                // m of tip travel since the tip last came to rest
    float vSmooth = 0;
    // Inertia: the "virtual weapon" tip speed, which can only change as fast as the weapon's
    // weight allows. A wrist flick never gets an ultra greatsword up to speed; a committed
    // swing does. Damage and light/heavy follow this, hit detection follows the real tip.
    float vVirt = 0, vVirtPeak = 0;
    double lastPoll = 0;
} g_sm[2];

// Shield / block (worker writes under g_mtx, game thread reads)
struct BlockState {
    bool raised = false;
    float quality = 0.f;
    float into = 0.f;             // shield speed along the look direction, recent max (deflect)
    double intoT = 0;
} g_block;
std::atomic<int> g_leftShield{0};    // game thread -> worker: 1 while a shield is in use in the left hand

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
const char* HandName(int h) { return h == HandRight ? "right" : "left"; }
float Lerp(float a, float b, float t) { return a + (b - a) * t; }

int ReadStamina(uint8_t* player, int* maxOut = nullptr) {
    uint8_t* mods = Deref(player + Chr_Modules); if (!mods) return -1;
    uint8_t* data = Deref(mods + Mod_Data); if (!data) return -1;
    int s = -1, m = -1; SafeRead(data + Data_Stamina, &s, 4); SafeRead(data + Data_MaxStamina, &m, 4);
    if (maxOut) *maxOut = m;
    return s;
}
bool WriteStamina(uint8_t* player, int value) {
    uint8_t* mods = Deref(player + Chr_Modules); uint8_t* data = mods ? Deref(mods + Mod_Data) : nullptr;
    if (!data) return false;
    if (value < 0) value = 0;
    return SafeWrite(data + Data_Stamina, &value, 4);
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

// ---------------------------------------------------------------- kinetic: equipment + params (game thread)
struct HandWeapon {
    bool valid = false;
    int  paramId = 0;         // equipped item's EquipParamWeapon id (upgrade level included)
    int  variation = 0;       // behaviorVariationId
    float weight = 0.f;
    uint16_t wepType = 0;
    uint8_t* row = nullptr;   // EQUIP_PARAM_WEAPON_ST base row
    bool isShield = false;
};
int g_armStyle = 0;
HandWeapon g_wep[2];
std::atomic<float> g_wepWeight[2]{0.f, 0.f};   // game thread -> worker: equipped weight per hand (inertia)
bool g_layoutBad = false;      // a param row size disagreed with the layout we compiled against: never write
bool g_loggedLayout = false;

bool IsShieldType(uint16_t t) {
    const std::string& s = Cfg().shieldWepTypes;
    size_t p = 0;
    while (p < s.size()) {
        size_t q = s.find(',', p); if (q == std::string::npos) q = s.size();
        if (q > p && (uint16_t)atoi(s.substr(p, q - p).c_str()) == t) return true;
        p = q + 1;
    }
    return false;
}

bool CheckRowSize(const char* what, uint32_t got, uint32_t expected) {
    if (got == 0 || got == expected) return true;
    if (!g_loggedLayout) { g_loggedLayout = true; LOGW("[combat] %s row size 0x%X != expected 0x%X: kinetic param edits DISABLED (layout changed)", what, got, expected); }
    g_layoutBad = true;
    return false;
}

void ReadEquipment(uint8_t* player) {
    uint8_t* asmp = Deref(player + Player_ChrAsm);
    if (!asmp) { g_wep[0].valid = g_wep[1].valid = false; g_wepWeight[0].store(0.f); g_wepWeight[1].store(0.f); return; }
    int style = 0; uint32_t slotL = 0, slotR = 0;
    SafeRead(asmp + Asm_ArmStyle, &style, 4); SafeRead(asmp + Asm_LeftSlot, &slotL, 4); SafeRead(asmp + Asm_RightSlot, &slotR, 4);
    if (slotL > 2) slotL = 0; if (slotR > 2) slotR = 0;
    g_armStyle = style;
    const uint32_t idx[2] = {slotL * 2 + 0, slotR * 2 + 1};
    for (int h = 0; h < 2; ++h) {
        int id = 0; SafeRead(asmp + Asm_ParamIds + idx[h] * 4, &id, 4);
        HandWeapon& w = g_wep[h];
        if (id <= 0 || id >= 100000000) { w.valid = false; continue; }
        if (w.valid && w.paramId == id) continue;   // unchanged: keep the resolved row
        w = HandWeapon{}; w.paramId = id;
        if (!ParamsReady()) continue;
        uint32_t sz = 0;
        uint8_t* row = ParamsRow(PI_EquipParamWeapon, (uint32_t)(id / 100) * 100u, &sz);
        if (!row) { LOGW("[combat] %s weapon %d: no EquipParamWeapon row", HandName(h), id); w.valid = true; continue; }
        CheckRowSize("EQUIP_PARAM_WEAPON_ST", sz, Wep_RowSize);
        SafeRead(row + Wep_VariationId, &w.variation, 4); SafeRead(row + Wep_Weight, &w.weight, 4); SafeRead(row + Wep_Type, &w.wepType, 2);
        w.row = row; w.isShield = IsShieldType(w.wepType); w.valid = true;
        LOGI("[combat] %s hand: weapon %d variation %d weight %.1f wepType %u%s (player level %d)", HandName(h), id, w.variation, w.weight, w.wepType, w.isShield ? " (shield)" : "", GameERPlayerLevel());
    }
    // Carried weight per hand. Two-handing puts the one weapon in both hands (each arm carries
    // it, so each lags with it); the free hand of a two-hander carries nothing extra.
    float wl = g_wep[HandLeft].valid ? g_wep[HandLeft].weight : 0.f, wr = g_wep[HandRight].valid ? g_wep[HandRight].weight : 0.f;
    if (style == 3) wl = wr; else if (style == 2) wr = wl;
    g_wepWeight[HandLeft].store(wl); g_wepWeight[HandRight].store(wr);
}

// Distinct behaviorVariationIds that actually have BehaviorParam_PC rows (built once, ~14k rows).
// Unique-moveset weapons carry a variation of their own in EquipParamWeapon (Sword of Night and
// Flame = 214) that has NO rows of its own for the basic attacks - the game falls back to the
// class base. We do the same: exact, then the tens, then the hundreds, then the nearest lower.
const std::vector<int>& BehaviorVariations() {
    static std::vector<int> v; static bool built = false;
    if (built) return v;
    const uint32_t n = ParamsRowCount(PI_BehaviorParam_PC);
    if (n == 0) return v;
    v.reserve(256);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t id = 0; uint8_t* row = ParamsRowAt(PI_BehaviorParam_PC, i, &id); int var = -1;
        if (row && SafeRead(row + Bhv_VariationId, &var, 4) && var >= 0) v.push_back(var);
    }
    std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end());
    built = true;
    LOGI("[combat] BehaviorParam_PC: %u rows, %zu distinct variations", n, v.size());
    return v;
}
// Candidate variations for a weapon, best first. A unique-moveset weapon's own variation (804)
// often has rows for its weapon art ONLY, so the fallback has to be tried per judge id, not
// just when the variation is absent altogether.
int VariationCandidates(int var, int out[4]) {
    const std::vector<int>& v = BehaviorVariations();
    auto has = [&](int x) { return std::binary_search(v.begin(), v.end(), x); };
    int n = 0;
    auto add = [&](int x) { if (x < 0) return; for (int i = 0; i < n; ++i) if (out[i] == x) return; if (n < 4) out[n++] = x; };
    add(var);
    if (v.empty()) return n;
    const int c10 = var - (var % 10), c100 = var - (var % 100);
    if (has(c10)) add(c10);
    if (has(c100)) add(c100);
    auto it = std::lower_bound(v.begin(), v.end(), var);
    if (it != v.begin()) add(*(it - 1));
    return n;
}
// The BehaviorParam_PC row for (variation, judge): id formula verified by reading the fields back,
// then a field scan.
uint8_t* FindBehaviorRow(int variation, int judge, uint32_t* idOut) {
    uint32_t bhvId = BehaviorPcBase + (uint32_t)variation * 1000u + (uint32_t)judge;
    uint8_t* b = ParamsRow(PI_BehaviorParam_PC, bhvId);
    int var = -1, jd = -1;
    if (b) { SafeRead(b + Bhv_VariationId, &var, 4); SafeRead(b + Bhv_JudgeId, &jd, 4); if (var != variation || jd != judge) b = nullptr; }
    if (!b) {
        const uint32_t n = ParamsRowCount(PI_BehaviorParam_PC);
        for (uint32_t i = 0; i < n && !b; ++i) {
            uint32_t id = 0; uint8_t* row = ParamsRowAt(PI_BehaviorParam_PC, i, &id);
            if (!row) continue;
            if (SafeRead(row + Bhv_VariationId, &var, 4) && var == variation && SafeRead(row + Bhv_JudgeId, &jd, 4) && jd == judge) { b = row; bhvId = id; }
        }
    }
    if (b && idOut) *idOut = bhvId;
    return b;
}

// The AtkParam_Pc row a (weapon variation, judge id) pair resolves to. The BehaviorParam_PC row
// id formula (1_0000_0000 + variation*1000 + judge, verified in ER 1.16 for 700/800/2400) is
// checked by reading the row's own variation/judge fields back, with a field scan as fallback.
struct AtkResolve { bool valid = false; int variation = -1, judge = -1; uint8_t* atkRow = nullptr; uint32_t bhvId = 0, atkId = 0; int bhvStamina = 0; };
AtkResolve g_resolve[2];
AtkResolve* ResolveAttack(int h, int wepVariation, int judge) {
    AtkResolve& r = g_resolve[h];
    if (r.variation == wepVariation && r.judge == judge) return r.valid ? &r : nullptr;
    r = AtkResolve{}; r.variation = wepVariation; r.judge = judge;
    if (!ParamsReady() || judge < 0) return nullptr;
    int cands[4]; const int nc = VariationCandidates(wepVariation, cands);
    uint8_t* b = nullptr; uint32_t bhvId = 0; int variation = wepVariation;
    for (int i = 0; i < nc && !b; ++i) { b = FindBehaviorRow(cands[i], judge, &bhvId); if (b) variation = cands[i]; }
    if (!b) { LOGW("[combat] no BehaviorParam_PC row for judge %d under variation %d or its class bases: strikes keep vanilla damage", judge, wepVariation); return nullptr; }
    if (variation != wepVariation) LOGI("[combat] %s hand: judge %d not under variation %d; using class variation %d", HandName(h), judge, wepVariation, variation);
    { uint32_t sz = 0; ParamsRow(PI_BehaviorParam_PC, bhvId, &sz); CheckRowSize("BEHAVIOR_PARAM_ST", sz, Bhv_RowSize); }
    int refId = -1; uint8_t refType = 0xFF;
    SafeRead(b + Bhv_RefType, &refType, 1); SafeRead(b + Bhv_RefId, &refId, 4); SafeRead(b + Bhv_Stamina, &r.bhvStamina, 4);
    if (refType != 0 || refId < 0) { LOGW("[combat] BehaviorParam_PC %u refType %u refId %d is not an attack: no scaling", bhvId, refType, refId); return nullptr; }
    uint32_t asz = 0;
    uint8_t* a = ParamsRow(PI_AtkParam_Pc, (uint32_t)refId, &asz);
    if (!a) { LOGW("[combat] no AtkParam_Pc row %d for BehaviorParam_PC %u", refId, bhvId); return nullptr; }
    CheckRowSize("ATK_PARAM_ST", asz, Atk_RowSize);
    r.atkRow = a; r.bhvId = bhvId; r.atkId = (uint32_t)refId; r.valid = true;
    uint16_t mv = 0; SafeRead(a + Atk_PhysCorr, &mv, 2);
    LOGI("[combat] %s hand judge %d -> BehaviorParam_PC %u (stamina %d) -> AtkParam_Pc %d (phys MV %u%%)", HandName(h), judge, bhvId, r.bhvStamina, refId, mv);
    return &r;
}

// Live edit of one AtkParam_Pc row's motion values; originals captured once, restored on end.
struct AtkEdit { bool active = false; uint8_t* row = nullptr; uint16_t orig[6]{}; float origSA = 0.f; float mult = -1.f; };
AtkEdit g_atk[2];
const size_t kAtkCorr[6] = {Atk_PhysCorr, Atk_MagCorr, Atk_FireCorr, Atk_ThunCorr, Atk_DarkCorr, Atk_StamCorr};

void RestoreAtk(int h) {
    AtkEdit& e = g_atk[h];
    if (!e.active) return;
    // Both hands on the same row: the other edit still owns it, leave its values in place.
    if (g_atk[1 - h].active && g_atk[1 - h].row == e.row) { e.active = false; e.row = nullptr; return; }
    for (int i = 0; i < 6; ++i) SafeWrite(e.row + kAtkCorr[i], &e.orig[i], 2);
    SafeWrite(e.row + Atk_SuperArmorCorr, &e.origSA, 4);
    e.active = false; e.row = nullptr; e.mult = -1.f;
}
void ApplyAtk(int h, uint8_t* row, float mult) {
    if (g_layoutBad || !row) return;
    AtkEdit& e = g_atk[h];
    if (e.active && e.row != row) RestoreAtk(h);
    if (!e.active) {
        const AtkEdit& o = g_atk[1 - h];
        if (o.active && o.row == row) { memcpy(e.orig, o.orig, sizeof(e.orig)); e.origSA = o.origSA; }
        else { for (int i = 0; i < 6; ++i) SafeRead(row + kAtkCorr[i], &e.orig[i], 2); SafeRead(row + Atk_SuperArmorCorr, &e.origSA, 4); }
        e.active = true; e.row = row; e.mult = -1.f;
    }
    if (std::fabs(mult - e.mult) < 0.002f) return;
    e.mult = mult;
    const Config& c = Cfg();
    for (int i = 0; i < 6; ++i) {
        if (i == 5 && !c.kineticStaminaDamageScales) continue;
        float v = (float)e.orig[i] * mult; uint16_t w = (uint16_t)(v < 0 ? 0 : v > 65535.f ? 65535.f : v + 0.5f);
        SafeWrite(row + kAtkCorr[i], &w, 2);
    }
    if (c.kineticPoiseScales) { float sa = e.origSA * mult; SafeWrite(row + Atk_SuperArmorCorr, &sa, 4); }
}

// Shield guard edit (EQUIP_PARAM_WEAPON_ST of the left-hand shield).
struct GuardEdit { bool active = false; uint8_t* row = nullptr; float origCut[4]{}; float origDark = 0.f; int16_t origDef = 0; float q = -1.f; } g_guard;
void RestoreGuard() {
    GuardEdit& e = g_guard;
    if (!e.active) return;
    for (int i = 0; i < 4; ++i) SafeWrite(e.row + Wep_GuardCut + i * 4, &e.origCut[i], 4);
    SafeWrite(e.row + Wep_DarkGuardCut, &e.origDark, 4);
    SafeWrite(e.row + Wep_StaminaGuardDef, &e.origDef, 2);
    e.active = false; e.row = nullptr; e.q = -1.f;
}
void ApplyGuard(uint8_t* row, float q) {
    if (g_layoutBad || !row) return;
    GuardEdit& e = g_guard;
    if (e.active && e.row != row) RestoreGuard();
    if (!e.active) {
        for (int i = 0; i < 4; ++i) SafeRead(row + Wep_GuardCut + i * 4, &e.origCut[i], 4);
        SafeRead(row + Wep_DarkGuardCut, &e.origDark, 4); SafeRead(row + Wep_StaminaGuardDef, &e.origDef, 2);
        e.active = true; e.row = row; e.q = -1.f;
    }
    if (std::fabs(q - e.q) < 0.01f) return;
    e.q = q;
    const Config& c = Cfg();
    const float cutMul = Lerp(c.blockMinCut, 1.f, q), defMul = Lerp(c.blockMinStability, 1.f, q);
    for (int i = 0; i < 4; ++i) { float v = e.origCut[i] * cutMul; SafeWrite(row + Wep_GuardCut + i * 4, &v, 4); }
    float d = e.origDark * cutMul; SafeWrite(row + Wep_DarkGuardCut, &d, 4);
    int16_t def = (int16_t)((float)e.origDef * defMul + 0.5f); SafeWrite(row + Wep_StaminaGuardDef, &def, 2);
}
void RestoreAll() { RestoreAtk(HandLeft); RestoreAtk(HandRight); RestoreGuard(); }

float QualityToMult(float q, const Config& c) {
    if (q <= 1.f) return Lerp(c.kineticMinMult, 1.f, q < 0 ? 0 : q);
    float t = (q - 1.f) / 0.5f; if (t > 1.f) t = 1.f;
    return Lerp(1.f, c.kineticMaxMult, t);
}
float Fatigue(int stam, int maxStam, const Config& c) {
    if (maxStam <= 0 || stam < 0 || c.kineticFatigueStart <= 0.f) return 1.f;
    float f = (float)stam / (float)maxStam;
    if (f >= c.kineticFatigueStart) return 1.f;
    return Lerp(c.kineticFatigueFloor, 1.f, f / c.kineticFatigueStart);
}

int g_prevStam = -1;   // stamina at the end of the previous frame body (block impact detection)

void GameFrameBody(uint8_t* player) {
    g_player = player;
    const Config& c = Cfg();
    SwingCmd cmd[2]; bool physical; BlockState blk;
    { std::lock_guard<std::mutex> g(g_mtx); ApplyConfigTemplates(); physical = PhysicalActiveLocked(); cmd[0] = g_cmd[0]; cmd[1] = g_cmd[1]; blk = g_block; }
    g_status.physical = physical;
    int maxStam = -1;
    int stam = ReadStamina(player, &maxStam); g_status.stamina = stam;
    const int stamAtEntry = stam;
    if (!physical) { RestoreAll(); g_leftShield.store(0); g_wepWeight[0].store(0.f); g_wepWeight[1].store(0.f); g_prevStam = stam; return; }
    g_status.paramsReady = ParamsReady();
    const bool kinetic = c.kineticEnabled && g_status.paramsReady && !g_layoutBad;
    ReadEquipment(player);
    const bool leftShield = c.blockEnabled && g_wep[HandLeft].valid && g_wep[HandLeft].isShield && g_armStyle != 3;
    g_leftShield.store(leftShield ? 1 : 0);
    g_status.shield = leftShield;

    CSChrTaeAnimEvent* ev = PlayerTaeEvent(player);
    if (!ev) { static double lastWarn = 0; double n = NowSec(); if (n - lastWarn > 10) { lastWarn = n; LOGW("[combat] player CSChrTaeAnimEvent not found (vtable mismatch?)"); } RestoreAll(); g_prevStam = stam; return; }

    bool anyStrike = false;
    // Per-strike stamina accounting: what the GAME took over the whole strike beyond our own
    // drain (one hit = one behavior cost; several = the same swing is hitting more than once).
    static int s_strikeStam[2] = {-1, -1}; static uint32_t s_strikeId[2] = {0, 0}; static int s_strikeFrames[2] = {0, 0};
    static int s_strikeHits[2] = {0, 0}; static double s_hitPauseUntil[2] = {0, 0};
    // HIT GATE. The game's own per-attack hit list does NOT deduplicate our injected attack: a
    // blade resting in an enemy hit it every frame (measured 2026-08-25: 8-10 hits, ~170 stamina
    // charged, in one 11-frame swing). The game charges the behavior's stamina cost exactly when a
    // hit lands, so a one-frame stamina drop during a strike IS the hit. After a hit the attack
    // instance pauses for KineticHitPause (the blade passes through this enemy; a second enemy
    // later in the arc can still be hit) or, at 0, the strike ends.
    const double nowS = NowSec();
    const bool hitNow = g_prevStam >= 0 && stamAtEntry >= 0 && (g_prevStam - stamAtEntry) >= c.kineticHitStamina;
    // Stamina modes: "off" takes the bar out of the loop entirely (no gate, no drain, no fatigue,
    // and the game's own on-hit charge is given back), "game" leaves the game's charges alone,
    // "full" adds ERVR's drain + fatigue on top.
    const bool stamOff = c.kineticStaminaMode == "off", stamFull = c.kineticStaminaMode == "full";
    static double s_lastHitHaptic[2] = {0, 0};
    for (int h = HandRight; h >= HandLeft; --h) {   // right first (main hand)
        SwingCmd& k = cmd[h];
        if (!k.active) {
            if (s_strikeStam[h] >= 0 && c.combatDebug) {
                const int now = ReadStamina(player);
                LOGD("[combat] strike %s #%u ended after %d frames, %d hits: game charged %d total (stamina %d -> %d)", HandName(h), s_strikeId[h], s_strikeFrames[h], s_strikeHits[h], s_strikeStam[h] - now, s_strikeStam[h], now);
            }
            s_strikeStam[h] = -1;
            RestoreAtk(h); continue;
        }
        if (k.first) { s_strikeId[h] = k.id; s_strikeFrames[h] = 0; s_strikeHits[h] = 0; s_hitPauseUntil[h] = 0; }
        ++s_strikeFrames[h];
        if (!k.first && hitNow && nowS >= s_hitPauseUntil[h]) {
            ++s_strikeHits[h];
            if (nowS - s_lastHitHaptic[h] > 0.1) { s_lastHitHaptic[h] = nowS; g_haptic.fetch_or(64u << h); }
            if (stamOff) { WriteStamina(player, g_prevStam); stam = g_prevStam; }   // the game charged its behavior cost: give it back
            if (c.combatDebug && (!c.kineticContinuous || s_strikeHits[h] == 1)) LOGD("[combat] %s strike HIT #%d (stamina %d -> %d)%s", HandName(h), s_strikeHits[h], g_prevStam, stamAtEntry, stamOff ? " refunded" : "");
            if (!c.kineticContinuous) {
                if (c.kineticHitPause <= 0.f) {
                    std::lock_guard<std::mutex> g(g_mtx);
                    if (g_cmd[h].id == k.id) g_cmd[h].active = false;
                    continue;   // instance retired; next frame restores the row and logs the strike
                }
                s_hitPauseUntil[h] = nowS + c.kineticHitPause;
            }
        }
        if (!c.kineticContinuous && !k.first && nowS < s_hitPauseUntil[h]) { anyStrike = true; continue; }   // paused: instance kept, no sweep
        if (h == HandLeft && leftShield) { RestoreAtk(h); continue; }   // the shield blocks, it does not strike
        Template t;
        { std::lock_guard<std::mutex> g(g_mtx); t = g_tpl[k.slot]; if (!t.valid && k.slot == SlotHeavy) t = g_tpl[SlotLight]; if (!t.valid && k.slot == SlotLeft) { t = g_tpl[SlotLight]; t.a.source = 2; } }
        if (!t.valid) continue;
        if (k.first) {
            // maxStam > 0 means the read is real: ER lets stamina go NEGATIVE after a hit is charged
            // at low stamina, and the old `stam >= 0` (meant for the -1 "unknown" value) waved every
            // strike through while exhausted - free attacks at -50 stamina.
            if (c.combatRequireStamina && !stamOff && (maxStam > 0 || stam >= 0) && stam < c.combatMinStamina) {
                std::lock_guard<std::mutex> g(g_mtx);
                if (g_cmd[h].id == k.id) { g_cmd[h].active = false; g_cmd[h].staminaFail = true; }
                g_haptic.fetch_or(4u << h);
                if (c.combatDebug) LOGD("[combat] %s swing rejected: stamina %d", HandName(h), stam);
                continue;
            }
            { std::lock_guard<std::mutex> g(g_mtx); if (g_cmd[h].id == k.id) g_cmd[h].first = false; }
            InterlockedIncrement(&g_swings);
        }
        anyStrike = true;
        // Stable per-hand storage: the game keys the attack instance on the args pointer across frames.
        static AttackBehaviorArgs s_ab[2]; static CSTaeAnimEventArgs s_args[2]; static float s_time[2]; static bool s_logNext[2];
        static int s_stamAfterDrain[2];
        AttackBehaviorArgs& ab = s_ab[h]; CSTaeAnimEventArgs& args = s_args[h];
        // Kinetic: scale the strike's attack row by quality x fatigue, refreshed every frame so the
        // value the sweep reads at contact reflects the tip speed at contact.
        float mult = 1.f; bool scaled = false;
        if (kinetic) {
            const HandWeapon& w = g_wep[h];
            AtkResolve* r = w.valid && w.row ? ResolveAttack(h, w.variation, t.a.judgeId) : nullptr;
            const float fat = stamOff ? 1.f : Fatigue(stam, maxStam, c);
            if (c.kineticContinuous) {
                // Every frame of contact is a hit, so each one is a slice of a vanilla hit sized by the
                // tip's speed right now: nothing at rest, KineticContinuousMult at KineticRefSpeed.
                const float ref = c.kineticRefSpeed > 0.5f ? c.kineticRefSpeed : 8.f;
                float qv = (k.tipLive / ref) * (k.tipLive / ref); if (qv > 1.f) qv = 1.f; if (qv < 0.f) qv = 0.f;
                mult = c.kineticContinuousMult * qv * fat * c.kineticDamageScale;
            } else mult = QualityToMult(k.quality, c) * fat * c.kineticDamageScale;
            if (r) { ApplyAtk(h, r->atkRow, mult); scaled = true; }
            g_status.lastQuality = k.quality; g_status.lastMult = scaled ? mult : 1.f;
        }
        if (k.first) {
            ab = t.a;
            ab.source = h == HandLeft ? 2 : 1;                       // always the swinging hand's weapon
            if (c.combatAttackIndexRotate > 0) ab.attackIndex = (int32_t)(k.id % (uint32_t)c.combatAttackIndexRotate);
            memset(&args, 0, sizeof(args));
            args.eventId = EV_AttackBehavior; args.args = &ab; args.from = 0.f; args.to = c.swingMaxSeconds > 0 ? c.swingMaxSeconds : 1.f;
            args.taeVersion = 0x1a; s_time[h] = 0.f; args.animTime = &s_time[h]; s_logNext[h] = c.combatDebug;
            int drain = 0;
            if (c.combatExtraStamina[k.slot] > 0) drain += c.combatExtraStamina[k.slot];
            if (kinetic && stamFull) {
                // The game's own cost for this very attack (BehaviorParam stamina) when the row
                // resolved, else the flat fallback scaled by weight. Quality scales it: a lazy
                // strike costs half, a full one all of it.
                float qc = k.qualityCommit; if (qc > 1.5f) qc = 1.5f;
                const AtkResolve& rr = g_resolve[h];
                float base = rr.valid && rr.bhvStamina > 0 ? (float)rr.bhvStamina * c.kineticStaminaScale
                                                           : c.kineticStaminaPerStrike * (1.f + (g_wep[h].valid ? g_wep[h].weight : 0.f) / 12.f);
                drain += (int)(base * (0.5f + 0.5f * qc) + 0.5f);
            }
            if (drain > 0 && stam > 0) { stam -= drain; if (stam < 0) stam = 0; WriteStamina(player, stam); }
            s_stamAfterDrain[h] = stam; s_strikeStam[h] = stam;
            if (c.combatDebug) {
                if (scaled) LOGD("[combat] strike %s %s: quality %.2f (tip %.1f m/s, arc %.2f m) mult %.2f drain %d stamina %d/%d", HandName(h), SlotName(k.slot), k.qualityCommit, k.tipSpeed, k.arc, mult, drain, stam, maxStam);
                else LOGD("[combat] strike %s %s: quality %.2f (tip %.1f m/s, arc %.2f m) VANILLA DAMAGE (no attack row to scale) drain %d stamina %d/%d", HandName(h), SlotName(k.slot), k.qualityCommit, k.tipSpeed, k.arc, drain, stam, maxStam);
            }
        } else s_time[h] = (float)(NowSec() - k.start);
        args.isFirstExecution = k.first ? 1 : 0;
        g_origExec(ev, &args);
        InterlockedIncrement(&g_injected);
        if (c.combatDebug && k.first) LOGD("[combat] injected %s attack (judgeId %d index %d source %u) stamina %d", SlotName(k.slot), ab.judgeId, ab.attackIndex, ab.source, stam);
        // Whether the GAME charged the behavior's stamina on top of ours: the delta a frame later
        // beyond our own drain (regen is < 1/frame; a blocked hit would also show here).
        if (!k.first && s_logNext[h]) { s_logNext[h] = false; int ns = ReadStamina(player); LOGD("[combat] ... next frame: stamina %d (game charged %d)", ns, s_stamAfterDrain[h] - ns); }
    }

    // ---- shield: guard quality -> shield params; blocked-hit / deflect detection -------------
    g_status.guarding = false; g_status.blockQuality = 0.f;
    if (leftShield && blk.raised) {
        g_status.guarding = true; g_status.blockQuality = blk.quality;
        if (kinetic) ApplyGuard(g_wep[HandLeft].row, blk.quality);
        // A stamina drop in one frame while guarding and not striking is a blocked hit (sprinting
        // drains far less per frame). If the shield was moving into the attack, it is a deflect.
        if (!anyStrike && g_prevStam >= 0 && stamAtEntry >= 0 && g_prevStam - stamAtEntry >= c.blockImpactStamina) {
            const int drop = g_prevStam - stamAtEntry;
            const bool deflect = blk.into >= c.deflectSpeed && (NowSec() - blk.intoT) < 0.25;
            if (deflect) { WriteStamina(player, g_prevStam); stam = g_prevStam; g_haptic.fetch_or(32u); }
            else g_haptic.fetch_or(16u);
            if (c.combatDebug) LOGD("[combat] blocked hit: stamina -%d quality %.2f into %.2f m/s%s", drop, blk.quality, blk.into, deflect ? " -> DEFLECT (refunded)" : "");
        }
    } else RestoreGuard();
    g_prevStam = ReadStamina(player);
}

// ---------------------------------------------------------------- swing detection (worker thread)
float TipSpeed(const Pose& p, float reach, float* handSpeed) {
    Vec3 fwd = p.rot.Rotate({0.f, 0.f, -1.f});
    Vec3 tip = p.linVel + p.angVel.Cross(fwd * reach);
    if (handSpeed) *handSpeed = p.linVel.Length();
    return tip.Length();
}
// Kinetic-energy shaped quality: (v/ref)^2, capped, times the arc factor (a strike that wound up
// through a real arc counts fully; a flick from rest counts 60%). The arc a weapon needs grows
// with its weight: a dagger is a flick, an ultra greatsword is a whole-body swing.
float ArcRefFor(float weight, const Config& c) {
    const float arcRef = c.kineticArcRef > 0.05f ? c.kineticArcRef : 0.5f;
    return arcRef * (1.f + weight / 10.f);
}
float StrikeQuality(float tipSpeed, float arc, float weight, const Config& c) {
    const float ref = c.kineticRefSpeed > 0.5f ? c.kineticRefSpeed : 8.f;
    float q = (tipSpeed / ref) * (tipSpeed / ref); if (q > 1.5f) q = 1.5f;
    float a = arc / ArcRefFor(weight, c); if (a > 1.f) a = 1.f;
    return q * (0.6f + 0.4f * a);
}
// Force-limited tip speed: the virtual weapon accelerates towards the real tip speed at most
// KineticAccel / (1 + weight/KineticAccelWeight) m/s^2 (braking is twice as easy).
float StepVirtualSpeed(float vVirt, float tipSpeed, float weight, float dt, const Config& c) {
    const float aMax = (c.kineticAccel > 1.f ? c.kineticAccel : 60.f) / (1.f + weight / (c.kineticAccelWeight > 0.1f ? c.kineticAccelWeight : 4.f));
    const float dv = tipSpeed - vVirt;
    const float lim = aMax * dt;
    if (dv > lim) return vVirt + lim;
    if (dv < -2.f * lim) return vVirt - 2.f * lim;
    return tipSpeed;
}

void StartSwing(int h, int slot, double now, float quality, float arc, float tip) {
    std::lock_guard<std::mutex> g(g_mtx);
    SwingCmd& k = g_cmd[h];
    k.active = true; k.first = true; k.slot = slot; k.start = now; k.id = g_sm[h].nextId++; k.staminaFail = false;
    k.quality = k.qualityCommit = quality; k.arc = arc; k.tipSpeed = tip; k.tipLive = tip;
}
void UpdateSwing(int h, float quality, float arc, float tip, float tipLive) {
    std::lock_guard<std::mutex> g(g_mtx);
    SwingCmd& k = g_cmd[h];
    if (!k.active) return;
    k.quality = quality; k.arc = arc; k.tipSpeed = tip; k.tipLive = tipLive;
}
void EndSwing(int h) { std::lock_guard<std::mutex> g(g_mtx); g_cmd[h].active = false; g_cmd[h].first = false; }

// Shield pose against the head -> guard held + block quality. The threat is assumed to be where
// you look, flattened to the horizon (enemies stand on the same ground).
//
// Two separate questions, because the first test showed quality gating the guard itself makes
// blocking flicker: (1) is the shield UP - anywhere in front of you above the hips - then the
// guard button is held continuously (with a release delay); (2) how GOOD is the block - facing,
// coverage of the head/chest line, steadiness - which only shapes absorption and stability.
void PollBlock(const VRTracking& tr, double now) {
    const Config& c = Cfg();
    BlockState b;
    { std::lock_guard<std::mutex> g(g_mtx); b = g_block; }
    const Pose& sh = tr.hand[HandLeft]; const Pose& hd = tr.head;
    static double s_upSince = 0, s_downSince = 0, s_lastLog = 0;
    if (!g_leftShield.load() || !sh.valid || !hd.valid) { b.raised = false; b.quality = 0.f; b.into = 0.f; s_upSince = s_downSince = 0; }
    else {
        Vec3 fwd = hd.rot.Forward(); fwd.y = 0.f; fwd = fwd.Normalized();
        if (fwd.Length() < 0.5f) fwd = {0.f, 0.f, -1.f};
        Vec3 n = sh.rot.Rotate(Quat::AxisAngle({1.f, 0.f, 0.f}, Deg2Rad(c.shieldNormalPitchDeg)).Rotate({0.f, 0.f, -1.f}));
        const float align = Clamp(n.Dot(fwd), 0.f, 1.f);
        Vec3 d = sh.pos - hd.pos;
        const float fd = d.Dot(fwd);
        Vec3 lat = d - fwd * fd; lat.y = 0.f;
        const float lateral = lat.Length(), height = d.y;
        auto band = [](float v, float lo, float hi, float fade) { if (v < lo) return Clamp(1.f - (lo - v) / fade, 0.f, 1.f); if (v > hi) return Clamp(1.f - (v - hi) / fade, 0.f, 1.f); return 1.f; };
        // Up: in front of the body (not hanging behind or at the far side), above the hips.
        const bool up = fd > -0.05f && lateral < 0.6f && height > -0.8f && height < 0.35f;
        if (up) { s_downSince = 0; if (s_upSince == 0) s_upSince = now; }
        else { s_upSince = 0; if (s_downSince == 0) s_downSince = now; }
        if (!b.raised && up && now - s_upSince >= 0.05) b.raised = true;
        if (b.raised && !up && now - s_downSince >= c.blockReleaseSeconds) b.raised = false;
        // Quality: how squarely the shield sits between the threat and the head/chest.
        const float cov = band(fd, 0.10f, 0.80f, 0.20f) * band(lateral, 0.f, 0.30f, 0.30f) * band(height, -0.70f, 0.20f, 0.20f);
        const float brace = 1.f - Clamp((sh.linVel.Length() - 0.6f) / 1.4f, 0.f, 1.f);
        b.quality = b.raised ? Clamp(align * 0.5f + cov * 0.35f + brace * 0.15f, 0.f, 1.f) : 0.f;
        const float v = sh.linVel.Dot(fwd);
        if (v > b.into || now - b.intoT > 0.12) { b.into = v; b.intoT = now; }
        // Calibration line (1 Hz while the shield is up): which controller axis faces forward
        // tells the right ShieldNormalPitch; the geometry tells why coverage is low.
        if (c.combatDebug && b.raised && now - s_lastLog > 1.0) {
            s_lastLog = now;
            const Vec3 ax = sh.rot.Rotate({1.f, 0.f, 0.f}), ay = sh.rot.Rotate({0.f, 1.f, 0.f}), az = sh.rot.Rotate({0.f, 0.f, -1.f});
            LOGD("[combat] shield: q %.2f align %.2f cov %.2f brace %.2f | fwd %.2f lat %.2f height %.2f | axes.fwd X %.2f Y %.2f -Z %.2f", b.quality, align, cov, brace, fd, lateral, height, ax.Dot(fwd), ay.Dot(fwd), az.Dot(fwd));
        }
    }
    std::lock_guard<std::mutex> g(g_mtx); g_block = b;
}

} // namespace

// ---------------------------------------------------------------- public API
bool CombatInit(uint8_t* base, bool versionOk) {
    ParamsInit();
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

void CombatShutdown() { RestoreAll(); }

// Physical combat exists only while the player is IN THE WORLD. Without the player check the
// attack-button block stayed armed on the title screen and in menus - RB (right trigger) and
// RT (right grip) silently did nothing there, since no swing can fire to replace them
// (reported 2026-08-21: "right trigger seems disabled").
// AND only while the world is not mid-transition (2026-08-23): the arm motion of pressing A
// on the fast-travel confirm reads as a ~4 m/s swing, so every travel injected an
// attack-behavior event into the player 200-400 ms into world teardown - executing the
// game's behavior dispatch there is a coin-flip crash (CS::ChrIns + garbage index, second
// travel of the 21:59 session). GameERTransitionActive covers request, teardown, load and
// arrival-settle; CombatPollHands resets any in-flight swing the moment this goes false.
bool CombatHandlesSwings() {
    if (!GameERPlayerActive()) return false;
    if (GameERTransitionActive()) {
        // One line per transition window, so a future postmortem can see the suppression.
        static ULONGLONG s_lastLog = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - s_lastLog > 10000) { s_lastLog = now; LOGD("[combat] swings suppressed: world transition"); }
        return false;
    }
    std::lock_guard<std::mutex> g(g_mtx);
    return g_status.taeHooked && PhysicalActiveLocked();
}

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

float CombatHandWeight(int hand) { return hand == HandLeft || hand == HandRight ? g_wepWeight[hand].load() : 0.f; }

uint16_t CombatHeldButtons() {
    const Config& c = Cfg();
    if (!c.blockEnabled || !g_leftShield.load() || !CombatHandlesSwings()) return 0;
    bool raised; { std::lock_guard<std::mutex> g(g_mtx); raised = g_block.raised; }
    return raised ? ParseXInputButton(c.guardButton) : 0;
}

void CombatPollHands(const VRTracking& tr, const VRInputState& in, double now) {
    const Config& c = Cfg();
    if (!CombatHandlesSwings()) {
        for (int h = 0; h < 2; ++h) { if (g_sm[h].st != HandSM::Idle) EndSwing(h); g_sm[h].st = HandSM::Idle; g_sm[h].tipValid = false; g_sm[h].arc = 0; }
        std::lock_guard<std::mutex> g(g_mtx); g_block = BlockState{};
        return;
    }
    PollBlock(tr, now);
    const bool leftShield = g_leftShield.load() != 0;
    for (int h = 0; h < 2; ++h) {
        HandSM& s = g_sm[h];
        const Pose& p = tr.hand[h];
        bool grip = in.buttons & (h == HandLeft ? VRB_LGrip : VRB_RGrip);
        bool allowed = p.valid && in.controllerConnected[h] && (h == HandRight || (c.combatLeftHand && !leftShield)) && (!c.gestureRequiresGrip || grip);
        const float reach = h == HandRight ? c.weaponReachRight : c.weaponReachLeft;
        float handSpd = 0.f;
        float tip = allowed ? TipSpeed(p, reach, &handSpd) : 0.f;
        const float weight = g_wepWeight[h].load();
        // With the inertial arm (game_er) the WEAPON the game renders and sweeps lags the
        // controller by its weight; its motion is the truth for speed and arc, so damage follows
        // what you see hit. The worker-side virtual-speed model is the fallback without it.
        float wmTip = 0.f, wmHand = 0.f, wmArc = 0.f;
        const bool inertial = c.kineticInertia && allowed && GameERWeaponMotion(h, &wmTip, &wmHand, &wmArc);
        if (inertial) { tip = wmTip; handSpd = wmHand; s.arc = wmArc; s.tipValid = false; }
        else if (p.valid) {
            // Arc: tip path length since the tip last came to rest (captures the windup before the
            // on-threshold).
            Vec3 tipPos = p.pos + p.rot.Rotate({0.f, 0.f, -1.f}) * reach;
            if (s.tipValid && tip >= c.swingOffSpeed) s.arc += (tipPos - s.lastTip).Length();
            else if (tip < c.swingOffSpeed) s.arc = 0.f;
            s.lastTip = tipPos; s.tipValid = true;
        } else { s.tipValid = false; s.arc = 0.f; }
        s.vSmooth = s.vSmooth * 0.6f + tip * 0.4f;
        {
            float dt = s.lastPoll > 0 ? (float)(now - s.lastPoll) : 0.f; s.lastPoll = now;
            if (dt < 0.f) dt = 0.f; if (dt > 0.05f) dt = 0.05f;
            s.vVirt = inertial ? s.vSmooth : StepVirtualSpeed(s.vVirt, tip, weight, dt, c);
            if (s.st == HandSM::Idle) s.vVirtPeak = s.vVirt; else if (s.vVirt > s.vVirtPeak) s.vVirtPeak = s.vVirt;
        }
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
            // Heavier weapons take longer to commit: the hitbox goes live only once the virtual
            // weapon has had time to get moving (a real ultra greatsword R1 has ~0.5 s of startup).
            const float armWindow = c.kineticEnabled ? c.swingArmWindow + weight * c.kineticArmPerWeight : c.swingArmWindow;
            if (now - s.t >= armWindow || (decel && !c.kineticEnabled)) {
                const float vq = c.kineticEnabled ? s.vVirtPeak : s.peak;
                const float q = StrikeQuality(vq, s.arc, weight, c);
                int slot;
                if (h == HandLeft) slot = SlotLeft;
                else if (c.kineticEnabled) slot = (q >= c.kineticHeavyQuality && s.arc >= 0.8f * ArcRefFor(weight, c)) ? SlotHeavy : SlotLight;
                else slot = s.peak >= c.heavySwingSpeed ? SlotHeavy : SlotLight;
                StartSwing(h, slot, now, q, s.arc, vq); s.st = HandSM::Swinging; s.t = now;
                g_haptic.fetch_or(1u << h);
                if (c.combatDebug) LOGD("[combat] %s swing start: %s (tip %.2f m/s, virtual %.2f m/s, hand %.2f m/s, arc %.2f/%.2f m, weight %.1f, quality %.2f)", HandName(h), SlotName(slot), s.peak, s.vVirtPeak, handSpd, s.arc, ArcRefFor(weight, c), weight, q);
            }
            break; }
        case HandSM::Swinging: {
            if (tip > s.peak) s.peak = tip;
            // Live quality: the virtual weapon's speed now, floored at 70% of its peak so a hit
            // landing late in the arc is not punished for the natural deceleration.
            float vEff = c.kineticEnabled ? (s.vVirt > 0.7f * s.vVirtPeak ? s.vVirt : 0.7f * s.vVirtPeak)
                                          : (s.vSmooth > 0.7f * s.peak ? s.vSmooth : 0.7f * s.peak);
            UpdateSwing(h, StrikeQuality(vEff, s.arc, weight, c), s.arc, vEff, inertial ? tip : s.vSmooth);
            if (!allowed || tip < c.swingOffSpeed || now - s.t >= c.swingMaxSeconds) {
                if (c.combatDebug) LOGD("[combat] %s swing end after %.0f ms (peak %.2f m/s, virtual peak %.2f m/s, arc %.2f m)", HandName(h), (now - s.t) * 1000.0, s.peak, s.vVirtPeak, s.arc);
                EndSwing(h); s.st = HandSM::Cooldown; s.t = now;
            }
            break; }
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
    // Live, not the value last written by the in-world frame body: that one reads 0 whenever
    // the player is not spawned, which hid an armed RB/RT block behind "physical 0".
    const bool phys = CombatHandlesSwings();
    CombatStatus s = g_status;
    s.physical = phys;
    { std::lock_guard<std::mutex> g(g_mtx); s.learnedLight = g_tpl[SlotLight].valid; s.learnedHeavy = g_tpl[SlotHeavy].valid; s.learnedLeft = g_tpl[SlotLeft].valid; }
    s.injected = InterlockedExchange(&g_injected, 0); s.swings = InterlockedExchange(&g_swings, 0);
    return s;
}
}
