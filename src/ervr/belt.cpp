#include "belt.h"
#include "belt_logic.h"
#include "body_calibration.h"
#include "body_calibration_logic.h"
#include "weapon_wheel.h"
#include "weapon_wheel_logic.h"   // WheelFrameFacing / WheelOrientation: a quad upright and facing the eyes
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
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ervr {
namespace {

constexpr int kItemPx = 256;            // each item's image, square
constexpr int kIconMaxPx = 256;         // drawn at ~155 px on the belt; held up to the eyes it fills more
constexpr double kReturnSec = 0.15;     // a let-go item flies back to its place this long
const Vec3 kHeldOffset{0.f, 0.03f, -0.05f};   // in the controller's grip frame: just past the fingers, a little up
const char* HandName(int h) { return h == HandRight ? "right" : "left"; }
uint8_t* Deref(const void* a) { uint8_t* p = nullptr; return SafeRead(a, &p, sizeof(p)) ? p : nullptr; }

// ---------------------------------------------------------------- the pouch (game thread -> worker)
// PlayerGameData: fromsoftware-rs EquipGameData at +0x2B0 (read-only reference), every offset below checked against
// the TGA cheat table - "Pouch 1" at +0x678, the inventory pointer at +0x5D0, EquipMagicData at +0x530 with its
// selected slot at +0x80.
constexpr size_t PGD_PouchItems    = 0x678;   // ChrAsmEquipEntries.pouch[6]: item ids, empty = 0xFFFFFFFF
constexpr size_t PGD_PouchSlots    = 0x590;   // EquipItemData.pouch_slots[6]: {gaitem handle, inventory index}
constexpr size_t PGD_Inventory     = 0x5D0;   // EquipItemData.inventory -> EquipInventoryData
constexpr size_t PGD_QuickSelected = 0x5D8;   // EquipItemData.selected_quick_slot
constexpr size_t PGD_MagicData     = 0x530;   // EquipGameData.equip_magic_data -> EquipMagicData
constexpr size_t Magic_Selected    = 0x80;
// EquipInventoryData: the key items and the normal items, each a list of 0x18-byte {gaitem handle, item id,
// quantity, ...} entries with a capacity and a length in use (TGA reads the lists at +0x10 / +0x20).
constexpr size_t Inv_NormalCap = 0x0C, Inv_NormalHead = 0x10, Inv_NormalLen = 0x18;
constexpr size_t Inv_KeyCap = 0x1C, Inv_KeyHead = 0x20, Inv_KeyLen = 0x28;
constexpr size_t Entry_Size = 0x18, Entry_ItemId = 0x4, Entry_Quantity = 0x8;
// ChrIns: the item the player's next UseGoods TAE event applies (fromsoftware-rs tae_queued_use_item).
constexpr size_t Chr_QueuedUseItem = 0x160;
// EQUIP_PARAM_GOODS_ST
constexpr size_t Goods_IconId = 0x30;   // u16: MENU_Knowledge_<iconId>, as for weapons
constexpr size_t Goods_MaxNum = 0x3A;   // i16: how many one stack holds (1 = no count shown)

struct Snap {
    bool valid = false;
    uint32_t seq = 0;
    BeltGameState g;
    int iconId[kBeltSlots] = {-1, -1, -1, -1};   // -1 = not resolved (params not loaded, no row)
    int maxNum[kBeltSlots] = {0, 0, 0, 0};
    bool SameAs(const Snap& o) const {
        return valid == o.valid && std::memcmp(&g, &o.g, sizeof(g)) == 0 && std::memcmp(iconId, o.iconId, sizeof(iconId)) == 0 &&
               std::memcmp(maxNum, o.maxNum, sizeof(maxNum)) == 0;
    }
};
std::mutex g_snapMtx;
Snap g_snap;

// Where each slot's item sits in the inventory: found once, re-checked by item id every frame. Game thread.
struct Found { uint32_t item = 0; uint8_t* entry = nullptr; double retryAt = 0.0; };
Found g_found[kBeltSlots];

bool EntryHolds(const uint8_t* e, uint32_t item) { uint32_t id = 0; return e && SafeRead(e + Entry_ItemId, &id, 4) && id == item; }

// The inventory entry holding `item`. The pouch slot's own index is tried first - as the game numbers its inventory
// (key items, then normal items) and as an index into either list - and failing those both lists are searched.
uint8_t* FindEntry(uint8_t* inv, uint32_t item, int32_t index, const char** how) {
    uint32_t keyCap = 0, normCap = 0;
    uint8_t* keyHead = Deref(inv + Inv_KeyHead);
    uint8_t* normHead = Deref(inv + Inv_NormalHead);
    if (!SafeRead(inv + Inv_KeyCap, &keyCap, 4) || !SafeRead(inv + Inv_NormalCap, &normCap, 4)) return nullptr;
    if (keyCap > 4096) keyCap = 0;
    if (normCap > 8192) normCap = 0;
    auto at = [](uint8_t* head, uint32_t cap, uint32_t i) -> uint8_t* { return head && i < cap ? head + (size_t)i * Entry_Size : nullptr; };
    if (index >= 0) {
        const uint32_t i = (uint32_t)index;
        uint8_t* e = i < keyCap ? at(keyHead, keyCap, i) : at(normHead, normCap, i - keyCap);
        if (EntryHolds(e, item)) { *how = "the game's numbering (key items, then normal)"; return e; }
        if (EntryHolds(e = at(normHead, normCap, i), item)) { *how = "an index into the normal items"; return e; }
        if (EntryHolds(e = at(keyHead, keyCap, i), item)) { *how = "an index into the key items"; return e; }
    }
    for (int list = 0; list < 2; ++list) {
        uint8_t* head = list ? normHead : keyHead;
        uint32_t cap = list ? normCap : keyCap, len = 0;
        SafeRead(inv + (list ? Inv_NormalLen : Inv_KeyLen), &len, 4);
        if (!head || !cap) continue;
        std::vector<uint8_t> all((size_t)cap * Entry_Size);
        // The whole capacity in one read; a list that is shorter than it claims falls back to the length in use.
        if (!SafeRead(head, all.data(), all.size())) {
            cap = std::min(len, cap);
            all.resize((size_t)cap * Entry_Size);
            if (!cap || !SafeRead(head, all.data(), all.size())) continue;
        }
        for (uint32_t i = 0; i < cap; ++i) {
            uint32_t id = 0;
            std::memcpy(&id, all.data() + (size_t)i * Entry_Size + Entry_ItemId, 4);
            if (id == item) { *how = list ? "a search of the normal items" : "a search of the key items"; return head + (size_t)i * Entry_Size; }
        }
    }
    return nullptr;
}

int ReadQuantity(int k, uint8_t* inv, uint32_t item, int32_t index, double now) {
    Found& f = g_found[k];
    if (f.item != item) { f = Found{}; f.item = item; }
    if (!EntryHolds(f.entry, item)) {
        f.entry = nullptr;
        if (!inv || now < f.retryAt) return -1;
        const char* how = "";
        f.entry = FindEntry(inv, item, index, &how);
        if (!f.entry) { f.retryAt = now + 2.0; return -1; }
        static bool s_said = false;
        if (!s_said) { s_said = true; LOGI("[belt] pouch counts: the item was found by %s", how); }
    }
    int32_t q = -1;
    return SafeRead(f.entry + Entry_Quantity, &q, 4) && q >= 0 && q < 100000 ? q : -1;
}

double NowSeconds() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

void ReadPouch(uint8_t* player) {
    Snap s;
    uint8_t* pgd = GameERPlayerGameData();
    uint32_t items[kBeltSlots] = {};
    if (player && pgd && SafeRead(pgd + PGD_PouchItems, items, sizeof(items))) {
        s.valid = true;
        const double now = NowSeconds();
        int32_t slots[kBeltSlots * 2];
        for (int32_t& v : slots) v = -1;
        SafeRead(pgd + PGD_PouchSlots, slots, sizeof(slots));
        uint8_t* inv = Deref(pgd + PGD_Inventory);
        int32_t v = -1;
        if (SafeRead(pgd + PGD_QuickSelected, &v, 4)) s.g.quickSlot = v;
        if (uint8_t* magic = Deref(pgd + PGD_MagicData); magic && SafeRead(magic + Magic_Selected, &v, 4)) s.g.spellSlot = v;
        if (uint8_t* asmp = Deref(player + Player_ChrAsm)) {
            if (SafeRead(asmp + Asm_LeftSlot, &v, 4)) s.g.weaponL = v;
            if (SafeRead(asmp + Asm_RightSlot, &v, 4)) s.g.weaponR = v;
        }
        SafeRead(player + Chr_QueuedUseItem, &s.g.queuedItem, 4);
        // EquipParamGoods row -> (icon id, stack size), once per item; checked to be that param first.
        static std::unordered_map<uint32_t, std::pair<int, int>> s_goods;
        static int s_goodsOk = -1;
        if (s_goodsOk < 0 && ParamsReady()) {
            s_goodsOk = std::strcmp(ParamsStructName(PI_EquipParamGoods), "EQUIP_PARAM_GOODS_ST") == 0 ? 1 : 0;
            if (!s_goodsOk) LOGW("[belt] param holder %d is %s, not EQUIP_PARAM_GOODS_ST: the belt shows slot numbers instead of icons",
                                 (int)PI_EquipParamGoods, ParamsStructName(PI_EquipParamGoods));
        }
        for (int k = 0; k < kBeltSlots; ++k) {
            s.g.pouch[k] = items[k];
            if (!BeltItemValid(items[k])) { g_found[k] = Found{}; continue; }
            s.g.quantity[k] = ReadQuantity(k, inv, items[k], slots[k * 2 + 1], now);
            const uint32_t goods = BeltGoodsId(items[k]);
            auto it = s_goods.find(goods);
            if (it == s_goods.end() && s_goodsOk == 1) {
                uint8_t* row = ParamsRow(PI_EquipParamGoods, goods);
                uint16_t icon = 0; int16_t maxNum = 0;
                if (row) { SafeRead(row + Goods_IconId, &icon, 2); SafeRead(row + Goods_MaxNum, &maxNum, 2); }
                it = s_goods.emplace(goods, std::make_pair(row ? (int)icon : -1, (int)maxNum)).first;
            }
            if (it != s_goods.end()) { s.iconId[k] = it->second.first; s.maxNum[k] = it->second.second; }
        }
    }
    std::lock_guard<std::mutex> g(g_snapMtx);
    if (!s.SameAs(g_snap)) { s.seq = g_snap.seq + 1; g_snap = s; }
}
// SafeRead never faults; this is for the unexpected, and nothing in ReadPouch holds a lock while it reads game memory.
void ReadPouchGuarded(uint8_t* player) {
    __try { ReadPouch(player); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------- the belt (worker)
struct HandState {
    int held = -1;            // the slot this hand holds
    int hover = -1;           // the slot within reach
    bool gripOpen = true;     // the grip opened since it last closed: taking an item needs a fresh squeeze
    double heldSince = 0.0;
};
HandState g_hand[2];
std::atomic<int> g_heldShared[2] = {-1, -1};   // worker -> combat / the wheel
std::atomic<bool> g_busyShared{false};
std::atomic<int> g_itemsShared{0};

enum Look { LookIdle = 0, LookNear, LookHeld, LookUsing };
struct ItemState {
    double returnStart = -1.0;   // let go: flying back from returnFrom
    Vec3 returnFrom;
    int look = -1, iconId = -2, qty = -2;   // what its image shows
    bool iconMissing = false;
    double nextIconLook = 0.0;
};
ItemState g_item[kBeltSlots];
BeltYaw g_yaw;
double g_lastNow = 0.0;

// The ini's directions and angles, parsed when they change. A direction the game itself corrected (a use that took
// another slot's item) stays corrected until the ini changes.
uint8_t g_dir[kBeltSlots] = {BeltUp, BeltRight, BeltLeft, BeltDown};
float g_angle[kBeltSlots] = {-30.f, 30.f, -80.f, 80.f};
std::string g_dirSrc = "\x01", g_angleSrc = "\x01";

BeltChord g_chord;
bool g_usePrev = false, g_useTaken = false;   // the use button went down on a held item: the belt's until released
struct Wait { int slot = -1; double since = 0.0; } g_wait;   // a use waiting for the weapon wheel's switch to finish
struct Watch {
    bool on = false;
    int slot = -1;
    uint32_t item = 0;
    BeltGameState before;
    double start = 0.0, switchUntil = 0.0, until = 0.0;
} g_watch;
std::atomic<long> g_grabs{0}, g_uses{0}, g_confirmed{0}, g_refused{0};

uint32_t VRButtonOf(const std::string& n) {
    if (n == "A") return VRB_A; if (n == "B") return VRB_B; if (n == "X") return VRB_X; if (n == "Y") return VRB_Y;
    return 0;
}
uint16_t DirMask(uint8_t d) {
    return d == BeltUp ? XINPUT_GAMEPAD_DPAD_UP : d == BeltRight ? XINPUT_GAMEPAD_DPAD_RIGHT : d == BeltLeft ? XINPUT_GAMEPAD_DPAD_LEFT : XINPUT_GAMEPAD_DPAD_DOWN;
}
bool HandAllowed(const std::string& hands, int h) {
    if (hands == "off" || hands == "none") return false;
    if (hands == "right") return h == HandRight;
    if (hands == "left") return h == HandLeft;
    return true;
}
Vec3 HeldPos(const Pose& hand) { return hand.pos + hand.rot.Rotate(kHeldOffset); }
std::string DirList() {
    std::string s;
    for (int k = 0; k < kBeltSlots; ++k) { if (k) s += ','; s += BeltDirName(g_dir[k]); }
    return s;
}
BeltChordTiming Timing(const Config& c) {
    BeltChordTiming t;
    t.lead = std::max((double)c.beltChordLead, 0.05);
    t.press = std::max((double)c.beltChordPress, 0.05);
    t.tail = std::max((double)c.beltChordTail, 0.02);
    return t;
}

// An item: a dark disc with the gold rims of ELDEN RING's menus, the game's icon, and its count when it stacks.
void Render(int k, int iconId, int qty, int look, bool& missing) {
    const Config& c = Cfg();
    const float S = (float)kItemPx, C = S * 0.5f, r = 0.44f * S;
    const bool lit = look != LookIdle;
    Canvas cv; cv.Reset(kItemPx);
    if (lit) cv.Glow(C, C, r, 10.f, Col(kGoldHi, look == LookUsing ? 0.75f : 0.45f));
    cv.Vignette(C, C, r, Col(lit ? kDarkWarm : kDark, 0.84f), Col(kDark, 0.64f));
    cv.Ring(C, C, r - 2.f, lit ? 5.f : 3.f, lit ? Col(kGoldHi, 1.f) : Col(kGold, 0.85f));
    cv.Ring(C, C, r - 10.f, 1.2f, Col(kGold, 0.30f));
    if (look == LookUsing) cv.Ring(C, C, r - 17.f, 2.5f, Col(kGoldHi, 0.9f), 20);   // the shortcut is being pressed
    missing = false;
    if (const Icon* ic = CanvasIcon(iconId, kIconMaxPx)) cv.Image(*ic, C, C, 1.40f * r, 1.f);
    else { cv.Number(k + 1, C + 0.3f * 0.62f * r, C, 0.62f * r, Col(kParchment, 0.9f)); missing = true; }
    if (qty >= 0) {   // the count, bottom right, on a dark pill
        const float h = 0.30f * r, right = C + 0.78f * r, cy = C + 0.70f * r;
        int digits = 1;
        for (int v = qty; v >= 10; v /= 10) ++digits;
        const float width = digits * 0.6f * h + (digits - 1) * 0.32f * h, pad = 0.3f * h, cap = 0.85f * h;
        float x0 = right - width - pad + cap, x1 = right + pad - cap;
        if (x1 < x0) x0 = x1 = 0.5f * (x0 + x1);
        cv.Stroke(x0, cy, x1, cy, 2.f * cap, Col(kDark, 0.88f));
        cv.Number(qty, right, cy, h, Col(qty ? kParchment : 0x9A8F7A, 1.f));
    }
    std::vector<uint32_t> img;
    cv.EncodeSrgb(img, look == LookIdle ? Clamp(c.beltOpacity, 0.f, 1.f) : 1.f);
    OverlayPublishImage(OverlayBelt0 + k, kItemPx, img);
}

void StartUse(int k, int hand, double now, const Snap& snap, const Config& c, BeltPadOut& out) {
    const BeltChordTiming t = Timing(c);
    g_chord.Start(k, now);
    g_watch = Watch{};
    g_watch.on = true; g_watch.slot = k; g_watch.item = snap.g.pouch[k]; g_watch.before = snap.g; g_watch.start = now;
    g_watch.switchUntil = now + t.lead + t.press + 0.35;   // a plain D-pad switch shows within a few frames of the press
    g_watch.until = now + t.lead + t.press + t.tail + 2.0;
    g_uses.fetch_add(1, std::memory_order_relaxed);
    char count[24] = "";
    if (snap.g.quantity[k] >= 0) snprintf(count, sizeof(count), ", %d left", snap.g.quantity[k]);
    LOGI("[belt] %s hand uses slot %d (item 0x%08X%s): %s held, D-pad %s", HandName(hand), k + 1, snap.g.pouch[k], count,
         XInputButtonName(ParseXInputButton(c.beltPouchButton)), BeltDirName(g_dir[k]));
    out.hapticAmp[hand] = 0.5f; out.hapticSec[hand] = 0.04f;
}

} // namespace

BeltPadOut BeltUpdate(const VRTracking& tr, const VRInputState& in, double now, bool menu) {
    const Config& c = Cfg();
    BeltPadOut out;
    CanvasInit();
    if (c.beltEnabled) {
        static bool s_started = false;
        if (!s_started) {
            s_started = true;
            ParamsInit();   // the icons come from EquipParamGoods; the wheel or combat usually did this already
            LOGI("[belt] pouch slots 1-4 hang at the waist (%.2f m below the eyes, %.2f m out, hands: %s): close a grip on one to take it, "
                 "%s to use it (the game's %s + D-pad), open the hand to put it back",
                 c.beltDrop, c.beltRadius, c.beltHands.c_str(), c.beltUseButton.c_str(), c.beltPouchButton.c_str());
        }
    }
    if (c.beltDirections != g_dirSrc) {
        g_dirSrc = c.beltDirections;
        uint8_t d[kBeltSlots] = {BeltUp, BeltRight, BeltLeft, BeltDown};
        if (!BeltParseDirections(g_dirSrc, d))
            LOGW("[belt] Directions=%s is not four different directions out of up/right/left/down - using up,right,left,down", g_dirSrc.c_str());
        std::memcpy(g_dir, d, sizeof(d));
    }
    if (c.beltAngles != g_angleSrc) {
        g_angleSrc = c.beltAngles;
        float a[kBeltSlots] = {-30.f, 30.f, -80.f, 80.f};
        if (!BeltParseAngles(g_angleSrc, a)) LOGW("[belt] Angles=%s is not four angles in degrees - using -30,30,-80,80", g_angleSrc.c_str());
        std::memcpy(g_angle, a, sizeof(a));
    }
    const uint32_t useMask = VRButtonOf(c.beltUseButton);
    const uint16_t modMask = ParseXInputButton(c.beltPouchButton);
    const BeltChordTiming timing = Timing(c);

    Snap snap; { std::lock_guard<std::mutex> g(g_snapMtx); snap = g_snap; }
    const bool active = c.beltEnabled && snap.valid && tr.head.valid && !menu && GameERPlayerActive() && !GameERTransitionActive();
    bool filled[kBeltSlots];
    int items = 0;
    for (int k = 0; k < kBeltSlots; ++k) { filled[k] = active && BeltItemValid(snap.g.pouch[k]); items += filled[k] ? 1 : 0; }
    g_itemsShared.store(items, std::memory_order_relaxed);
    // A new pouch: read its icons now, not when a hand first reaches for it, and say what the belt holds.
    static uint32_t s_logged[kBeltSlots] = {0, 0, 0, 0};
    if (c.beltEnabled && snap.valid && std::memcmp(s_logged, snap.g.pouch, sizeof(s_logged)) != 0) {
        std::memcpy(s_logged, snap.g.pouch, sizeof(s_logged));
        char list[320]; int n = 0;
        for (int k = 0; k < kBeltSlots && n >= 0 && n < (int)sizeof(list); ++k) {
            if (!BeltItemValid(snap.g.pouch[k])) { n += snprintf(list + n, sizeof(list) - n, "%s %d empty", k ? "," : "", k + 1); continue; }
            CanvasIcon(snap.iconId[k], kIconMaxPx);
            n += snprintf(list + n, sizeof(list) - n, "%s %d item 0x%08X icon %d count %d", k ? "," : "", k + 1, snap.g.pouch[k],
                          snap.iconId[k], snap.g.quantity[k]);
        }
        LOGI("[belt] pouch:%s (D-pad %s)", list, DirList().c_str());
    }

    // ---- where the belt hangs: the yaw the hands reach against, frozen while they reach
    const double dt = g_lastNow > 0.0 ? Clamp((float)(now - g_lastNow), 0.f, 0.1f) : 0.0;
    g_lastNow = now;
    const Vec3 neck = BeltNeck(tr.head);
    // [Belt] Drop is for a 1.76 m player; with a body calibration the belt hangs at your own waist.
    const float eye = c.bodyFitBelt ? BodyEyeHeight() : 0.f;
    const float radius = Clamp(c.beltRadius, 0.05f, 0.6f), drop = Clamp(CalibBeltDrop(c.beltDrop, eye), 0.1f, 1.5f);
    {
        static float s_said = -1.f;
        if (std::fabs(drop - s_said) > 0.005f) {
            s_said = drop;
            if (eye > 0.f) LOGI("[belt] hangs %.2f m below the eyes: Drop %.2f for a 1.76 m player, fitted to your calibrated %.2f m", drop, c.beltDrop, CalibHeight(eye));
            else LOGD("[belt] hangs %.2f m below the eyes (Drop; no body calibration%s)", drop, c.bodyFitBelt ? "" : " used: [Body] FitBelt=false");
        }
    }
    const float grabR = Clamp(c.beltGrabRadius, 0.03f, 0.3f);
    Vec3 pos[kBeltSlots];
    auto place = [&](float yaw) { for (int k = 0; k < kBeltSlots; ++k) pos[k] = BeltSlotPos(neck, yaw, Deg2Rad(g_angle[k]), radius, drop); };
    place(g_yaw.valid ? g_yaw.yaw : tr.head.rot.Yaw());
    bool reaching = false;
    for (int h = 0; h < 2; ++h)
        for (int k = 0; k < kBeltSlots; ++k)
            if (filled[k] && tr.hand[h].valid && (tr.hand[h].pos - pos[k]).Length() < 2.5f * grabR) reaching = true;
    if (tr.head.valid) {
        const bool freeze = reaching || g_hand[0].held >= 0 || g_hand[1].held >= 0 || tr.head.rot.Pitch() < Deg2Rad(-35.f);
        g_yaw.Update(tr.head.rot.Yaw(), now, dt, Deg2Rad(Clamp(c.beltFollowDeg, 5.f, 120.f)), freeze);
        place(g_yaw.yaw);
    }

    // ---- the hands: a fresh squeeze at an item takes it, an open hand puts it back
    for (int h = 0; h < 2; ++h) {
        HandState& s = g_hand[h];
        const float grip = in.handTrigger[h];
        const bool down = grip >= c.gripThreshold, open = grip < 0.5f * c.gripThreshold;
        const bool tracked = tr.hand[h].valid && in.controllerConnected[h];
        if (s.held >= 0) {
            const int k = s.held;
            const char* why = open ? nullptr : !active ? "the belt went away (a menu, a load)" : !filled[k] ? "it is gone from the pouch" : nullptr;
            if (open || why) {
                g_item[k].returnStart = now;
                g_item[k].returnFrom = tracked ? HeldPos(tr.hand[h]) : pos[k];
                s.held = -1;
                if (why) LOGI("[belt] %s hand let go of slot %d: %s", HandName(h), k + 1, why);
                else LOGD("[belt] %s hand put slot %d back", HandName(h), k + 1);
                if (open) { out.hapticAmp[h] = 0.15f; out.hapticSec[h] = 0.02f; }
            }
        }
        if (s.held < 0) {
            bool taken[kBeltSlots] = {};
            if (g_hand[1 - h].held >= 0) taken[g_hand[1 - h].held] = true;
            const int at = active && tracked && HandAllowed(c.beltHands, h) ? BeltNearest(tr.hand[h].pos, pos, filled, taken, grabR) : -1;
            if (at != s.hover) {
                s.hover = at;
                if (at >= 0 && !down) { out.hapticAmp[h] = 0.12f; out.hapticSec[h] = 0.012f; }   // a touch: the item is under the hand
            }
            // A grip already closed (a weapon wheel, a guard, a hilt) passing by takes nothing.
            if (at >= 0 && down && s.gripOpen) {
                s.held = at; s.hover = -1; s.heldSince = now;
                g_item[at].returnStart = -1.0;
                g_grabs.fetch_add(1, std::memory_order_relaxed);
                char count[24] = "";
                if (snap.g.quantity[at] >= 0) snprintf(count, sizeof(count), ", %d left", snap.g.quantity[at]);
                LOGI("[belt] %s hand took slot %d (item 0x%08X%s)", HandName(h), at + 1, snap.g.pouch[at], count);
                out.hapticAmp[h] = 0.4f; out.hapticSec[h] = 0.035f;
            }
        } else s.hover = -1;
        if (down) s.gripOpen = false;
        else if (open) s.gripOpen = true;
        out.holdGrip[h] = s.held >= 0;
        g_heldShared[h].store(s.held, std::memory_order_relaxed);
    }

    // ---- the use button: on a held item it is the belt's until released; with nothing in hand it is untouched
    const bool useDown = useMask && (in.buttons & useMask);
    int user = -1;   // a press uses the item taken last
    for (int h = 0; h < 2; ++h)
        if (g_hand[h].held >= 0 && (user < 0 || g_hand[h].heldSince > g_hand[user].heldSince)) user = h;
    if (useDown && !g_usePrev && user >= 0) {
        g_useTaken = true;
        const int k = g_hand[user].held;
        if (g_chord.Active()) {
            LOGD("[belt] slot %d: still pressing the shortcut for slot %d", k + 1, g_chord.slot + 1);
            out.hapticAmp[user] = 0.25f; out.hapticSec[user] = 0.03f;
        } else if (WeaponWheelBusy()) {
            g_wait.slot = k; g_wait.since = now;   // the wheel is pressing the D-pad: right after it
        } else StartUse(k, user, now, snap, c, out);
    }
    if (!useDown) g_useTaken = false;
    g_usePrev = useDown;
    if (g_useTaken) out.swallow |= useMask;
    if (g_wait.slot >= 0) {
        if (!active || now - g_wait.since > 1.5) {
            LOGI("[belt] use of slot %d dropped: %s", g_wait.slot + 1, active ? "the weapon wheel's switch took longer than 1.5 s" : "the belt went away");
            g_wait.slot = -1;
        } else if (!WeaponWheelBusy() && !g_chord.Active()) {
            int hand = user >= 0 ? user : HandLeft;
            StartUse(g_wait.slot, hand, now, snap, c, out);
            g_wait.slot = -1;
        }
    }

    // ---- the shortcut itself, then what the game made of it
    if (g_chord.Active()) {
        if (!active) {
            LOGI("[belt] use of slot %d abandoned: %s", g_chord.slot + 1, menu ? "a menu opened" : "left the world");
            g_chord = BeltChord{};
            g_watch.on = false;
        } else {
            const int k = g_chord.slot;
            const uint8_t b = BeltChordStep(g_chord, now, timing);
            if (b & BeltChordMod) out.buttons |= modMask;
            if (b & BeltChordDir) out.buttons |= DirMask(g_dir[k]);
        }
    }
    g_busyShared.store(g_chord.Active(), std::memory_order_relaxed);
    if (g_watch.on && snap.valid) {
        const int k = g_watch.slot;
        const BeltVerdict v = BeltJudge(g_watch.before, snap.g, k, now <= g_watch.switchUntil, now >= g_watch.until);
        const BeltGameState& b = g_watch.before;
        switch (v.outcome) {
        case BeltOutcome::Pending:
            break;
        case BeltOutcome::Used:
            g_confirmed.fetch_add(1, std::memory_order_relaxed);
            LOGI("[belt] slot %d: the game used item 0x%08X %.2f s after the press (count %d -> %d, queued 0x%08X -> 0x%08X)",
                 k + 1, g_watch.item, now - g_watch.start, b.quantity[k], snap.g.pouch[k] == g_watch.item ? snap.g.quantity[k] : 0,
                 b.queuedItem, snap.g.queuedItem);
            g_watch.on = false;
            break;
        case BeltOutcome::OtherSlot: {
            g_refused.fetch_add(1, std::memory_order_relaxed);
            const uint8_t sent = g_dir[k];
            BeltSwapDirections(g_dir, k, v.otherSlot);
            LOGW("[belt] D-pad %s used pouch slot %d (item 0x%08X), not slot %d: the game's directions differ from [Belt] Directions. "
                 "Swapped for this session - set Directions=%s to keep it",
                 BeltDirName(sent), v.otherSlot + 1, b.pouch[v.otherSlot], k + 1, DirList().c_str());
            for (int h = 0; h < 2; ++h) { out.hapticAmp[h] = 0.9f; out.hapticSec[h] = 0.2f; }
            g_watch.on = false;
            break;
        }
        case BeltOutcome::Switched:
            g_refused.fetch_add(1, std::memory_order_relaxed);
            LOGW("[belt] slot %d: the D-pad press reached the game as a plain %s switch - it did not see %s held yet. "
                 "Raise [Belt] ChordLead (now %.2f s)", k + 1, v.switched, XInputButtonName(modMask), timing.lead);
            for (int h = 0; h < 2; ++h) { out.hapticAmp[h] = 0.9f; out.hapticSec[h] = 0.2f; }
            g_watch.on = false;
            break;
        case BeltOutcome::Unconfirmed:
            LOGI("[belt] slot %d: no sign of the game using item 0x%08X within %.1f s (count %d -> %d, queued 0x%08X -> 0x%08X) - "
                 "it ignores items mid-attack, mid-roll or staggered, and an item without a count leaves nothing to see",
                 k + 1, g_watch.item, now - g_watch.start, b.quantity[k], snap.g.quantity[k], b.queuedItem, snap.g.queuedItem);
            g_watch.on = false;
            break;
        }
    }

    // ---- what each item looks like and where it hangs
    const float size = Clamp(c.beltSize, 0.03f, 0.3f);
    for (int k = 0; k < kBeltSlots; ++k) {
        ItemState& it = g_item[k];
        const int holder = g_hand[0].held == k ? HandLeft : g_hand[1].held == k ? HandRight : -1;
        const bool reached = g_hand[0].hover == k || g_hand[1].hover == k;
        if (filled[k]) {
            const int look = g_chord.Active() && g_chord.slot == k ? LookUsing : holder >= 0 ? LookHeld : reached ? LookNear : LookIdle;
            const int qty = snap.maxNum[k] > 1 ? snap.g.quantity[k] : -1;
            if (look != it.look || snap.iconId[k] != it.iconId || qty != it.qty || (it.iconMissing && now >= it.nextIconLook)) {
                it.look = look; it.iconId = snap.iconId[k]; it.qty = qty;
                Render(k, it.iconId, qty, look, it.iconMissing);
                if (it.iconMissing) it.nextIconLook = now + 0.25;   // an icon the archive reader finishes later
            }
        }
        Pose p;
        p.valid = true;
        if (holder >= 0 && tr.hand[holder].valid) p.pos = HeldPos(tr.hand[holder]);
        else if (it.returnStart >= 0.0 && now - it.returnStart < kReturnSec) {
            const float t = Clamp((float)((now - it.returnStart) / kReturnSec), 0.f, 1.f), e = t * t * (3.f - 2.f * t);
            p.pos = it.returnFrom + (pos[k] - it.returnFrom) * e;
        } else {
            it.returnStart = -1.0;
            p.pos = pos[k];
        }
        p.rot = WheelOrientation(WheelFrameFacing(p.pos, tr.head.pos, tr.head.rot.Up()));
        OverlayPublishPose(OverlayBelt0 + k, filled[k], p, size);
    }
    if (!c.beltHaptics) for (int h = 0; h < 2; ++h) out.hapticAmp[h] = 0.f;
    return out;
}

void BeltGameFrame(uint8_t* player) {
    if (!Cfg().beltEnabled) return;
    ReadPouchGuarded(player);
}

bool BeltOwnsHand(int hand) { return (hand == HandLeft || hand == HandRight) && g_heldShared[hand].load(std::memory_order_relaxed) >= 0; }
bool BeltPadBusy() { return g_busyShared.load(std::memory_order_relaxed); }

BeltStats BeltGetStats() {
    BeltStats s;
    for (int h = 0; h < 2; ++h) s.held[h] = g_heldShared[h].load(std::memory_order_relaxed);
    s.items = g_itemsShared.load(std::memory_order_relaxed);
    s.grabs = g_grabs.exchange(0); s.uses = g_uses.exchange(0); s.confirmed = g_confirmed.exchange(0); s.refused = g_refused.exchange(0);
    return s;
}

} // namespace ervr
