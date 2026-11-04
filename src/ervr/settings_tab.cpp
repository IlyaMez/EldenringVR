#include "settings_tab.h"
#include "settings_tab_logic.h"
#include "body_calibration.h"
#include "config.h"
#include "game_memory.h"
#include "game_version.h"
#include "ini_file.h"
#include "log.h"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace ervr {
namespace {

// ---- exe 2.7.2.0 -----------------------------------------------------------------------------------------
// Found from the executable's RTTI and the Settings dialog's constructor (0x967770); docs/DEVELOPMENT.md has
// the walk. Each function is checked against its first bytes, each vtable against its RTTI name, before
// anything is hooked or called.
struct GameFn {
    const char* name;
    uint32_t rva;
    uint8_t head[12];
};
enum : int { fnTabPush, fnBrightnessPage, fnGridScroll, fnSetPage, fnBlockHeap, fnTabCtor, fnTabDtor, fnMenuString,
             fnCreatePage, fnOffOnList, fnOffOnRow, fnListRow, fnListPush, fnLabelDtor, fnTabIcon, fnFindChild,
             fnProxyValid, fnProxyCopy, fnProxyRelease, fnCount };
const GameFn kFns[fnCount] = {
    // BasicViewItemList<MenuOptionCategory, 10>::push_back(storage, tab): the dialog's constructor, 9 calls
    {"tab push", 0x9691a0, {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe}},
    // the page factory of kind 4 (window list, option data, flag): the console Brightness page
    {"brightness page", 0x93cf40, {0x40, 0x53, 0x56, 0x57, 0x48, 0x81, 0xec, 0x20, 0x01, 0x00, 0x00, 0x48}},
    // GridControl::SetScroll(grid, const int xy[2]): the tab bar's cursor grid scrolls
    {"grid scroll", 0x73d070, {0x40, 0x55, 0x53, 0x41, 0x56, 0x48, 0x8b, 0xec, 0x48, 0x83, 0xec, 0x60}},
    // CompositeOptionSettingDialog: show the page of a kind (composite, kind), made by its factory the first time
    {"set page", 0x93cdb0, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48}},
    // the heap a block came from (block): how the composite frees its pages
    {"block heap", 0xe1d770, {0x4c, 0x63, 0x05, 0xb1, 0x0c, 0xf7, 0x02, 0x33, 0xd2, 0x4d, 0x85, 0xc0}},
    // MenuOptionCategory(this, MenuString&& name, kind, const icon*): takes the name's string
    {"tab ctor", 0x86a550, {0x4c, 0x8b, 0xdc, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x50, 0x49}},
    {"tab dtor", 0x86a640, {0x48, 0x89, 0x4c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7}},
    // MenuString(this, const wchar_t* text): the text by pointer and an empty string of its own
    {"menu string", 0x676ba0, {0x48, 0x89, 0x4c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7}},
    // a page: (window list, tab, option data, flag, window, row builder, after the rows, 1, 0)
    {"create page", 0x9601e0, {0x48, 0x8b, 0xc4, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}},
    // the game's OFF and ON (ComboItemList<bool, 2>)
    {"off/on list", 0x955a00, {0x48, 0x8b, 0xc4, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x68, 0xa1}},
    // a row: (page, label, value*, choices, default*, enabled)
    {"off/on row", 0x94a5f0, {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41}},
    {"list row", 0x94a470, {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41}},
    // ComboItemList<unsigned char, 32>: its storage's push_back(storage, item)
    {"list push", 0x86b4c0, {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe}},
    // a row's label: the title and help MenuStrings
    {"label dtor", 0x743f90, {0x48, 0x89, 0x4c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7}},
    // the dialog's tab icon binder: (&dialog, icon slot, tab index), shows or hides the slot; takes the slot
    {"tab icon", 0x968290, {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x50, 0x10, 0x57, 0x48, 0x81, 0xec, 0x90}},
    // SceneObjProxy: a child by its path (parent, out, path), whether it exists, a copy, its value's release
    {"find child", 0x74b5f0, {0x4c, 0x89, 0x44, 0x24, 0x18, 0x4c, 0x89, 0x4c, 0x24, 0x20, 0x55, 0x53}},
    {"proxy valid", 0x734450, {0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x01, 0xff, 0x10, 0xf6, 0x40, 0x20}},
    {"proxy copy", 0x74ba60, {0x48, 0x89, 0x4c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7}},
    {"proxy release", 0xd82cf0, {0x48, 0x89, 0x4c, 0x24, 0x08, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7}},
};
struct GameVt {
    const char* rtti;
    uint32_t rva;
};
enum : int { vtDialog, vtComposite, vtTabList, vtList, vtListItem, vtCount };
const GameVt kVts[vtCount] = {
    {".?AVOptionSettingTopDialog@CS@@", 0x2b18c38},
    {".?AVCompositeOptionSettingDialog@CS@@", 0x2b0ea40},
    {".?AV?$BasicViewItemList@VMenuOptionCategory@CS@@$09@CS@@", 0x2b18c00},
    {".?AV?$ComboItemList@E$0CA@@CS@@", 0x2b16c30},
    {".?AV?$ComboItem@E@CS@@", 0x2ad91a0},
};

