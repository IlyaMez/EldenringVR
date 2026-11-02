#pragma once
// Body haptics on bHaptics devices (body_haptics.h) through the bHaptics Player, with bHaptics' own
// SDK (github.com/bhaptics/tact-cpp2, v2.7.1, fetched by CMake). Its bhaptics_library.dll is loaded
// only once body haptics are on, from beside ERVR.dll, so a missing library costs only the haptics.
// Everything runs on a thread of its own: every SDK call waits for the SDK's own worker, and neither
// the game thread nor the pad worker may. Ported from Spidy (../Spidy/src/bhaptics_output.cpp).
//
// The thread mixes what it was given (BodyMixer) every `tickMs` and sends each connected device its
// frame (the SDK's playDot), each playing for `frameMs`, longer than a tick, so a late tick leaves no
// gap. Every device keeps one request id for its frames, and its last frame is stopped before the next
// plays, so one plays at a time: whether the Player replaces a playing frame under the same id, or
// plays both, is not to be seen from outside (Spidy's 70 ms frames every 40 ms overlapped for 30 ms of
// each tick until 2026-10-20). A motor under kBhapticsMinimumLevel percent is sent as off: too weak to
// turn, and it would keep a device streaming for nothing.
//
// The Player knows an app by the workspace ID and API key of an app on bHaptics' Developer Portal
// (developer.bhaptics.com), deployed with at least one event, which ERVR never plays. bHaptics' server
// answers "No workspace" for any other ID, the app's own record ID among them (Spidy, 2026-10-20);
// taken, the Player sends the app's event list as it connects, which is how the thread tells a Player
// that took the app from one that did not (BhapticsStatus::Rejected). A build gets them from
// ERVR_BHAPTICS_APP_ID / ERVR_BHAPTICS_API_KEY (CMakeLists.txt); one without them plays nothing.
#include "body_haptics.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace ervr {

enum class BhapticsStatus : uint32_t {
    Off,        // not started: body haptics have not been on
    Starting,   // the library loads
    NoLibrary,  // no bhaptics_library.dll, or one without the SDK's functions
    NoApp,      // this build has no bHaptics app (its App ID and API key)
    NoPlayer,   // the bHaptics Player is not installed
    Waiting,    // waiting for the Player: not running, or not answering yet
    Connected,  // connected, without a device on
    Playing,    // connected, with a device on
    Failed,     // the SDK would not start
    Rejected,   // connected, but the Player sent no event list for the app within 5 s: it did not take the app
};
const char* BhapticsStatusName(BhapticsStatus);

struct BhapticsConfig {
    std::wstring library;        // bhaptics_library.dll, a full path
    std::string appId, apiKey;   // the Developer Portal app
    int tickMs = 40, frameMs = 100;
};
constexpr int kBhapticsMinimumLevel = 3;

struct BhapticsCounters {
    BhapticsStatus status{};
    // Devices on now and at any time (bit 1 << BodyDevice); the times the Player was connected; the
    // level now, percent.
    uint32_t devices = 0, devicesSeen = 0, connects = 0, level = 0;
    // Events posted, device frames sent and those the SDK did not send.
    uint64_t events = 0, frames = 0, refused = 0;
    std::array<uint32_t, kBodyKinds> kinds{};
};

class BhapticsOutput {
public:
    BhapticsOutput() = default;
    BhapticsOutput(const BhapticsOutput&) = delete;
    BhapticsOutput& operator=(const BhapticsOutput&) = delete;
    ~BhapticsOutput() { Stop(); }
    // Starts the thread; it loads the library and connects. Once: a stopped output does not start again.
    void Start(const BhapticsConfig&);
    // Lets the devices go quiet and disconnects, waiting a few seconds at most. The library stays
    // loaded until the process ends: a library unloaded under threads of its own is a crash waiting
    // for its moment.
    void Stop();
    bool Started() const { return shared_ != nullptr; }
    void Level(int percent);   // 0-100 percent: 0 sends nothing
    void Post(const BodyEvent&);
    // Lasts 300 ms: given every tick, as the pad worker does; a loop that stops leaves the devices quiet.
    void Set(const BodyState&);
    void Hold(const BodyFrame&, float seconds);
    BhapticsCounters Counters() const;
    // The Player's own words about its devices (getDeviceInfoJson), for the test tool. Empty while the
    // Player is not connected.
    std::string DeviceInfo();

private:
    // What the callers and the thread share. The thread holds it too: one that does not finish within
    // a few seconds of Stop() (an SDK call that hangs) is let go of, and must find it still there.
    struct Shared;
    std::shared_ptr<Shared> shared_;
    std::thread thread_;
};

// The app this build was made for (ERVR_BHAPTICS_APP_ID / _API_KEY); empty in a build without.
const char* BhapticsBuiltAppId();
const char* BhapticsBuiltApiKey();
// bhaptics_library.dll beside the module (DLL or program) this code is in.
std::wstring BhapticsLibraryBeside();
// The devices in `devices` (bit 1 << BodyDevice) by the names the Player knows them by:
// "TactSuit, both TactSleeves and TactVisor"; "none".
std::string BhapticsDeviceNames(uint32_t devices);

}
