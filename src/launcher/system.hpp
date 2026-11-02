#pragma once
// What the launcher finds on this PC (the game, Steam, the VR runtimes, ERVR's files and what else sits in
// the game folder), what it changes in the game folder (install.ps1 and tools/game_mode.ps1, in C++), and the
// play session it runs: Steam, the game started with EAC off, ERVR.log followed line by line. Long work runs
// on worker threads; the window reads snapshots. Nothing here touches the network.
#include "launcher_text.hpp"
#include <windows.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace launcher {
using text::LineKind;
using text::LogLine;
using text::Progress;

inline constexpr const char* kVersion = ERVR_VERSION;
inline constexpr const char* kAuthor = "Ilya Mezerowsky";
inline constexpr const char* kRepoUrl = "https://github.com/IlyaMez/EldenringVR";
inline constexpr const char* kIssuesUrl = "https://github.com/IlyaMez/EldenringVR/issues";
// The Discord server ERVR shares with Spidy: release news, help (#ervr-help) and clips (the README's invite).
inline constexpr const char* kDiscordUrl = "https://discord.gg/x27ZXDpdNt";
inline constexpr const char* kReleasesUrl = "https://github.com/IlyaMez/EldenringVR/releases";
inline constexpr const char* kKofiUrl = "https://ko-fi.com/ilyamezerowsky";
inline constexpr const char* kCoopUrl = "https://www.nexusmods.com/eldenring/mods/510";
inline constexpr const wchar_t* kSteamAppId = L"1245620";
// The runtime choice that picks, at each start, the runtime the headset is most likely streaming through
// (automaticRuntime); any other choice is a runtime manifest's path.
inline constexpr const wchar_t* kAutoRuntime = L"auto";
// What a parked file is renamed to (tools/game_mode.ps1's suffix).
inline constexpr const wchar_t* kParked = L".ervr-off";

std::string narrow(std::wstring_view text);
std::wstring widen(std::string_view text);

// How the game starts: eldenring.exe directly with steam_appid.txt beside it (no EAC, no FromSoftware online
// play), or Seamless Co-op's ersc_launcher.exe, which starts it with EAC off itself.
enum class StartMode { offline, coop };

struct Runtime {
    std::wstring manifest;
    std::string name;
    bool active{}; // Windows' active OpenXR runtime
};

// A file in the game folder that breaks the headset image, live or parked (<path>.ervr-off).
struct ModFile {
    std::wstring path; // the live path
    std::string label; // what it is, for people
    bool parked{};
};

enum class InstallState { missing, current, older, newer, different, parked };

// ERVR in the game folder, against the copy this launcher carries.
struct Install {
    InstallState state = InstallState::missing;
    std::string version;     // ERVR\ERVR.dll's
    bool dll{}, ini{};       // ERVR\ERVR.dll, ERVR\ERVR.ini
    bool proxy{};            // ERVR's dinput8.dll, live
    bool proxyParked{};      // ERVR's dinput8.dll.ervr-off
    bool foreignProxy{};     // another mod loader's dinput8.dll
    bool reshade{};          // dxgi.dll is the bundled ReShade
    std::string dxgi;        // what dxgi.dll is otherwise ("ReShade 5.9.2", "not ReShade"); empty: no dxgi.dll
    std::vector<ModFile> flat; // flat-only render mods and R.E.A.L. VR's files
};

// ELDEN RING's own graphics settings: every GraphicsConfig.xml under %APPDATA%\EldenRing (directly there, or in a
// folder per Steam account; ERVR.dll edits the same files).
struct GameGraphics {
    std::vector<std::wstring> files;
    // Each of text::kVrEffects as the first file has it ("MAX", "DISABLE"); empty when it lacks the tag.
    std::array<std::string, text::kVrEffects.size()> values;
};
GameGraphics readGameGraphics();

struct Scan {
    bool done{};
    // This launcher's ERVR: the zip's files beside it, or build\dist in a checkout.
    std::wstring payload;
    std::vector<std::string> payloadMissing;
    std::string payloadVersion;
    bool payloadInGame{}; // the launcher runs from the game folder: its files are the installed ones
    // Steam and the game.
    std::wstring steam; // steam.exe
    bool steamRunning{};
    std::wstring gameExe, gameDir;
    std::string gameSource; // "Steam", "chosen by you", "the launcher's folder"
    std::string gameVersion, gamePatch; // eldenring.exe's version; its patch when ERVR has its address map
    bool gameRunning{};
    bool coop{};            // ersc_launcher.exe in the game folder
    bool writable = true;   // the launcher can change files in the game folder
    Install install;
    GameGraphics graphics;
    // VR.
    std::vector<Runtime> runtimes;
    bool virtualDesktop{}, steamVr{}; // their streamer / server runs now
};

struct Settings {
    std::wstring gameExe, runtime = kAutoRuntime;
    StartMode mode = StartMode::offline;
    bool shortcutsAsked{};
    // The game's effects (text::kVrEffects) the launcher turns off; the rest stay as the game's menu sets them.
    text::EffectsOff effectsOff = text::allEffects();
};
Settings loadSettings();
void saveSettings(const Settings& settings);

class Scanner {
public:
    ~Scanner();
    void start(const Settings& settings);
    Scan snapshot();
    bool busy();

private:
    void run(Settings settings);
    std::mutex mutex_;
    Scan scan_;
    std::thread worker_;
    bool busy_{};
};

// The game folder's ERVR read again after a change, the rest of the scan kept.
void refreshInstall(Scan& scan);

// The runtime Automatic picks now: Virtual Desktop while its streamer runs, SteamVR while it runs; null for
// Windows' active one.
const Runtime* automaticRuntime(const Scan& scan);

