// XInput hooks: ELDEN RING polls XInputGetState every frame (via XInput9_1_0.dll,
// with XInput1_4 also present). We detour GetState / GetStateEx / GetCapabilities
// / SetState in every loaded XInput module and merge the virtual pad in.
//
// If no physical gamepad is plugged in, XInput returns ERROR_DEVICE_NOT_CONNECTED
// for user 0; we turn that into a connected, zeroed pad so the game accepts our
// synthetic input.
#include "xinput_hook.h"
#include "virtual_pad.h"
#include "autopilot.h"
#include "config.h"
#include "log.h"
#include <windows.h>
#include <xinput.h>
#include <MinHook.h>
#include <vector>
#include <cstring>

namespace ervr {
namespace {

// Resolve an export by walking the PE export table ourselves. R.E.A.L. hooks
// kernel32!GetProcAddress, so the address it hands back may be its own detour
// rather than the function the game's import table points at.
void* RawExport(HMODULE mod, const char* name, WORD ordinal) {
    auto* base = (uint8_t*)mod;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return nullptr;
    auto* exp = (IMAGE_EXPORT_DIRECTORY*)(base + dir.VirtualAddress);
    auto* funcs = (DWORD*)(base + exp->AddressOfFunctions);
    auto* names = (DWORD*)(base + exp->AddressOfNames);
    auto* ords  = (WORD*)(base + exp->AddressOfNameOrdinals);
    DWORD rva = 0;
    if (name) {
        for (DWORD i = 0; i < exp->NumberOfNames; ++i)
            if (strcmp((const char*)(base + names[i]), name) == 0) { rva = funcs[ords[i]]; break; }
    }
    if (!rva && ordinal) {
        DWORD idx = ordinal - exp->Base;
        if (idx < exp->NumberOfFunctions) rva = funcs[idx];
    }
    if (!rva) return nullptr;
    if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) return nullptr;   // forwarder; ignore
    return base + rva;
}

using PFN_GetState = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
using PFN_SetState = DWORD (WINAPI*)(DWORD, XINPUT_VIBRATION*);
using PFN_GetCaps  = DWORD (WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);

struct ModuleHooks {
    const wchar_t* name;
    HMODULE mod = nullptr;
    PFN_GetState origGetState = nullptr, origGetStateEx = nullptr;
    PFN_SetState origSetState = nullptr;
    PFN_GetCaps  origGetCaps = nullptr;
    void* tGetState = nullptr; void* tGetStateEx = nullptr; void* tSetState = nullptr; void* tGetCaps = nullptr;
};

ModuleHooks g_mods[] = { {L"xinput1_4.dll"}, {L"xinput1_3.dll"}, {L"xinput9_1_0.dll"} };
DWORD g_packet = 1;
volatile LONG g_calls = 0, g_merged = 0, g_fabricated = 0;

DWORD Common_GetState(PFN_GetState orig, DWORD user, XINPUT_STATE* st) {
    DWORD r = orig ? orig(user, st) : ERROR_DEVICE_NOT_CONNECTED;
    if (user == 0) InterlockedIncrement(&g_calls);
    if (user != 0 || !st) return r;
    if (r != ERROR_SUCCESS) {
        // No physical pad: fabricate an idle one only if VR controllers are live - or if the
        // autopilot is driving an unattended test run, which must work with no headset at all.
        XINPUT_STATE tmp{};
        bool have = VirtualPadMerge(tmp);
        have |= AutoPilotMerge(tmp.Gamepad);
        // Must claim "connected" even while the autopilot is idle. ELDEN RING polls an empty
        // slot exactly ONCE and then never asks again, so reporting not-connected during the
        // startup delay means no later press is ever seen. Measured: 1 poll in a 50 s run.
        have |= AutoPilotActive();
        if (!have) return r;
        *st = tmp;
        st->dwPacketNumber = ++g_packet;
        InterlockedIncrement(&g_fabricated);
        return ERROR_SUCCESS;
    }
    XINPUT_GAMEPAD before = st->Gamepad;
    bool changed = VirtualPadMerge(*st);
    changed |= AutoPilotMerge(st->Gamepad);
    if (changed && std::memcmp(&before, &st->Gamepad, sizeof(before)) != 0) {
        st->dwPacketNumber = ++g_packet + st->dwPacketNumber;   // ensure the game notices the change
        InterlockedIncrement(&g_merged);
    }
    return ERROR_SUCCESS;
}

template <int I> DWORD WINAPI Hook_GetState(DWORD user, XINPUT_STATE* st)   { return Common_GetState(g_mods[I].origGetState, user, st); }
template <int I> DWORD WINAPI Hook_GetStateEx(DWORD user, XINPUT_STATE* st) { return Common_GetState(g_mods[I].origGetStateEx, user, st); }
template <int I> DWORD WINAPI Hook_SetState(DWORD user, XINPUT_VIBRATION* vib) {
    if (user == 0 && vib && Cfg().rumbleToControllers)
        VirtualPadRumble(vib->wLeftMotorSpeed / 65535.f, vib->wRightMotorSpeed / 65535.f);
    DWORD r = g_mods[I].origSetState ? g_mods[I].origSetState(user, vib) : ERROR_DEVICE_NOT_CONNECTED;
    if (r != ERROR_SUCCESS && user == 0) { XINPUT_STATE t{}; if (VirtualPadMerge(t) || AutoPilotActive()) return ERROR_SUCCESS; }
    return r;
}
template <int I> DWORD WINAPI Hook_GetCaps(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) {
    DWORD r = g_mods[I].origGetCaps ? g_mods[I].origGetCaps(user, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
    if (r != ERROR_SUCCESS && user == 0 && caps) {
        XINPUT_STATE t{};
        if (VirtualPadMerge(t) || AutoPilotActive()) {
            std::memset(caps, 0, sizeof(*caps));
            caps->Type = XINPUT_DEVTYPE_GAMEPAD;
            caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
            caps->Flags = 0;
            caps->Gamepad.wButtons = 0xF3FF; caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 0xFF;
            caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = (SHORT)0xFFC0;
            caps->Vibration.wLeftMotorSpeed = caps->Vibration.wRightMotorSpeed = 0xFF;
            return ERROR_SUCCESS;
        }
    }
    return r;
}

template <int I> bool HookModule() {
    ModuleHooks& m = g_mods[I];
    if (m.mod) return true;
    HMODULE h = GetModuleHandleW(m.name);
    if (!h) return false;
    m.mod = h;
    auto hook = [&](void* target, void* detour, void** orig, const char* what) {
        if (!target) return;
        if (MH_CreateHook(target, detour, orig) == MH_OK && MH_EnableHook(target) == MH_OK) LOGI("[xinput] hooked %S!%s", m.name, what);
        else LOGW("[xinput] failed to hook %S!%s", m.name, what);
    };
    m.tGetState = RawExport(h, "XInputGetState", 2);
    m.tGetStateEx = RawExport(h, nullptr, 100);   // XInputGetStateEx (guide button)
    m.tSetState = RawExport(h, "XInputSetState", 3);
    m.tGetCaps  = RawExport(h, "XInputGetCapabilities", 4);
    LOGD("[xinput] %S exports: GetState %p (GetProcAddress says %p) GetStateEx %p SetState %p GetCaps %p", m.name,
         m.tGetState, (void*)GetProcAddress(h, "XInputGetState"), m.tGetStateEx, m.tSetState, m.tGetCaps);
    if (m.tGetStateEx == m.tGetState) m.tGetStateEx = nullptr;
    hook(m.tGetState,   (void*)&Hook_GetState<I>,   (void**)&m.origGetState,   "XInputGetState");
    hook(m.tGetStateEx, (void*)&Hook_GetStateEx<I>, (void**)&m.origGetStateEx, "XInputGetStateEx");
    hook(m.tSetState,   (void*)&Hook_SetState<I>,   (void**)&m.origSetState,   "XInputSetState");
    hook(m.tGetCaps,    (void*)&Hook_GetCaps<I>,    (void**)&m.origGetCaps,    "XInputGetCapabilities");
    return true;
}

// ---- IAT hook on eldenring.exe -----------------------------------------------------
// The game imports XINPUT1_4.dll by ordinal (2 = XInputGetState, 3 = XInputSetState).
// Patching the IAT slots guarantees we sit on the game's actual call path no matter
// what other hooks (R.E.A.L.) did to the export or to GetProcAddress.
PFN_GetState g_iatOrigGetState = nullptr;
PFN_SetState g_iatOrigSetState = nullptr;
PFN_GetCaps  g_iatOrigGetCaps = nullptr;
bool g_iatDone = false;

DWORD WINAPI IAT_GetState(DWORD user, XINPUT_STATE* st) { return Common_GetState(g_iatOrigGetState, user, st); }
DWORD WINAPI IAT_SetState(DWORD user, XINPUT_VIBRATION* vib) {
    if (user == 0 && vib && Cfg().rumbleToControllers)
        VirtualPadRumble(vib->wLeftMotorSpeed / 65535.f, vib->wRightMotorSpeed / 65535.f);
    DWORD r = g_iatOrigSetState ? g_iatOrigSetState(user, vib) : ERROR_DEVICE_NOT_CONNECTED;
    if (r != ERROR_SUCCESS && user == 0) { XINPUT_STATE t{}; if (VirtualPadMerge(t) || AutoPilotActive()) return ERROR_SUCCESS; }
    return r;
}
DWORD WINAPI IAT_GetCaps(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) {
    DWORD r = g_iatOrigGetCaps ? g_iatOrigGetCaps(user, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
    if (r != ERROR_SUCCESS && user == 0 && caps) {
        XINPUT_STATE t{};
        if (VirtualPadMerge(t) || AutoPilotActive()) {
            std::memset(caps, 0, sizeof(*caps));
            caps->Type = XINPUT_DEVTYPE_GAMEPAD; caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
            caps->Gamepad.wButtons = 0xF3FF; caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 0xFF;
            caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = (SHORT)0xFFC0;
            caps->Vibration.wLeftMotorSpeed = caps->Vibration.wRightMotorSpeed = 0xFF;
            return ERROR_SUCCESS;
        }
    }
    return r;
}

bool PatchSlot(void** slot, void* detour, void** origOut, const char* what) {
    if (*slot == detour) return true;
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) { LOGW("[xinput] IAT VirtualProtect failed for %s", what); return false; }
    *origOut = *slot;
    *slot = detour;
    VirtualProtect(slot, sizeof(void*), old, &old);
    LOGI("[xinput] IAT patched %s: slot %p, was %p", what, (void*)slot, *origOut);
    return true;
}

bool HookMainIAT() {
    if (g_iatDone) return true;
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    HMODULE xi = GetModuleHandleW(L"xinput1_4.dll");
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        const char* dll = (const char*)(base + imp->Name);
        if (_stricmp(dll, "xinput1_4.dll") != 0 && _stricmp(dll, "xinput1_3.dll") != 0 && _stricmp(dll, "xinput9_1_0.dll") != 0) continue;
        auto* thunkNames = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        auto* thunkIat   = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; thunkNames->u1.AddressOfData; ++thunkNames, ++thunkIat) {
            const char* fname = nullptr; WORD ord = 0;
            if (IMAGE_SNAP_BY_ORDINAL(thunkNames->u1.Ordinal)) ord = IMAGE_ORDINAL(thunkNames->u1.Ordinal);
            else fname = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + thunkNames->u1.AddressOfData))->Name;
            auto is = [&](const char* n, WORD o) { return (fname && strcmp(fname, n) == 0) || (!fname && ord == o); };
            void** slot = (void**)&thunkIat->u1.Function;
            if (is("XInputGetState", 2)) {
                void* real = xi ? RawExport(xi, "XInputGetState", 2) : nullptr;
                LOGI("[xinput] %s import XInputGetState(ord 2): IAT -> %p, real export %p, GetProcAddress %p", dll, *slot, real, xi ? (void*)GetProcAddress(xi, "XInputGetState") : nullptr);
                PatchSlot(slot, (void*)&IAT_GetState, (void**)&g_iatOrigGetState, "XInputGetState");
            } else if (is("XInputSetState", 3)) {
                PatchSlot(slot, (void*)&IAT_SetState, (void**)&g_iatOrigSetState, "XInputSetState");
            } else if (is("XInputGetCapabilities", 4)) {
                PatchSlot(slot, (void*)&IAT_GetCaps, (void**)&g_iatOrigGetCaps, "XInputGetCapabilities");
            } else {
                LOGD("[xinput] %s import %s(ord %u) left alone", dll, fname ? fname : "?", ord);
            }
        }
        g_iatDone = true;
    }
    return g_iatDone;
}

} // namespace

void XInputHookStats(long& calls, long& merged, long& fabricated) {
    calls = InterlockedExchange(&g_calls, 0); merged = InterlockedExchange(&g_merged, 0); fabricated = InterlockedExchange(&g_fabricated, 0);
}

bool XInputHooksInstall() {
    bool any = false;
    any |= HookMainIAT();
    any |= HookModule<0>();
    any |= HookModule<1>();
    any |= HookModule<2>();
    return any;
}

void XInputHooksRemove() {
    for (auto& m : g_mods) {
        for (void* t : { m.tGetState, m.tGetStateEx, m.tSetState, m.tGetCaps })
            if (t) { MH_DisableHook(t); MH_RemoveHook(t); }
        m = ModuleHooks{m.name};
    }
}
}