// Layouts, from the constructors above and their callers.
constexpr size_t kMenuStringBytes = 0x38;     // the text pointer, then a DLString
constexpr size_t kTabBytes = 0x88;
constexpr size_t kTabKind = 0x40;             // int: the page the tab opens
constexpr size_t kTabsCount = 0x558;          // the storage: 10 tabs, then their count
constexpr size_t kTabsInGame = 0x1898 - 0x1208;   // storage -> the dialog's in-game byte (it adds Quit Game)
constexpr size_t kTabsMax = 10;
// The dialog: its tabs' storage, the tab bar's cursor grid (TabList, a CS::GridControl, its column scroll at +0xe0)
// and the root its scene objects are found from.
constexpr size_t kDialogTabs = 0x1208, kDialogTabList = 0xa38, kDialogRoot = 0x120, kGridScrollX = 0xe0;
constexpr size_t kProxyValue = 0x28, kProxyBytes = 0x100;
// The composite under the tabs: a page per kind (+0x68, 10 of them), the one shown (+0xb8).
constexpr size_t kCompositePages = 0x68, kCompositeCurrent = 0xb8;
// The page's window. The layout (menu/win/02_040_optionsetting.gfx) has no spare one: its windows are the
// tabs' own (and BrightnessSetting, which is no list), with fixed rows. ERVR's page uses Sound's, AudioSetting
// (window 5, 12 rows, Sound fills 4), and the two pages never live at once: opening one deletes the other,
// which the game makes again, fresh, the next time its tab opens.
constexpr uint8_t kWindowAudio = 5;
constexpr int kKindSound = 3;
constexpr size_t kOffOnBytes = 0xa8, kOffOnCount = 0xa0;
constexpr size_t kListBytes = 0x918, kListCount = 0x910;
constexpr size_t kItemBytes = 0x48, kItemValue = 8, kItemText = 0x10;
constexpr int kKindBrightness = 4, kKindGraphics = 8, kKindQuitGame = 9;
const wchar_t* const kTabName = L"ERVR Settings";

using TabPushFn = void*(__fastcall*)(uint8_t* storage, const uint8_t* tab);
using PageFn = void*(__fastcall*)(void* windowList, void* data, uint8_t flag);
using TabCtorFn = void*(__fastcall*)(uint8_t* self, uint8_t* name, int kind, const uint8_t* icon);
using SelfFn = void*(__fastcall*)(uint8_t* self);
using MenuStringFn = void*(__fastcall*)(uint8_t* self, const wchar_t* text);
struct Pmf {   // a pointer to member function as MSVC passes one with an adjustor
    void* fn;
    int32_t adjust, pad;
};
using CreatePageFn = void*(__fastcall*)(void* windowList, uint8_t* tab, void* data, uint8_t flag, uint8_t window,
                                        const Pmf* rows, const Pmf* afterRows, uint8_t keyGuide, uint8_t title);
using RowFn = void*(__fastcall*)(void* page, uint8_t* label, uint8_t* value, uint8_t* choices, uint8_t* fallback,
                                 uint8_t enabled);
