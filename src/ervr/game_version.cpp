#include "game_version.h"
#include "log.h"
#include <windows.h>
#include <atomic>
#include <string>
#include <vector>
#pragma comment(lib, "version.lib")

namespace ervr {
namespace {
std::atomic<GameBuild> g_build{GameBuild::Unknown};
bool ReadVersion(const wchar_t* path, VS_FIXEDFILEINFO& out) {
    DWORD ignored = 0; const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
    if (!size) return false;
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(path, 0, size, data.data())) return false;
    VS_FIXEDFILEINFO* info = nullptr; UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", (void**)&info, &len) || len < sizeof(out)) return false;
    out = *info; return true;
}
}
void GameVersionInit() {
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr, path, MAX_PATH);
    VS_FIXEDFILEINFO version{};
    if (!ReadVersion(path, version)) return;
    const GameBuild build = IdentifyGameBuild(version.dwFileVersionMS, version.dwFileVersionLS);
    g_build.store(build);
    LOGI("[game] executable version %u.%u.%u.%u: %s", HIWORD(version.dwFileVersionMS), LOWORD(version.dwFileVersionMS),
         HIWORD(version.dwFileVersionLS), LOWORD(version.dwFileVersionLS), build == GameBuild::Unknown ? "no verified RVA map" : "verified RVA map selected");
    const std::wstring running(path);
    const auto slash = running.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    const std::wstring installed = running.substr(0, slash + 1) + L"eldenring.exe";
    VS_FIXEDFILEINFO other{};
    if (_wcsicmp(running.c_str(), installed.c_str()) && ReadVersion(installed.c_str(), other) &&
        (version.dwFileVersionMS != other.dwFileVersionMS || version.dwFileVersionLS != other.dwFileVersionLS))
        LOGW("[game] launch executable differs from installed eldenring.exe (%u.%u.%u.%u). An old renamed launcher with updated game assets can leave Torrent's model missing; align the launcher and game data versions.",
             HIWORD(other.dwFileVersionMS), LOWORD(other.dwFileVersionMS), HIWORD(other.dwFileVersionLS), LOWORD(other.dwFileVersionLS));
}
GameBuild CurrentGameBuild() { return g_build.load(); }
uint32_t GameRva(uint32_t legacy) { return GameBuildRva(CurrentGameBuild(), legacy); }
} // namespace ervr
