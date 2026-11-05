// End-to-end check of the launcher's updater against the real GitHub release (needs the network; not a ctest test).
// Built with ERVR_VERSION 0.0.1, so whatever GitHub calls latest is "newer". Copy it into an unpacked older ERVR
// folder as "ERVR Launcher.exe" and run it there: it checks, downloads, verifies and swaps that folder's files -
// itself included, as the real launcher does - then prints what happened.
//   updater_e2e.exe [folder]   (default: the folder it runs from)
#include "update.hpp"
#include "system.hpp"
#include <cstdio>
#include <filesystem>

using namespace launcher;

static const char* name(UpdateState s) {
    switch (s) {
    case UpdateState::idle: return "idle";
    case UpdateState::checking: return "checking";
    case UpdateState::current: return "current";
    case UpdateState::available: return "available";
    case UpdateState::checkFailed: return "checkFailed";
    case UpdateState::downloading: return "downloading";
    case UpdateState::installing: return "installing";
    case UpdateState::installed: return "installed";
    case UpdateState::installFailed: return "installFailed";
    }
    return "?";
}

int wmain(int argc, wchar_t** argv) {
    std::string whyNot;
    const std::wstring own = installFolder(&whyNot);
    std::printf("installFolder(): %s%s\n", own.empty() ? "(none) " : narrow(own).c_str(), whyNot.c_str());
    const std::wstring folder =
        argc > 1 ? std::wstring(argv[1]) : std::filesystem::path(argv[0]).parent_path().wstring();
    std::printf("updating %s as version %s\n", narrow(folder).c_str(), kVersion);
    Updater updater;
    updater.start(folder, true);
    UpdateStatus s;
    for (int i = 0; i < 600 && (s = updater.status()).state == UpdateState::checking; ++i)
        Sleep(100);
    std::printf("check: %s %s%s\n", name(s.state), s.release ? s.release->version.c_str() : "", s.message.c_str());
    if (s.state != UpdateState::available)
        return 2;
    std::printf("zip %s, %llu bytes, sha256 %s\n", s.release->zipName.c_str(),
                static_cast<unsigned long long>(s.release->zipSize), s.release->sha256.c_str());
    updater.install();
    UpdateState last = UpdateState::idle;
    for (int i = 0; i < 3000; ++i) {
        s = updater.status();
        if (s.state != last) {
            std::printf("  -> %s (%llu / %llu) %s\n", name(s.state), static_cast<unsigned long long>(s.received),
                        static_cast<unsigned long long>(s.total), s.message.c_str());
            last = s.state;
        }
        if (s.state == UpdateState::installed || s.state == UpdateState::installFailed)
            break;
        Sleep(100);
    }
    return s.state == UpdateState::installed ? 0 : 1;
}