using ItemFn = void*(__fastcall*)(uint8_t* self, const uint8_t* item);
using ItemDtorFn = void*(__fastcall*)(uint8_t* self, uint32_t flags);
using GridScrollFn = void(__fastcall*)(uint8_t* grid, const int32_t* xy);
using TabIconFn = void(__fastcall*)(void* dialogRef, uint8_t* slot, int32_t tab);
using FindChildFn = void*(__fastcall*)(uint8_t* parent, uint8_t* out, const char* path);
using ProxyValidFn = bool(__fastcall*)(uint8_t* proxy);
using ProxyCopyFn = void*(__fastcall*)(uint8_t* out, const uint8_t* proxy);
using SetPageFn = void(__fastcall*)(uint8_t* composite, int kind);
using BlockHeapFn = void*(__fastcall*)(void* block);

uint8_t* g_base = nullptr;
TabPushFn g_origTabPush = nullptr;
PageFn g_origPage = nullptr;
GridScrollFn g_origGridScroll = nullptr;
SetPageFn g_origSetPage = nullptr;
std::atomic<bool> g_live{false};
std::atomic<bool> g_failed{false};     // a fault in the game's code: the Settings go on without the tab
std::atomic<bool> g_tabAdded{false};   // ERVR's tab went into a Settings dialog: kind 4 is ours
std::atomic<long> g_tabsAdded{0}, g_pagesBuilt{0};

template <class F> F Game(int fn) { return reinterpret_cast<F>(g_base + kFns[fn].rva); }

uintptr_t Vt(int vt) { return reinterpret_cast<uintptr_t>(g_base + kVts[vt].rva); }

// The rows: the bytes the game edits, what ERVR.ini says for each, and what the page was built from.
SRWLOCK g_lock = SRWLOCK_INIT;
std::string g_ini;
std::vector<TabShown> g_shown;
alignas(16) volatile uint8_t g_value[kTabMaxRows];
alignas(16) uint8_t g_default[kTabMaxRows];
uint8_t g_written[kTabMaxRows];
// Labels of values a list does not offer: never freed, the game keeps their pointers.
std::set<std::wstring> g_texts;

const wchar_t* KeptText(const std::wstring& s) {
    AcquireSRWLockExclusive(&g_lock);
    const wchar_t* p = g_texts.insert(s).first->c_str();
    ReleaseSRWLockExclusive(&g_lock);
    return p;
}

void DestroyItems(uint8_t* list, size_t countAt) {
    const size_t count = *reinterpret_cast<size_t*>(list + countAt);
    for (size_t k = 0; k < count; ++k) {
        uint8_t* item = list + 8 + k * kItemBytes;
        reinterpret_cast<ItemDtorFn>((*reinterpret_cast<void***>(item))[0])(item, 0);
    }
}

void AddRow(void* page, const TabRow& row, const TabShown& s, size_t i) {
    alignas(16) uint8_t label[2 * kMenuStringBytes];
    Game<MenuStringFn>(fnMenuString)(label, row.title);
    Game<MenuStringFn>(fnMenuString)(label + kMenuStringBytes, row.help);
    uint8_t* value = const_cast<uint8_t*>(&g_value[i]);
    if (row.kind == TabRowKind::onOff) {
        alignas(16) uint8_t choices[kOffOnBytes + 8];
        Game<SelfFn>(fnOffOnList)(choices);
        Game<RowFn>(fnOffOnRow)(page, label, value, choices, &g_default[i], 1);
        DestroyItems(choices, kOffOnCount);
    } else {
        alignas(16) uint8_t choices[kListBytes];
        std::memset(choices, 0, sizeof(choices));
        *reinterpret_cast<uintptr_t*>(choices) = Vt(vtList);
        for (size_t k = 0; k < s.labels.size() && k < (size_t)kTabMaxChoices; ++k) {
            alignas(16) uint8_t item[kItemBytes] = {};
            *reinterpret_cast<uintptr_t*>(item) = Vt(vtListItem);
            item[kItemValue] = static_cast<uint8_t>(k);   // the row's byte is the choice's index
            Game<MenuStringFn>(fnMenuString)(item + kItemText,
                                             k < row.choices.size() ? row.choices[k].label : KeptText(s.labels[k]));
            Game<ItemFn>(fnListPush)(choices + 8, item);
            reinterpret_cast<ItemDtorFn>((*reinterpret_cast<void***>(item))[0])(item, 0);
        }
        Game<RowFn>(fnListRow)(page, label, value, choices, &g_default[i], 1);
        DestroyItems(choices, kListCount);
    }
    Game<SelfFn>(fnLabelDtor)(label);
}

