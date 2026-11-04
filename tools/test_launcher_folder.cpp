#include "system.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

// The launcher's changes to a game folder, on a throwaway one: install (a foreign dxgi.dll kept, ERVR.ini kept),
// parking what is in VR's way, flat play and back, restoring ERVR.ini, and removing ERVR again. The package it
// installs is assembled from this build's own ERVR.dll and dinput8.dll and the bundled ReShade.
namespace fs = std::filesystem;
using namespace launcher;

namespace {
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}
void Write(const fs::path& path, const char* text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}
std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}
bool Exists(const fs::path& path) { return fs::exists(path); }
}

int main() {
    const fs::path root = fs::temp_directory_path() / ("ervr-launcher-test-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(root);
    const fs::path payload = root / "package", game = root / "ELDEN RING" / "Game";
    fs::create_directories(payload / "ERVR");
    fs::copy_file(ERVR_TEST_DLL, payload / "ERVR" / "ERVR.dll");
    fs::copy_file(ERVR_TEST_PROXY, payload / "dinput8.dll");
    fs::copy_file(ERVR_TEST_RESHADE, payload / "dxgi.dll");
    fs::copy_file(ERVR_TEST_RESHADE_INI, payload / "ReShade.ini");
    fs::copy_file(ERVR_TEST_INI, payload / "ERVR" / "ERVR.ini");
    // A game folder with a foreign dxgi.dll, a player's own ERVR.ini, a ReShade add-on and R.E.A.L.'s leftovers.
    fs::create_directories(game);
    fs::copy_file(ERVR_TEST_FOREIGN_DLL, game / "dxgi.dll");
    Write(game / "ERVR" / "ERVR.ini", "[VR]\nStereoMode=cinema\n");
    Write(game / "reshade-addons" / "renodx.addon64", "addon");
    Write(game / "ReShade.ini", "[ADDON]\nAddonPath=.\\reshade-addons\n");
    Write(game / "RealVR.ini", "real");

    Scan scan;
    scan.payload = payload.wstring();
    scan.payloadVersion = ERVR_VERSION;
    scan.gameDir = game.wstring();
    scan.gameExe = (game / "eldenring.exe").wstring();
    refreshInstall(scan);
    Check(scan.install.state == InstallState::missing, "starts uninstalled");
    Check(!scan.install.reshade && scan.install.dxgi.find("Microsoft") != std::string::npos, "foreign dxgi.dll named");
    Check(scan.install.flat.size() == 2, "the add-on and R.E.A.L.'s file are in VR's way");

    Change change = installErvr(scan);
    Check(change.ok, "install");
    Check(Exists(game / "dxgi.dll.ervr-bak"), "foreign dxgi.dll kept");
    Check(Read(game / "ERVR" / "ERVR.ini") == "[VR]\nStereoMode=cinema\n", "the player's ERVR.ini kept");
    Check(Read(game / "ReShade.ini") == "[ADDON]\nAddonPath=.\\reshade-addons\n", "the player's ReShade.ini kept");
    refreshInstall(scan);
    Check(scan.install.state == InstallState::current && scan.install.reshade && scan.install.proxy, "installed and current");

    // A player's own build of the same version (here: another ERVR module) stays when an install only has to
    // put back a missing ERVR.ini; Replace takes it.
    const std::string own = Read(ERVR_TEST_PROXY);
    fs::copy_file(ERVR_TEST_PROXY, game / "ERVR" / "ERVR.dll", fs::copy_options::overwrite_existing);
    fs::rename(game / "ERVR" / "ERVR.ini", root / "ERVR.ini.player");
    refreshInstall(scan);
    Check(scan.install.state == InstallState::different && !scan.install.ini, "another build, no ERVR.ini");
    Check(installErvr(scan).ok && Read(game / "ERVR" / "ERVR.dll") == own && Exists(game / "ERVR" / "ERVR.ini"),
          "another build kept, ERVR.ini added");
    refreshInstall(scan);
    Check(installErvr(scan, true).ok && Read(game / "ERVR" / "ERVR.dll") == Read(ERVR_TEST_DLL), "Replace takes it");
    fs::copy_file(root / "ERVR.ini.player", game / "ERVR" / "ERVR.ini", fs::copy_options::overwrite_existing);
    refreshInstall(scan);
    Check(scan.install.state == InstallState::current, "current again");

    change = switchToVr(scan);
    Check(change.ok && Exists(game / "reshade-addons" / "renodx.addon64.ervr-off") && Exists(game / "RealVR.ini.ervr-off"),
          "flat mods parked");
    refreshInstall(scan);
    Check(scan.install.flat.size() == 2 && scan.install.flat[0].parked && scan.install.flat[1].parked, "seen parked");

    change = switchToFlat(scan);
    Check(change.ok && Exists(game / "dinput8.dll.ervr-off") && !Exists(game / "dinput8.dll"), "ERVR parked for flat play");
    Check(Exists(game / "reshade-addons" / "renodx.addon64"), "the add-on is back");
    Check(Exists(game / "RealVR.ini.ervr-off"), "R.E.A.L. stays parked in flat play");
    refreshInstall(scan);
    Check(scan.install.state == InstallState::parked, "parked state");

    change = switchToVr(scan);
    Check(change.ok && Exists(game / "dinput8.dll") && !Exists(game / "dinput8.dll.ervr-off"), "ERVR back for VR");
    Check(Exists(game / "reshade-addons" / "renodx.addon64.ervr-off"), "the add-on parked again");

    refreshInstall(scan);
    change = restoreDefaultIni(scan);
    Check(change.ok && Read(game / "ERVR" / "ERVR.ini") == Read(payload / "ERVR" / "ERVR.ini"), "defaults restored");
    bool kept = false;
    for (const auto& entry : fs::directory_iterator(game / "ERVR"))
        kept |= entry.path().extension() == ".bak" && Read(entry.path()) == "[VR]\nStereoMode=cinema\n";
    Check(kept, "the player's settings kept as a .bak");
    // The body calibration measures the player; it is no setting, and Defaults keeps it.
    Write(game / "ERVR" / "ERVR.ini", "[VR]\nStereoMode=cinema\n[Body]\nEyeHeightMm=1760\nArmLengthMm=640\nCalibrationPrompt=true\n");
    change = restoreDefaultIni(scan);
    {
        const std::string now = Read(game / "ERVR" / "ERVR.ini");
        Check(change.ok && text::iniGet(now, "Body", "EyeHeightMm") == "1760" && text::iniGet(now, "Body", "ArmLengthMm") == "640" &&
                  text::iniGet(now, "Body", "CalibrationPrompt") == "true",
              "Defaults keeps the body calibration");
        Check(text::iniGet(now, "VR", "StereoMode") != "cinema", "and none of the player's settings");
    }

    // ERVR.ini edits as the Options tab makes them, through the file.
    auto ini = readIni((game / "ERVR" / "ERVR.ini").wstring());
    Check(ini.has_value(), "ini read");
    const std::string before = ini->text;
    ini->text = text::iniSet(ini->text, "VR", "WorldScale", "0.8");
    Check(writeIni(*ini) && text::iniGet(Read(game / "ERVR" / "ERVR.ini"), "VR", "WorldScale") == "0.8", "ini written");
    Check(Read(game / "ERVR" / "ERVR.ini").size() == before.size(), "one value changed, nothing else");

    refreshInstall(scan);
    change = uninstallErvr(scan);
    Check(change.ok && !Exists(game / "dinput8.dll") && !Exists(game / "ERVR" / "ERVR.dll"), "removed");
    Check(Exists(game / "dxgi.dll") && Read(game / "dxgi.dll") == Read(ERVR_TEST_FOREIGN_DLL), "the old dxgi.dll restored");
    Check(Exists(game / "ERVR" / "ERVR.ini"), "settings kept after removal");

    fs::remove_all(root);
    std::puts("launcher folder changes OK");
}