// A change to the game folder (the game must be closed): what it did, a line a change, or why it stopped.
struct Change {
    bool ok = true;
    bool accessDenied{}; // Windows refused a file: the launcher needs administrator rights there
    std::vector<std::string> lines;
};
// Copies this launcher's ERVR into the game folder (install.ps1): an existing ERVR.ini and ReShade.ini are
// kept, a foreign dxgi.dll is kept as dxgi.dll.ervr-bak, another loader's dinput8.dll is never replaced (that
// loader has to load ERVR\ERVR.dll; ReShade is installed all the same, as ERVR needs it either way). ERVR's own
// DLLs are replaced only when missing or older, unless `replaceBuild` (a newer or another build stays otherwise).
Change installErvr(const Scan& scan, bool replaceBuild = false);
// tools/game_mode.ps1 -Mode vr: ERVR's dinput8.dll back in place, flat render mods and R.E.A.L.'s files parked.
Change switchToVr(const Scan& scan);
// -Mode flat: ERVR parked, the flat render mods back.
Change switchToFlat(const Scan& scan);
// install.ps1 -Uninstall: ERVR's DLLs and the bundled ReShade gone (the old dxgi.dll back); ERVR.ini, its logs
// and ReShade.ini stay.
Change uninstallErvr(const Scan& scan);
// The game folder's ERVR.ini replaced by this launcher's, the old one kept beside it.
Change restoreDefaultIni(const Scan& scan);
// The effects `which` names switched off in every GraphicsConfig.xml, each file first kept once as
// GraphicsConfig_ervr_backup.xml (ERVR.dll's backup, the same rule). Refused while the game runs: it writes the
// file back as it closes. A line a file changed; none when nothing needed changing.
Change turnOffGameEffects(const Scan& scan, const text::EffectsOff& which);

// ERVR.ini as the Options tab edits it.
struct IniFile {
    std::wstring path;
    std::string text;
    uint64_t stamp{}; // last write time: an edit made elsewhere reloads it
};
std::optional<IniFile> readIni(const std::wstring& path);
uint64_t writeTime(const std::wstring& path);
// Written whole and then put in place, so ERVR's hot reload never reads half a file.
bool writeIni(IniFile& ini);
// The file the Options tab edits: the game folder's, else this launcher's (installed with it).
std::wstring iniPath(const Scan& scan);

enum class Outcome { idle, running, ok, warning, error };

// This launcher run again as `--probe` (runProbe), against the chosen runtime: the headset, if one answers.
class HeadsetCheck {
public:
    ~HeadsetCheck();
    void start(const std::wstring& manifest);
    Outcome state();
    text::ProbeResult result();
    void reset();

private:
    std::mutex mutex_;
    Outcome state_ = Outcome::idle;
    text::ProbeResult result_;
    std::thread worker_;
};

// main.cpp's --probe: asks the OpenXR runtime (XR_RUNTIME_JSON's, else Windows' active one) for a headset and
// prints what answered (text::parseProbe). Exit 0: a headset; 2: the runtime, but no headset; 3: no runtime.
int runProbe();

struct LaunchPlan {
    std::wstring gameDir, gameExe, steamExe;
    StartMode mode = StartMode::offline;
    std::wstring runtimeManifest; // empty: Windows' active runtime
    std::string runtimeName;
    bool attach{};                // the game runs already: follow it, start nothing
    std::vector<std::string> before; // what the launcher did first (install, park), for the log
};

// A play session: Steam, the game, then ERVR.log until the game closes.
class Session {
public:
    enum class Phase { idle, steam, starting, game };
    ~Session();
    bool start(LaunchPlan plan, std::string& error);
    // The game's window is asked to close, as Alt+F4 does.
    void requestQuit();
    void forceQuit();
    bool running();
    bool quitRequested();
    Phase phase();
    std::optional<DWORD> exitCode();
    // Lines from `from` on; returns the total so far.
    size_t lines(size_t from, std::vector<LogLine>& out);
    Progress progress();
    std::chrono::steady_clock::time_point started() const { return started_; }
    std::chrono::steady_clock::time_point quitRequestedAt() const { return quitAt_; }

private:
    void run(LaunchPlan plan);
    // The launcher's own line, and a line of ERVR.log (which also tells how far the session got).
    void say(LineKind kind, std::string text);
    void add(LogLine line, bool ervr);
    std::mutex mutex_;
    std::vector<LogLine> lines_;
    Progress progress_;
    std::thread worker_;
    bool running_{}, quitRequested_{};
    Phase phase_ = Phase::idle;
    DWORD gamePid_{};
    std::optional<DWORD> exit_;
    std::chrono::steady_clock::time_point started_, quitAt_;
};

// For the bug-report summary.
struct Hardware {
    std::string gpu;
    uint64_t gpuMemoryMb{}, ramMb{};
};
Hardware hardware();
// The last lines of the game folder's ERVR.log, read as the session reads them.
std::vector<LogLine> lastLog(const std::wstring& gameDir);
bool gameRunning(const std::wstring& gameDir);
// The running game's windows asked to close, as Alt+F4 does; false when it has none.
bool askGameToClose(const std::wstring& gameDir);

// Shell actions.
void openUrl(const char* url);
void openPath(const std::wstring& path);
std::optional<std::wstring> browseForGame(HWND owner, const std::wstring& current);
bool createShortcuts(std::string& error);
// Starts this launcher again as administrator (Windows asks first); the new one waits for this one to close.
// False when it did not start, with `error` empty when the player declined Windows' question.
bool restartAsAdministrator(std::string& error);

} // namespace launcher
