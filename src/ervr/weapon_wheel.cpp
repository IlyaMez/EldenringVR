#include "weapon_wheel.h"
#include "weapon_wheel_logic.h"
#include "belt.h"
#include "combat.h"
#include "config.h"
#include "equipment_layout.h"
#include "game_er.h"
#include "game_memory.h"
#include "log.h"
#include "overlay_canvas.h"
#include "overlay_layers.h"
#include "params_er.h"
#include <windows.h>
#include <xinput.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ervr {
namespace {

std::atomic<int> g_openHandShared{-1};   // worker -> combat: the hand whose wheel is up
std::atomic<bool> g_busy{false};         // a switch is pressing the D-pad: the belt waits with its pouch chord
constexpr int kWheelPx = 512;      // the wheel image, square
constexpr int kIconMaxPx = 128;    // icons are drawn at ~120 px: kept at most twice that, so they shrink cleanly
constexpr int kCursorPx = 64;
constexpr float kCursorM = 0.028f; // the cursor dot's quad, metres across
const char* HandName(int h) { return h == HandRight ? "right" : "left"; }
uint8_t* Deref(const void* a) { uint8_t* p = nullptr; return SafeRead(a, &p, sizeof(p)) ? p : nullptr; }

// ---------------------------------------------------------------- equipment (game thread -> worker)
struct Equip {
    bool valid = false;
    uint32_t seq = 0;
    int armStyle = 0;
    int active[2] = {0, 0};
    int paramId[2][kWheelSlots] = {};
    int iconId[2][kWheelSlots] = {};   // -1 = not resolved (params not loaded, no row)
    uint16_t wepType[2][kWheelSlots] = {};
    bool SameAs(const Equip& o) const {
        return valid == o.valid && armStyle == o.armStyle && active[0] == o.active[0] && active[1] == o.active[1] &&
               std::memcmp(paramId, o.paramId, sizeof(paramId)) == 0 && std::memcmp(iconId, o.iconId, sizeof(iconId)) == 0;
    }
};
std::mutex g_equipMtx;
Equip g_equip;

void ReadEquip(uint8_t* player) {
    Equip e;
    uint8_t* asmp = player ? Deref(player + Player_ChrAsm) : nullptr;
    uint32_t sl = 0, sr = 0; int ids[2 * kWheelSlots] = {};
    if (asmp && SafeRead(asmp + Asm_ArmStyle, &e.armStyle, 4) && SafeRead(asmp + Asm_LeftSlot, &sl, 4) &&
        SafeRead(asmp + Asm_RightSlot, &sr, 4) && SafeRead(asmp + Asm_ParamIds, ids, sizeof(ids)) && sl < 3 && sr < 3) {
        e.valid = true;
        e.active[HandLeft] = (int)sl; e.active[HandRight] = (int)sr;
        // param id -> (icon id, wepType); resolved once per weapon (a linear row scan), game thread only.
        static std::unordered_map<int, std::pair<int, uint16_t>> s_rows;
        for (int h = 0; h < 2; ++h)
            for (int k = 0; k < kWheelSlots; ++k) {
                const int id = ids[k * 2 + h];
                e.paramId[h][k] = id; e.iconId[h][k] = -1;
                if (!WheelSlotFilled(id)) continue;
                auto it = s_rows.find(id);
                if (it == s_rows.end() && ParamsReady()) {
                    // Every affinity and upgrade level of a weapon shares its base row's icon.
                    uint8_t* row = ParamsRow(PI_EquipParamWeapon, (uint32_t)(id / 100) * 100u);
                    uint16_t icon = 0, type = 0;
                    if (row) { SafeRead(row + Wep_IconId, &icon, 2); SafeRead(row + Wep_Type, &type, 2); }
                    it = s_rows.emplace(id, std::make_pair(row ? (int)icon : -1, type)).first;
                }
                if (it != s_rows.end()) { e.iconId[h][k] = it->second.first; e.wepType[h][k] = it->second.second; }
            }
    }
    std::lock_guard<std::mutex> g(g_equipMtx);
    if (!e.SameAs(g_equip)) { e.seq = g_equip.seq + 1; g_equip = e; }
}
// SafeRead never faults; this is for the unexpected, and nothing in ReadEquip holds a lock while it
// reads game memory.
void ReadEquipGuarded(uint8_t* player) {
    __try { ReadEquip(player); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------- the gesture (worker)
enum class St { Idle, Pass, Pending, Open, Swallow };
struct HandState {
    St st = St::Idle;
    double downAt = 0, stillSince = 0;
    // Why this squeeze has not opened the wheel - said in the log when a long one ends without it.
    const char* why = nullptr;
    bool expected = false;    // a block the player meant (a grab, two-handing, a menu): no log line
    float maxSpeed = 0.f;
};
HandState g_hand[2];

struct OpenWheel {
    int hand = -1;                       // -1 = closed
    WheelFrame frame;
    double openedAt = 0;
    int hover = -1, active = 0;
    bool filled[kWheelSlots] = {};
    int paramId[kWheelSlots] = {}, iconId[kWheelSlots] = {};
    bool iconMissing = false;            // a filled slot had no icon when last drawn
    double nextIconLook = 0;
    float cx = 0.f, cy = 0.f;            // the hand over the wheel, metres
} g_open;
WheelCommit g_commit[2];
double g_commitStart[2] = {0.0, 0.0};
int g_commitFrom[2] = {0, 0};
// Jump: a target more than one press away gets the active slot moved to the one before it first, so
// the game's own switch runs once. Worker asks, the game thread writes on its own frame.
struct Jump { bool tried = false; int slot = -1; double at = 0.0; };
Jump g_jump[2];
std::atomic<int> g_jumpReq[2] = {-1, -1};     // worker -> game thread: put the active slot here
std::atomic<int> g_jumpResult[2] = {0, 0};    // game thread -> worker: 1 written, -1 refused

// PlayerGameData's own ChrAsm, the save-side copy the character's is applied from (the cheat table's
// CurrentWepSlotOffset: left +0x328, right +0x32C = ChrAsm +0xC / +0x10).
constexpr size_t PGD_ChrAsm = 0x31C;
// Game thread. Moves the hand's active slot in both ChrAsm copies - only after checking that the save
// copy mirrors the character's exactly (arm style, active slots, the six weapons), so a layout that moved
// in a patch can never be written into. False = refused (logged once), the switch cycles instead.
bool WriteActiveSlot(uint8_t* player, int hand, int slot) {
    uint8_t* chrAsm = player ? Deref(player + Player_ChrAsm) : nullptr;
    uint8_t* pgd = GameERPlayerGameData();
    if (!chrAsm || !pgd || slot < 0 || slot >= kWheelSlots) return false;
    uint8_t* saveAsm = pgd + PGD_ChrAsm;
    uint8_t a[0x1C], b[0x1C], wa[24], wb[24];
    const bool read = SafeRead(chrAsm + Asm_ArmStyle, a, sizeof(a)) && SafeRead(saveAsm + Asm_ArmStyle, b, sizeof(b)) &&
                      SafeRead(chrAsm + Asm_ParamIds, wa, sizeof(wa)) && SafeRead(saveAsm + Asm_ParamIds, wb, sizeof(wb));
    if (!read || std::memcmp(a, b, sizeof(a)) != 0 || std::memcmp(wa, wb, sizeof(wa)) != 0) {
        static bool s_warned = false;
        if (!s_warned) { s_warned = true; LOGW("[wheel] PlayerGameData+0x%zX does not mirror the character's ChrAsm (moved in a patch?): no jumps, the switch cycles through the slots", PGD_ChrAsm); }
        return false;
    }
    const int32_t v = slot;
    const size_t at = hand == HandRight ? Asm_RightSlot : Asm_LeftSlot;
    return SafeWrite(saveAsm + at, &v, 4) && SafeWrite(chrAsm + at, &v, 4);
}
std::atomic<long> g_opens{0}, g_switches{0}, g_failed{0};

Canvas g_canvas;                         // worker only

void Render(double now) {
    const Config& c = Cfg();
    const float S = (float)kWheelPx, C = S * 0.5f;
    const float size = std::max(c.wheelSize, 0.1f);
    const float rBg = 0.47f * S, rItems = 0.30f * S, rSlot = 0.135f * S;
    const float rDead = Clamp(c.wheelDeadZone / size * S, 8.f, rItems - rSlot);
    const bool right = g_open.hand == HandRight;
    // The disc and its rings never change while the settings do not: drawn once, copied per render,
    // so a hover change only costs the slots (the worker also polls the controllers).
    static Canvas s_base;
    static float s_baseDead = -1.f;
    if (s_base.px.empty() || s_baseDead != rDead) {
        s_baseDead = rDead;
        s_base.Reset(kWheelPx);
        s_base.Vignette(C, C, rBg, Col(kDark, 0.66f), Col(kDark, 0.46f));
        s_base.Ring(C, C, rBg - 1.5f, 2.5f, Col(kGold, 0.85f));
        s_base.Ring(C, C, rBg - 7.f, 1.f, Col(kGold, 0.30f));
        s_base.Ring(C, C, rDead, 1.5f, Col(kGold, 0.35f));
    }
    Canvas& cv = g_canvas;
    cv = s_base;
    if (g_open.hover >= 0)
        cv.Sector(C, C, rDead + 4.f, rItems + rSlot, WheelSlotAngle(g_open.hover, right), 0.42f, Col(kGold, 0.22f));
    g_open.iconMissing = false;
    for (int k = 0; k < kWheelSlots; ++k) {
        const float a = WheelSlotAngle(k, right);
        const float x = C + rItems * std::sin(a), y = C - rItems * std::cos(a);
        const bool hover = k == g_open.hover, active = k == g_open.active;
        if (!g_open.filled[k]) {
            cv.Disc(x, y, rSlot * 0.82f, Col(kDark, 0.35f));
            cv.Ring(x, y, rSlot * 0.82f, 1.5f, Col(kGold, 0.35f), 16);
            continue;
        }
        if (hover) cv.Glow(x, y, rSlot + 2.f, 9.f, Col(kGoldHi, 0.55f));
        cv.Disc(x, y, rSlot, hover ? Col(kDarkWarm, 0.92f) : Col(kDark, 0.80f));
        cv.Ring(x, y, rSlot, hover ? 4.f : active ? 2.5f : 1.8f, hover ? Col(kGoldHi, 1.f) : Col(kGold, active ? 0.95f : 0.70f));
        const float iconSize = (hover ? 1.78f : 1.62f) * rSlot;
        if (const Icon* ic = CanvasIcon(g_open.iconId[k], kIconMaxPx)) cv.Image(*ic, x, y, iconSize, hover || active ? 1.f : 0.85f);
        else { cv.Digit(k + 1, x, y, rSlot * 0.95f, Col(kParchment, 0.9f)); g_open.iconMissing = true; }
        if (active) {   // the weapon in hand: a gold pip just outside its slot, toward the rim
            const float r = rItems + rSlot + 11.f;
            cv.Disc(C + r * std::sin(a), C - r * std::cos(a), 7.5f, Col(kDark, 0.8f));
            cv.Disc(C + r * std::sin(a), C - r * std::cos(a), 5.5f, Col(kGoldHi, 1.f));
        }
    }
    std::vector<uint32_t> img;
    cv.EncodeSrgb(img, Clamp(c.wheelOpacity, 0.f, 1.f));
    OverlayPublishImage(OverlayWheel, kWheelPx, img);
}
void RenderCursor() {
    Canvas cv; cv.Reset(kCursorPx);
    const float C = kCursorPx * 0.5f;
    cv.Glow(C, C, 10.f, 6.f, Col(kGoldHi, 0.35f));
    cv.Disc(C, C, 9.f, Col(kParchment, 0.95f));
    cv.Ring(C, C, 16.f, 2.2f, Col(kGold, 0.9f));
    std::vector<uint32_t> img;
    cv.EncodeSrgb(img, 1.f);
    OverlayPublishImage(OverlayWheelCursor, kCursorPx, img);
}

void Publish(double now, const Config& c) {
    g_openHandShared.store(g_open.hand, std::memory_order_relaxed);
    if (g_open.hand < 0) {
        OverlayPublishPose(OverlayWheel, false, Pose{}, 0.f);
        OverlayPublishPose(OverlayWheelCursor, false, Pose{}, 0.f);
        return;
    }
    const WheelFrame& f = g_open.frame;
    // Opens from 70% to full size over 120 ms: enough to read as "it appeared here", too short to aim at.
    const float t = Clamp((float)((now - g_open.openedAt) / 0.12), 0.f, 1.f);
    const float grow = 0.7f + 0.3f * t * t * (3.f - 2.f * t);
    Pose wheel;
    wheel.pos = f.center;
    wheel.rot = WheelOrientation(f);
    wheel.valid = true;
    OverlayPublishPose(OverlayWheel, true, wheel, std::max(c.wheelSize, 0.1f) * grow);
    // The cursor rides the plane a few mm toward the eyes, clamped to the wheel's rim.
    const float rim = 0.47f * std::max(c.wheelSize, 0.1f);
    float x = g_open.cx, y = g_open.cy;
    const float r = std::sqrt(x * x + y * y);
    if (r > rim) { x *= rim / r; y *= rim / r; }
    Pose cursor;
    cursor.pos = f.center + f.right * x + f.up * y + f.normal * 0.004f;
    cursor.rot = wheel.rot;
    cursor.valid = true;
    OverlayPublishPose(OverlayWheelCursor, true, cursor, kCursorM);
}

void Open(int h, const VRTracking& tr, const Equip& eq, double now) {
    g_open = OpenWheel{};
    g_open.hand = h;
    g_open.frame = WheelFrameFacing(tr.hand[h].pos, tr.head.pos, tr.head.rot.Up());
    g_open.openedAt = now;
    g_open.active = eq.active[h];
    char slots[384]; int n = 0;
    for (int k = 0; k < kWheelSlots; ++k) {
        g_open.filled[k] = WheelSlotFilled(eq.paramId[h][k]);
        g_open.paramId[k] = eq.paramId[h][k];
        g_open.iconId[k] = g_open.filled[k] ? eq.iconId[h][k] : -1;
        const bool icon = CanvasIcon(g_open.iconId[k], kIconMaxPx) != nullptr;
        if (n < 0 || n >= (int)sizeof(slots)) break;
        if (!g_open.filled[k]) n += snprintf(slots + n, sizeof(slots) - n, "%s slot %d empty", k ? "," : "", k + 1);
        else n += snprintf(slots + n, sizeof(slots) - n, "%s slot %d weapon %d icon %d%s%s", k ? "," : "", k + 1,
                           g_open.paramId[k], g_open.iconId[k], icon ? "" : " (icon not read yet)",
                           k == g_open.active ? " [in hand]" : "");
    }
    g_opens.fetch_add(1, std::memory_order_relaxed);
    LOGI("[wheel] %s hand: open -%s", HandName(h), slots);
    Render(now);
}

void Close(const char* why) {
    if (g_open.hand < 0) return;
    if (why) LOGI("[wheel] %s hand: closed without switching (%s)", HandName(g_open.hand), why);
    g_open.hand = -1;
}

bool HandAllowed(const std::string& hands, int h) {
    if (hands == "off" || hands == "none") return false;
    if (hands == "right") return h == HandRight;
    if (hands == "left") return h == HandLeft;
    return true;
}

} // namespace

WheelPadOut WeaponWheelUpdate(const VRTracking& tr, const VRInputState& in, double now, bool menu) {
    const Config& c = Cfg();
    WheelPadOut out;
    CanvasInit();
    if (c.wheelEnabled) {
        static bool s_started = false;
        if (!s_started) {
            s_started = true;
            ParamsInit();   // combat normally did this already; the wheel must not depend on it
            RenderCursor();
            LOGI("[wheel] hold a grip still for %.2f s to open that hand's weapon wheel (hands: %s, switch: %s); a shorter squeeze sends the grip's own button",
                 c.wheelHoldSeconds, c.wheelHands.c_str(), c.wheelSwitch.c_str());
        }
    }
    Equip eq; { std::lock_guard<std::mutex> g(g_equipMtx); eq = g_equip; }
    // A new loadout: decode its icons now, not in the tick the wheel opens, and say what the wheel sees.
    static uint32_t s_warmSeq = 0;
    if (c.wheelEnabled && eq.valid && eq.seq != s_warmSeq) {
        s_warmSeq = eq.seq;
        for (int h = 0; h < 2; ++h)
            for (int k = 0; k < kWheelSlots; ++k)
                if (WheelSlotFilled(eq.paramId[h][k])) CanvasIcon(eq.iconId[h][k], kIconMaxPx);
        static int s_logged[2][kWheelSlots] = {};
        if (std::memcmp(s_logged, eq.paramId, sizeof(s_logged)) != 0) {
            std::memcpy(s_logged, eq.paramId, sizeof(s_logged));
            auto slot = [&](int h, int k) { return WheelSlotFilled(eq.paramId[h][k]) ? eq.paramId[h][k] : 0; };
            LOGI("[wheel] equipment: right %d / %d / %d, left %d / %d / %d (0 = empty)", slot(HandRight, 0), slot(HandRight, 1),
                 slot(HandRight, 2), slot(HandLeft, 0), slot(HandLeft, 1), slot(HandLeft, 2));
        }
    }
    const bool playerActive = GameERPlayerActive(), transition = GameERTransitionActive();
    const bool twoHanded = eq.valid && (eq.armStyle == 2 || eq.armStyle == 3);
    for (int h = 0; h < 2; ++h) {
        HandState& s = g_hand[h];
        const float grip = in.handTrigger[h];
        const bool down = grip >= c.gripThreshold, up = grip < 0.5f * c.gripThreshold;
        bool holding = false;
        const bool nearHilt = CombatTwoHandGrip(h, &holding);
        const bool tracked = tr.hand[h].valid && tr.head.valid && in.controllerConnected[h];
        const float speed = tr.hand[h].linVel.Length();
        WheelGate gate;
        gate.enabled = c.wheelEnabled;
        gate.handAllowed = HandAllowed(c.wheelHands, h);
        gate.playerActive = playerActive; gate.transition = transition; gate.menu = menu;
        gate.twoHanded = twoHanded;
        gate.grabbing = holding;
        gate.belt = BeltOwnsHand(h);
        bool filled[kWheelSlots];
        for (int k = 0; k < kWheelSlots; ++k) filled[k] = eq.valid && WheelSlotFilled(eq.paramId[h][k]);
        gate.weapons = eq.valid && WheelHasWeapons(filled);
        const char* blocked = WheelBlocked(gate);
        // A squeeze that ends without the wheel after the hold time says why, unless the block was meant.
        auto ended = [&] {
            if (c.wheelEnabled && !s.expected && now - s.downAt >= c.wheelHoldSeconds)
                LOGI("[wheel] %s grip held %.1f s without opening the wheel: %s", HandName(h), now - s.downAt,
                     s.why ? s.why : s.maxSpeed > c.wheelStillSpeed ? "the hand kept moving faster than StillSpeed - hold it still" : "let go before HoldSeconds");
            s.st = St::Idle;
        };
        switch (s.st) {
        case St::Idle:
            if (!down) break;
            s.downAt = s.stillSince = now; s.maxSpeed = 0.f; s.why = nullptr; s.expected = false;
            // Closed at the other weapon's hilt it is a two-hand grab: combat's, not ours.
            if (blocked || nearHilt) {
                s.st = St::Pass;
                s.why = blocked ? blocked : "it closed at the other weapon's hilt (a two-hand grab)";
                s.expected = nearHilt || WheelBlockExpected(blocked, gate);
            } else s.st = St::Pending;
            break;
        case St::Pass:
        case St::Swallow:
            if (up) ended();
            break;
        case St::Pending:
            if (up) {
                // Let go before the wheel could have opened: the grip's own button, as one tap.
                if (now - s.downAt <= std::max(c.wheelHoldSeconds, 0.1f)) { out.tapGrip[h] = true; s.st = St::Idle; }
                else ended();
            } else if (blocked) {
                s.st = St::Swallow;   // it became a two-hand grab, a menu, ...: no wheel and no tap
                s.why = blocked; s.expected = WheelBlockExpected(blocked, gate);
            } else if (nearHilt || !tracked) {
                s.stillSince = now;   // passing the other hilt, or a tracking gap: the hold waits
                s.why = !tracked ? "the controller lost tracking" : "the hand was near the other weapon's hilt";
            } else {
                if (speed > s.maxSpeed) s.maxSpeed = speed;
                if (WheelHoldReady(s.stillSince, now, speed, c.wheelStillSpeed, c.wheelHoldSeconds)) {
                    if (g_open.hand >= 0) s.why = "the other hand's wheel was open";
                    else {
                        Open(h, tr, eq, now);
                        s.st = St::Open;
                        out.hapticAmp[h] = 0.35f; out.hapticSec[h] = 0.03f;
                    }
                }
            }
            break;
        case St::Open: {
            if (blocked) {
                Close(blocked); s.st = St::Swallow;
                s.why = blocked; s.expected = true;   // already said by Close
                break;
            }
            // A tracking gap freezes the pick where it was; letting go still counts.
            const int pick = tracked ? (WheelProject(g_open.frame, tr.hand[h].pos, g_open.cx, g_open.cy),
                                        WheelPick(g_open.cx, g_open.cy, g_open.filled, h == HandRight, c.wheelDeadZone, g_open.hover))
                                     : g_open.hover;
            if (pick != g_open.hover) {
                g_open.hover = pick;
                if (pick >= 0) { out.hapticAmp[h] = 0.2f; out.hapticSec[h] = 0.015f; }
                Render(now);
            } else if (g_open.iconMissing && now >= g_open.nextIconLook) {
                g_open.nextIconLook = now + 0.2;   // an icon the loader finishes while the wheel is up
                Render(now);
            }
            if (!up) break;
            const int target = g_open.hover;
            if (target >= 0 && target != eq.active[h]) {
                g_commit[h].Start(target);
                g_commitStart[h] = now; g_commitFrom[h] = eq.active[h];
                g_jump[h] = Jump{};
                LOGI("[wheel] %s hand: picked slot %d (weapon %d) - pressing %s until the game's slot %d becomes %d",
                     HandName(h), target + 1, g_open.paramId[target], h == HandRight ? "DPAD_RIGHT" : "DPAD_LEFT",
                     eq.active[h] + 1, target + 1);
                out.hapticAmp[h] = 0.5f; out.hapticSec[h] = 0.05f;
                g_open.hand = -1;
            } else Close(target >= 0 ? "picked the weapon already in hand" : "let go in the middle");
            s.st = St::Idle;
            break;
        }
        }
        out.holdGrip[h] = s.st == St::Pending || s.st == St::Open || s.st == St::Swallow;
    }
    // The switch itself: the game's own button, until its active slot answers.
    WheelCommitTiming timing;
    timing.press = std::max((double)c.gesturePressDuration, 0.10);
    timing.timeout = Clamp(c.wheelSwitchTimeout, 0.3f, 5.f);
    for (int h = 0; h < 2; ++h) {
        WheelCommit& cm = g_commit[h];
        if (cm.target < 0) continue;
        if (!eq.valid || menu || !playerActive || transition) {
            LOGI("[wheel] %s hand: switch to slot %d abandoned (%s)", HandName(h), cm.target + 1, menu ? "a menu opened" : "left the world");
            cm.target = -1;
            continue;
        }
        // The belt is holding the game's Event Action button for a pouch item: a D-pad press now would use a
        // pouch item instead of switching. The switch waits it out (the belt never starts one while this runs).
        if (BeltPadBusy()) continue;
        // Jump: before the first press of a switch more than one press long, the active slot goes to the
        // slot just before the target, so the one press that follows is the game's only switch.
        Jump& jp = g_jump[h];
        if (cm.phase == 0 && cm.presses == 0 && !jp.tried) {
            jp.tried = true;
            bool filledH[kWheelSlots];
            for (int k = 0; k < kWheelSlots; ++k) filledH[k] = WheelSlotFilled(eq.paramId[h][k]);
            if (c.wheelSwitch != "cycle" && WheelCycleSteps(eq.active[h], cm.target, filledH) >= 2) {
                jp.slot = WheelSlotBefore(cm.target, filledH);
                jp.at = now;
                g_jumpResult[h].store(0);
                g_jumpReq[h].store(jp.slot);
                continue;
            }
        }
        if (jp.slot >= 0) {   // the game thread writes it on its next frame; the snapshot shows it the frame after
            const int r = g_jumpResult[h].load();
            if (r == 0 && eq.active[h] != jp.slot && now - jp.at < 0.3) continue;
            if (eq.active[h] == jp.slot)
                LOGI("[wheel] %s hand: jumped to slot %d, one press to slot %d", HandName(h), jp.slot + 1, cm.target + 1);
            else
                LOGI("[wheel] %s hand: jump to slot %d did not take (%s) - cycling instead", HandName(h), jp.slot + 1,
                     r < 0 ? "refused" : "the slot did not change");
            jp.slot = -1;
        }
        const int presses = cm.presses;
        switch (WheelCommitStep(cm, eq.active[h], now, timing)) {
        case WheelStep::Press:
            out.buttons |= h == HandRight ? XINPUT_GAMEPAD_DPAD_RIGHT : XINPUT_GAMEPAD_DPAD_LEFT;
            break;
        case WheelStep::Done:
            g_switches.fetch_add(1, std::memory_order_relaxed);
            LOGI("[wheel] %s hand: slot %d -> %d after %d press%s (%.2f s)", HandName(h), g_commitFrom[h] + 1, eq.active[h] + 1,
                 presses, presses == 1 ? "" : "es", now - g_commitStart[h]);
            break;
        case WheelStep::Failed:
            g_failed.fetch_add(1, std::memory_order_relaxed);
            LOGW("[wheel] %s hand: the game's slot stayed %d after %d press%s - it ignores weapon switches mid-action "
                 "(attacking, rolling, staggered); pick again once it is free",
                 HandName(h), eq.active[h] + 1, presses, presses == 1 ? "" : "es");
            out.hapticAmp[h] = 0.9f; out.hapticSec[h] = 0.2f;
            break;
        default: break;
        }
    }
    g_busy.store(g_commit[0].target >= 0 || g_commit[1].target >= 0, std::memory_order_relaxed);
    if (!c.wheelHaptics) for (int h = 0; h < 2; ++h) out.hapticAmp[h] = 0.f;
    Publish(now, c);
    return out;
}

void WeaponWheelGameFrame(uint8_t* player) {
    if (!Cfg().wheelEnabled) return;
    for (int h = 0; h < 2; ++h) {
        const int slot = g_jumpReq[h].exchange(-1);
        if (slot >= 0) g_jumpResult[h].store(WriteActiveSlot(player, h, slot) ? 1 : -1);
    }
    ReadEquipGuarded(player);
}

bool WeaponWheelOwnsHand(int hand) { return hand >= 0 && g_openHandShared.load(std::memory_order_relaxed) == hand; }
bool WeaponWheelBusy() { return g_busy.load(std::memory_order_relaxed); }

WheelStats WeaponWheelGetStats() {
    WheelStats s;
    s.openHand = g_openHandShared.load(std::memory_order_relaxed);
    s.opens = g_opens.exchange(0); s.switches = g_switches.exchange(0); s.failed = g_failed.exchange(0);
    return s;
}

} // namespace ervr