// The page's row builder, called by the game's page code as (page, page's option data).
void __fastcall BuildRows(void* page, void*) {
    const std::vector<TabRow>& rows = TabRows();
    std::vector<TabShown> shown;
    AcquireSRWLockShared(&g_lock);
    for (const TabRow& r : rows)
        shown.push_back(TabShow(r, g_ini));
    ReleaseSRWLockShared(&g_lock);
    const size_t n = (std::min)(rows.size(), (size_t)kTabMaxRows);
    AcquireSRWLockExclusive(&g_lock);
    g_shown = shown;
    for (size_t i = 0; i < n; ++i) {
        g_value[i] = g_written[i] = static_cast<uint8_t>(shown[i].current);
        g_default[i] = static_cast<uint8_t>(shown[i].fallback);
    }
    ReleaseSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < n; ++i)
        AddRow(page, rows[i], shown[i], i);
}

void* BuildPage(void* windowList, void* data, uint8_t flag) {
    alignas(16) uint8_t name[kMenuStringBytes];
    alignas(16) uint8_t tab[kTabBytes + 8];
    alignas(16) uint8_t icon[0x40] = {};
    *reinterpret_cast<int32_t*>(icon) = Cfg().settingsTabIcon;
    *reinterpret_cast<int32_t*>(icon + 0x3c) = -1;   // the icon by its number, not an item's
    Game<MenuStringFn>(fnMenuString)(name, kTabName);
    Game<TabCtorFn>(fnTabCtor)(tab, name, kKindBrightness, icon);
    const Pmf rows{reinterpret_cast<void*>(&BuildRows), 0, 0}, none{};
    void* page = Game<CreatePageFn>(fnCreatePage)(windowList, tab, data, flag, kWindowAudio, &rows, &none, 1, 0);
    Game<SelfFn>(fnTabDtor)(tab);
    return page;
}
void* SafeBuildPage(void* windowList, void* data, uint8_t flag, uint32_t* fault) {
    __try {
        return BuildPage(windowList, data, flag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = GetExceptionCode();
        return nullptr;
    }
}

void* __fastcall Hook_BrightnessPage(void* windowList, void* data, uint8_t flag) {
    if (!g_tabAdded.load() || g_failed.load())
        return g_origPage(windowList, data, flag);
    uint32_t fault = 0;
    if (void* page = SafeBuildPage(windowList, data, flag, &fault)) {
        if (g_pagesBuilt.fetch_add(1) == 0)
            LOGI("[tab] ERVR page built: %zu rows in Sound's window", (std::min)(TabRows().size(), (size_t)kTabMaxRows));
        return page;
    }
    g_failed = true;
    LOGE("[tab] building the ERVR page faulted (0x%08x): the Settings go on without the ERVR tab", fault);
    return g_origPage(windowList, data, flag);
}

void AddTab(uint8_t* storage) {
    alignas(16) uint8_t name[kMenuStringBytes];
    alignas(16) uint8_t tab[kTabBytes + 8];
    alignas(16) uint8_t icon[0x40] = {};
    *reinterpret_cast<int32_t*>(icon) = Cfg().settingsTabIcon;
    *reinterpret_cast<int32_t*>(icon + 0x3c) = -1;
    Game<MenuStringFn>(fnMenuString)(name, kTabName);
    Game<TabCtorFn>(fnTabCtor)(tab, name, kKindBrightness, icon);
    g_origTabPush(storage, tab);
    Game<SelfFn>(fnTabDtor)(tab);
}
// ERVR's tab, unless the list has it or lacks room for it and `reserve` more. 0 added, 1 not, else a fault.
uint32_t SafeAddTab(uint8_t* storage, size_t reserve, size_t* index) {
    __try {
        const size_t count = *reinterpret_cast<size_t*>(storage + kTabsCount);
        for (size_t i = 0; i < count && i < kTabsMax; ++i)
            if (*reinterpret_cast<int*>(storage + i * kTabBytes + kTabKind) == kKindBrightness)
                return 1;
        if (count + 1 + reserve > kTabsMax)
            return 1;
        AddTab(storage);
        *index = count;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}
bool IsTabList(uint8_t* storage, int* kind, const uint8_t* tab, bool* inGame) {
    uintptr_t vt = 0;
    if (!SafeRead(storage - 8, &vt, sizeof(vt)) || vt != Vt(vtTabList))
        return false;
    uint8_t game = 0;
    return SafeRead(tab + kTabKind, kind, sizeof(*kind)) && SafeRead(storage + kTabsInGame, &game, 1) &&
           (*inGame = game != 0, true);
}
void NoteAdded(uint32_t result, size_t index, const char* where) {
    if (result == 0) {
        g_tabAdded = true;
        if (g_tabsAdded.fetch_add(1) == 0)
            LOGI("[tab] ERVR tab added to the game's Settings as tab %zu, %s", index + 1, where);
    } else if (result != 1) {
        g_failed = true;
        LOGE("[tab] adding the ERVR tab faulted (0x%08x): the Settings go on without it", result);
    }
}

void* __fastcall Hook_TabPush(uint8_t* storage, const uint8_t* tab) {
    int kind = -1;
    bool inGame = false;
    if (g_failed.load() || !IsTabList(storage, &kind, tab, &inGame))
        return g_origTabPush(storage, tab);
    size_t index = 0;
    // In game the last tab is Quit Game: ERVR's goes before it, after the settings.
    if (kind == kKindQuitGame) {
        const uint32_t added = SafeAddTab(storage, 1, &index);
        NoteAdded(added, index, "before Quit Game");
    }
    void* r = g_origTabPush(storage, tab);
    if (kind == kKindGraphics && !inGame) {
        const uint32_t added = SafeAddTab(storage, 0, &index);
        NoteAdded(added, index, "after Graphics");
    }
    return r;
}

// The tab bar: the dialog binds its 9 icons (BackTabList/Item_0_0..8) to tabs 0..8 once, as it opens, under a
// cursor grid (TabList) that scrolls when there are more tabs than icons. With ERVR's there are 10 in game, so
// the icons follow the grid: slot s shows tab s + the grid's scroll.
void RebindIcons(uint8_t* dialog, int32_t scroll) {
    for (int s = 0; s < 16; ++s) {
        char path[32];
        sprintf_s(path, "BackTabList/Item_0_%d", s);
        alignas(16) uint8_t slot[kProxyBytes];
        Game<FindChildFn>(fnFindChild)(dialog + kDialogRoot, slot, path);
        const bool valid = Game<ProxyValidFn>(fnProxyValid)(slot);
        if (valid) {
            alignas(16) uint8_t copy[kProxyBytes];
            Game<ProxyCopyFn>(fnProxyCopy)(copy, slot);
            void* dialogRef = dialog;
            Game<TabIconFn>(fnTabIcon)(&dialogRef, copy, s + scroll);   // releases the copy
        }
        Game<SelfFn>(fnProxyRelease)(slot + kProxyValue);
        if (!valid)
            break;
    }
}
bool HasErvrTab(uint8_t* dialog) {
    uint8_t* storage = dialog + kDialogTabs;
    size_t count = 0;
    if (!SafeRead(storage + kTabsCount, &count, sizeof(count)) || count > kTabsMax)
        return false;
    for (size_t i = 0; i < count; ++i) {
        int kind = -1;
        if (SafeRead(storage + i * kTabBytes + kTabKind, &kind, sizeof(kind)) && kind == kKindBrightness)
            return true;
    }
    return false;
}
uint32_t SafeRebindIcons(uint8_t* dialog, int32_t scroll) {
    __try {
        RebindIcons(dialog, scroll);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}
void __fastcall Hook_GridScroll(uint8_t* grid, const int32_t* xy) {
    g_origGridScroll(grid, xy);
    if (!g_tabAdded.load() || g_failed.load())
        return;
    uint8_t* dialog = grid - kDialogTabList;
    uintptr_t vt = 0;
    int32_t scroll = 0;
    if (!SafeRead(dialog, &vt, sizeof(vt)) || vt != Vt(vtDialog) ||
        !SafeRead(grid + kGridScrollX, &scroll, sizeof(scroll)) || !HasErvrTab(dialog))
        return;
    static int32_t s_logged = -1;
    if (scroll != s_logged) {
        s_logged = scroll;
        LOGI("[tab] tab bar scrolled to %d: its icons follow", scroll);
    }
    if (const uint32_t fault = SafeRebindIcons(dialog, scroll)) {
        g_failed = true;
        LOGE("[tab] moving the tab bar's icons faulted (0x%08x): the ERVR tab stands down", fault);
    }
}

// ERVR's page and Sound's share a window: the one about to open deletes the other, before the switch. A page
// being left that way is the composite's shown one, so the composite forgets it first (the switch then skips
// handing its option data on; the new page has its own from the game's settings, made at its creation).
void FreePage(uint8_t* composite, int kind) {
    void** slot = reinterpret_cast<void**>(composite + kCompositePages + kind * sizeof(void*));
    void* page = *slot;
    if (!page)
        return;
    void** current = reinterpret_cast<void**>(composite + kCompositeCurrent);
    if (*current == page)
        *current = nullptr;
    void* heap = Game<BlockHeapFn>(fnBlockHeap)(page);
    reinterpret_cast<void(__fastcall*)(void*, uint32_t)>((*reinterpret_cast<void***>(page))[1])(page, 0);
    reinterpret_cast<void(__fastcall*)(void*, void*)>((*reinterpret_cast<void***>(heap))[13])(heap, page);
    *slot = nullptr;
}
uint32_t SafeFreePage(uint8_t* composite, int kind) {
    __try {
        FreePage(composite, kind);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}
void __fastcall Hook_SetPage(uint8_t* composite, int kind) {
    uintptr_t vt = 0;
    if (g_tabAdded.load() && !g_failed.load() && (kind == kKindBrightness || kind == kKindSound) &&
        SafeRead(composite, &vt, sizeof(vt)) && vt == Vt(vtComposite)) {
        if (const uint32_t fault = SafeFreePage(composite, kind == kKindBrightness ? kKindSound : kKindBrightness)) {
            g_failed = true;
            LOGE("[tab] closing the page that shares ERVR's window faulted (0x%08x): the ERVR tab stands down", fault);
        }
    }
    g_origSetPage(composite, kind);
}

// ---- ERVR.ini -------------------------------------------------------------------------------------------
// Whole, then renamed into place: the hot-reload never reads half a file (ini_file.h).
std::string ReadAll(const std::wstring& path) { return IniReadAll(path); }
bool WriteAll(const std::wstring& path, const std::string& text) { return IniWriteAll(path, text, L".tab"); }

bool CheckGame() {
    for (const GameFn& f : kFns) {
        uint8_t head[sizeof(f.head)]{};
        if (!SafeRead(g_base + f.rva, head, sizeof(head)) || std::memcmp(head, f.head, sizeof(head)) != 0) {
            LOGW("[tab] the game's %s at exe+0x%x is not the one the ERVR tab was made for: the Settings stay as they are",
                 f.name, f.rva);
            return false;
        }
    }
    for (const GameVt& v : kVts) {
        uintptr_t colp = 0;
        uint32_t col[4]{};
        char name[96]{};
        const size_t want = std::strlen(v.rtti) + 1;
        if (!SafeRead(g_base + v.rva - 8, &colp, sizeof(colp)) ||
            !SafeRead(reinterpret_cast<void*>(colp), col, sizeof(col)) || col[0] != 1 || want > sizeof(name) ||
            !SafeRead(g_base + col[3] + 0x10, name, want) || std::memcmp(name, v.rtti, want) != 0) {
            LOGW("[tab] the vtable at exe+0x%x is not %s: the Settings stay as they are", v.rva, v.rtti);
            return false;
        }
    }
    return true;
}

} // namespace

void SettingsTabInit() {
    if (!Cfg().settingsTab) {
        LOGI("[tab] off ([SettingsTab] Enabled=false)");
        return;
    }
    if (CurrentGameBuild() != GameBuild::V272) {
        LOGI("[tab] no ERVR tab in the game's Settings on this game build (exe 2.7.2.0 only)");
        return;
    }
    g_base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!CheckGame())
        return;
    SettingsTabIniChanged();
    void* push = g_base + kFns[fnTabPush].rva;
    void* page = g_base + kFns[fnBrightnessPage].rva;
    void* scroll = g_base + kFns[fnGridScroll].rva;
    void* setPage = g_base + kFns[fnSetPage].rva;
    if (MH_CreateHook(push, reinterpret_cast<void*>(&Hook_TabPush), reinterpret_cast<void**>(&g_origTabPush)) != MH_OK ||
        MH_CreateHook(page, reinterpret_cast<void*>(&Hook_BrightnessPage), reinterpret_cast<void**>(&g_origPage)) != MH_OK ||
        MH_CreateHook(scroll, reinterpret_cast<void*>(&Hook_GridScroll), reinterpret_cast<void**>(&g_origGridScroll)) != MH_OK ||
        MH_CreateHook(setPage, reinterpret_cast<void*>(&Hook_SetPage), reinterpret_cast<void**>(&g_origSetPage)) != MH_OK ||
        MH_EnableHook(push) != MH_OK || MH_EnableHook(page) != MH_OK || MH_EnableHook(scroll) != MH_OK ||
        MH_EnableHook(setPage) != MH_OK) {
        MH_RemoveHook(push);
        MH_RemoveHook(page);
        MH_RemoveHook(scroll);
        MH_RemoveHook(setPage);
        LOGW("[tab] could not hook the game's Settings: no ERVR tab");
        return;
    }
    g_live = true;
    LOGI("[tab] the game's Settings get an ERVR tab (pause menu > System, after Graphics): %zu rows, icon %d",
         (std::min)(TabRows().size(), (size_t)kTabMaxRows), Cfg().settingsTabIcon);
}

void SettingsTabIniChanged() {
    std::string text = ReadAll(Cfg().iniPath);
    AcquireSRWLockExclusive(&g_lock);
    g_ini.swap(text);
    ReleaseSRWLockExclusive(&g_lock);
}

void SettingsTabTick(double now) {
    if (!g_live.load() || !g_tabAdded.load())
        return;
    const std::vector<TabRow>& rows = TabRows();
    const size_t n = (std::min)(rows.size(), (size_t)kTabMaxRows);
    uint8_t value[kTabMaxRows]{};
    for (size_t i = 0; i < n; ++i)
        value[i] = g_value[i];
    // A row held still for a moment goes into ERVR.ini: stepping through a list writes once, where it stops.
    static uint8_t s_seen[kTabMaxRows]{};
    static double s_movedAt = 0;
    if (std::memcmp(value, s_seen, n) != 0) {
        std::memcpy(s_seen, value, n);
        s_movedAt = now;
        return;
    }
    if (now - s_movedAt < 0.3)
        return;
    std::vector<TabWrite> writes;
    std::vector<bool> calibration;   // the Body calibration row's asks, in order
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < n && i < g_shown.size(); ++i)
        if (value[i] != g_written[i]) {
            if (rows[i].kind == TabRowKind::calibrate)
                calibration.push_back(TabAsksCalibration(rows[i], value[i]));
            for (TabWrite& w : TabWrites(rows[i], g_shown[i], value[i]))
                writes.push_back(std::move(w));
            g_written[i] = value[i];
        }
    ReleaseSRWLockExclusive(&g_lock);
    for (const bool ask : calibration) {
        LOGI("[tab] Body calibration: %s", ask ? "the T-pose panel comes up when the menu closes" : "not asked for after all");
        BodyCalibrationRequest(ask);
    }
    if (writes.empty())
        return;
    const std::wstring& path = Cfg().iniPath;
    std::string text = ReadAll(path);
    std::string said;
    for (const TabWrite& w : writes) {
        text = launcher::text::iniSet(text, w.section, w.key, w.value);
        said += (said.empty() ? "" : ", ") + w.section + "." + w.key + "=" + w.value;
    }
    if (text.empty() || !WriteAll(path, text)) {
        LOGW("[tab] could not write ERVR.ini (%s): the change did not apply", said.c_str());
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_ini = text;
    ReleaseSRWLockExclusive(&g_lock);
    LOGI("[tab] ERVR.ini: %s", said.c_str());
}

} // namespace ervr
