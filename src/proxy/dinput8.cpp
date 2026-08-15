// dinput8.dll proxy: forwards DirectInput8Create to the real System32 dinput8.dll
// and loads ERVR\ERVR.dll (the motion-control mod) into the game process.
//
// Zero game-specific logic lives here on purpose; if you already use another
// dinput8-based mod loader (e.g. ELDEN MOD LOADER) drop ERVR.dll in its mods
// folder instead and delete this proxy.
#include <windows.h>
#include <unknwn.h>
#include <string>

namespace {

HMODULE g_real = nullptr;
using PFN_DirectInput8Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
PFN_DirectInput8Create g_realCreate = nullptr;

std::wstring ModuleDir(HMODULE m) {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(m, buf, MAX_PATH);
    std::wstring s(buf);
    size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"." : s.substr(0, p);
}

void LoadReal() {
    if (g_real) return;
    wchar_t sys[MAX_PATH]{};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring path = std::wstring(sys) + L"\\dinput8.dll";
    g_real = LoadLibraryW(path.c_str());
    if (g_real)
        g_realCreate = reinterpret_cast<PFN_DirectInput8Create>(GetProcAddress(g_real, "DirectInput8Create"));
}

void LoadMod(HMODULE self) {
    std::wstring dir = ModuleDir(self);
    // Preferred layout: <Game>\ERVR\ERVR.dll ; fallback <Game>\ERVR.dll
    const std::wstring candidates[] = { dir + L"\\ERVR\\ERVR.dll", dir + L"\\ERVR.dll" };
    for (const auto& c : candidates) {
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) {
            LoadLibraryW(c.c_str());
            break;
        }
    }
    // Also load any DLL in <Game>\ERVR\mods\ so other small mods can ride along.
    WIN32_FIND_DATAW fd{};
    std::wstring pat = dir + L"\\ERVR\\mods\\*.dll";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { LoadLibraryW((dir + L"\\ERVR\\mods\\" + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

} // namespace

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter) {
    LoadReal();
    if (!g_realCreate) return E_FAIL;
    return g_realCreate(hinst, dwVersion, riidltf, ppvOut, punkOuter);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        LoadReal();
        LoadMod(hModule);
    }
    return TRUE;
}
