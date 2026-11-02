// Body haptics on bHaptics devices (bhaptics_output.h). The SDK's header only names its functions:
// they come from the library, loaded at run time.
#define BHAPTICS_DECLSPEC
#include <bhaptics/bhaptics.h>

#include "bhaptics_output.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <vector>

#ifndef ERVR_BHAPTICS_APP_ID
#define ERVR_BHAPTICS_APP_ID ""
#endif
#ifndef ERVR_BHAPTICS_API_KEY
#define ERVR_BHAPTICS_API_KEY ""
#endif

namespace ervr {
namespace {
// The SDK's functions ERVR calls, from the library.
struct Api {
    decltype(&::registryAndInit) registryAndInit{};
    decltype(&::wsIsConnected) wsIsConnected{};
    decltype(&::wsClose) wsClose{};
    decltype(&::bHapticsShutdown) shutdown{};
    decltype(&::playDot) playDot{};
    decltype(&::stop) stop{};
    decltype(&::stopAll) stopAll{};
    decltype(&::isbHapticsConnected) isbHapticsConnected{};
    decltype(&::isPlayerInstalled) isPlayerInstalled{};
    decltype(&::getDeviceInfoJson) getDeviceInfoJson{};
    decltype(&::getHapticMappingsJson) getHapticMappingsJson{};
    bool Load(HMODULE m) {
        bool all = true;
        const auto get = [&](auto& f, const char* name) {
            f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(GetProcAddress(m, name));
            all = all && f;
        };
        get(registryAndInit, "registryAndInit");
        get(wsIsConnected, "wsIsConnected");
        get(wsClose, "wsClose");
        get(shutdown, "bHapticsShutdown");
        get(playDot, "playDot");
        get(stop, "stop");
        get(stopAll, "stopAll");
        get(isbHapticsConnected, "isbHapticsConnected");
        get(isPlayerInstalled, "isPlayerInstalled");
        get(getDeviceInfoJson, "getDeviceInfoJson");
        get(getHapticMappingsJson, "getHapticMappingsJson");
        return all;
    }
};
// Each device's frames play under a request id of its own ("ER" and the device).
constexpr int32_t StreamId(BodyDevice d) { return 0x4552 + (int32_t)d; }
// The positions of the devices the Player calls connected, as bits 1 << position, from its device
// list (getDeviceInfoJson: an object for each, with its "position" and "connected"). The SDK's own
// isbHapticsConnected said false for every position on Spidy's devkit (2026-10-20) while the list had a
// TactSuit Pro (0) and both TactSleeves (1, 2) connected and playDot was taken for each.
uint32_t ConnectedPositions(const char* json) {
    if (!json) return 0;
    const std::string_view text(json);
    // An object's value for a key: from its first character.
    const auto value = [](std::string_view object, std::string_view key) -> std::string_view {
        const size_t at = object.find(key);
        if (at == std::string_view::npos) return {};
        const size_t colon = object.find(':', at + key.size());
        const size_t start = colon == std::string_view::npos ? colon : object.find_first_not_of(" \t\r\n", colon + 1);
        return start == std::string_view::npos ? std::string_view{} : object.substr(start);
    };
    uint32_t bits = 0;
    for (size_t open = text.find('{'); open != std::string_view::npos; open = text.find('{', open + 1)) {
        const size_t close = text.find('}', open);
        if (close == std::string_view::npos) break;
        const auto object = text.substr(open, close - open);
        const auto position = value(object, "\"position\"");
        if (value(object, "\"connected\"").substr(0, 4) == "true" && !position.empty() && position[0] >= '0' &&
            position[0] <= '3' && (position.size() < 2 || position[1] < '0' || position[1] > '9'))
            bits |= 1u << (position[0] - '0');
        open = close;
    }
    return bits;
}
constexpr size_t kMostWaiting = 256;        // events waiting for the thread at most: it takes them 25 times a second
constexpr uint64_t kDevicePoll = 25;        // ticks between asking again for the devices while connected
constexpr auto kStopWait = std::chrono::seconds(3);
// A state lasts this long unless given again: the pad worker sends one every tick, and one that stops
// leaves nothing playing.
constexpr auto kStateLease = std::chrono::milliseconds(300);
} // namespace

struct BhapticsOutput::Shared {
    BhapticsConfig config;
    std::mutex lock;
    std::condition_variable wake;
    bool stopping = false, finished = false;
    Api api;
    bool loaded = false;   // the api is there, and the SDK not shut down
    std::vector<BodyEvent> events;
    struct Held { BodyFrame frame; float seconds = 0; };
    std::vector<Held> holds;
    BodyState state;
    std::chrono::steady_clock::time_point stateAt{};
    int level = 100;
    BhapticsCounters counters;
    void Status(BhapticsStatus s) { std::lock_guard<std::mutex> g(lock); counters.status = s; }
    void Run();
};

const char* BhapticsStatusName(BhapticsStatus s) {
    switch (s) {
    case BhapticsStatus::Off: return "off";
    case BhapticsStatus::Starting: return "starting";
    case BhapticsStatus::NoLibrary: return "no library";
    case BhapticsStatus::NoApp: return "no app";
    case BhapticsStatus::NoPlayer: return "no player";
    case BhapticsStatus::Waiting: return "waiting";
    case BhapticsStatus::Connected: return "connected";
    case BhapticsStatus::Playing: return "playing";
    case BhapticsStatus::Failed: return "failed";
    case BhapticsStatus::Rejected: return "rejected";
    }
    return "unknown";
}
const char* BhapticsBuiltAppId() { return ERVR_BHAPTICS_APP_ID; }
const char* BhapticsBuiltApiKey() { return ERVR_BHAPTICS_API_KEY; }
std::wstring BhapticsLibraryBeside() {
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&BhapticsLibraryBeside), &self))
        return {};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring folder(path, n);
    folder.resize(folder.find_last_of(L"\\/") + 1);
    return folder + L"bhaptics_library.dll";
}
std::string BhapticsDeviceNames(uint32_t devices) {
    constexpr const char* kNames[] = {"TactSuit", "left TactSleeve", "right TactSleeve", "TactVisor"};
    std::vector<const char*> on;
    for (unsigned d = 0; d < kBodyDevices; ++d)
        if (devices >> d & 1) on.push_back(kNames[d]);
    if ((devices & 6) == 6) {   // both sleeves are a pair
        on.erase(std::remove(on.begin(), on.end(), kNames[2]), on.end());
        *std::find(on.begin(), on.end(), kNames[1]) = "both TactSleeves";
    }
    std::string text;
    for (size_t i = 0; i < on.size(); ++i) text += (i == 0 ? "" : i + 1 == on.size() ? " and " : ", ") + std::string(on[i]);
    return text.empty() ? "none" : text;
}

void BhapticsOutput::Start(const BhapticsConfig& config) {
    if (shared_) return;
    shared_ = std::make_shared<Shared>();
    shared_->config = config;
    shared_->config.tickMs = std::clamp(config.tickMs, 10, 200);
    shared_->config.frameMs = std::clamp(config.frameMs, shared_->config.tickMs, 1000);
    if (config.appId.empty() || config.apiKey.empty()) {
        shared_->counters.status = BhapticsStatus::NoApp;
        shared_->finished = true;
        return;
    }
    shared_->counters.status = BhapticsStatus::Starting;
    thread_ = std::thread([shared = shared_] { shared->Run(); });
}
void BhapticsOutput::Stop() {
    if (!shared_) return;
    std::unique_lock<std::mutex> g(shared_->lock);
    shared_->stopping = true;
    shared_->wake.notify_all();
    const bool finished = shared_->wake.wait_for(g, kStopWait, [&] { return shared_->finished; });
    g.unlock();
    if (thread_.joinable()) {
        if (finished) thread_.join();
        else thread_.detach();   // it holds the shared state, and the library stays loaded
    }
}
void BhapticsOutput::Level(int percent) {
    if (!shared_) return;
    std::lock_guard<std::mutex> g(shared_->lock);
    shared_->level = std::clamp(percent, 0, 100);
    shared_->counters.level = (uint32_t)shared_->level;
}
void BhapticsOutput::Post(const BodyEvent& e) {
    if (!shared_ || (unsigned)e.kind >= kBodyKinds) return;
    std::lock_guard<std::mutex> g(shared_->lock);
    ++shared_->counters.events;
    ++shared_->counters.kinds[(unsigned)e.kind];
    if (!shared_->finished && shared_->events.size() < kMostWaiting) shared_->events.push_back(e);
}
void BhapticsOutput::Set(const BodyState& s) {
    if (!shared_) return;
    std::lock_guard<std::mutex> g(shared_->lock);
    shared_->state = s;
    shared_->stateAt = std::chrono::steady_clock::now();
}
void BhapticsOutput::Hold(const BodyFrame& f, float seconds) {
    if (!shared_) return;
    std::lock_guard<std::mutex> g(shared_->lock);
    if (!shared_->finished && shared_->holds.size() < kMostWaiting) shared_->holds.push_back({f, seconds});
}
BhapticsCounters BhapticsOutput::Counters() const {
    if (!shared_) return {};
    std::lock_guard<std::mutex> g(shared_->lock);
    return shared_->counters;
}
std::string BhapticsOutput::DeviceInfo() {
    if (!shared_) return {};
    std::lock_guard<std::mutex> g(shared_->lock);
    // The SDK takes calls from any thread, one at a time.
    if (!shared_->loaded || shared_->stopping || !shared_->api.wsIsConnected()) return {};
    const char* text = shared_->api.getDeviceInfoJson();
    return text ? text : "";
}

void BhapticsOutput::Shared::Run() {
    using namespace std::chrono;
    struct Finish {
        Shared& s;
        ~Finish() {
            std::lock_guard<std::mutex> g(s.lock);
            s.finished = true; s.loaded = false;
            s.events.clear(); s.holds.clear();
            s.wake.notify_all();
        }
    } finish{*this};
    const HMODULE module = LoadLibraryExW(config.library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    Api sdk;
    if (!module || !sdk.Load(module)) { Status(BhapticsStatus::NoLibrary); return; }
    { std::lock_guard<std::mutex> g(lock); api = sdk; loaded = true; }
    // Shut down on every way out once the SDK may have started: it has threads of its own. Not under the
    // lock, which the pad worker takes every tick: the SDK joins its worker here.
    struct Shutdown {
        Shared& s;
        ~Shutdown() {
            { std::lock_guard<std::mutex> g(s.lock); s.loaded = false; }
            s.api.shutdown();
        }
    } shutdown{*this};
    if (!sdk.isPlayerInstalled()) { Status(BhapticsStatus::NoPlayer); return; }
    // It connects in the background, and keeps reconnecting: a Player started later, or again, is found by itself.
    if (!sdk.registryAndInit(config.apiKey.c_str(), config.appId.c_str(), "")) { Status(BhapticsStatus::Failed); return; }
    Status(BhapticsStatus::Waiting);
    BodyMixer mixer;
    const auto origin = steady_clock::now();
    auto next = origin;
    std::vector<BodyEvent> posted;
    std::vector<Held> held;
    std::array<bool, kBodyDevices> sending{};
    uint32_t devices = 0;
    bool connected = false;
    // Whether the Player took the app: it sends the app's event list as it does (within a fifth of a
    // second on Spidy's devkit), and nothing for an app its server does not know.
    bool accepted = false;
    auto connectedAt = origin;
    for (uint64_t tick = 0;; ++tick) {
        BodyState now;
        int percent = 0;
        {
            std::unique_lock<std::mutex> g(lock);
            if (wake.wait_until(g, next, [this] { return stopping; })) break;
            posted.swap(events);
            held.swap(holds);
            now = state;
            now.live = now.live && steady_clock::now() - stateAt < kStateLease;
            percent = level;
        }
        const double clock = duration<double>(steady_clock::now() - origin).count();
        mixer.Set(now);
        for (const auto& e : posted) mixer.Post(e, clock);
        for (const auto& h : held) mixer.Hold(h.frame, clock, h.seconds);
        posted.clear(); held.clear();
        const BodyFrame frame = mixer.Render(clock);
        const bool connecting = sdk.wsIsConnected();
        if (connecting && !connected) connectedAt = steady_clock::now();
        if (!connecting) accepted = false;
        else if (!accepted && (!connected || tick % 5 == 0)) {
            const char* list = sdk.getHapticMappingsJson();
            accepted = list && std::strlen(list) > 2;   // "[]" or nothing: no list
        }
        // The Player lists its devices a second or so after it connects: asked for five times a second
        // until there are some.
        if (connecting && (!connected || tick % kDevicePoll == 0 || (!devices && tick % 5 == 0))) {
            devices = ConnectedPositions(sdk.getDeviceInfoJson());
            for (unsigned d = 0; d < kBodyDevices; ++d)
                if (sdk.isbHapticsConnected(BodySdkPosition((BodyDevice)d))) devices |= 1u << d;
        }
        if (!connecting) devices = 0;
        uint64_t sent = 0, refused = 0;
        for (unsigned d = 0; d < kBodyDevices; ++d) {
            const auto device = (BodyDevice)d;
            const unsigned motors = BodyMotorCount(device);
            int32_t values[kBodyMaxMotors]{};
            int32_t peak = 0;
            for (unsigned m = 0; m < motors; ++m) {
                const auto v = (int32_t)std::lround(std::clamp(frame.At(device, m), 0.f, 1.f) * (float)percent);
                values[m] = v >= kBhapticsMinimumLevel ? v : 0;
                peak = std::max(peak, values[m]);
            }
            const bool on = connecting && (devices >> d & 1) && peak > 0;
            // The last frame goes before the next plays, and at once when the device goes quiet (a
            // loading screen), not when it runs out.
            if (sending[d] && connecting) sdk.stop(StreamId(device));
            if (on) {
                if (sdk.playDot(StreamId(device), BodySdkPosition(device), config.frameMs, values, (int32_t)motors) < 0) ++refused;
                else ++sent;
            }
            sending[d] = on;
        }
        {
            std::lock_guard<std::mutex> g(lock);
            counters.connects += connecting && !connected;
            counters.devices = devices;
            counters.devicesSeen |= devices;
            counters.frames += sent;
            counters.refused += refused;
            // Connected without a device only once the Player has had time to list them.
            const auto since = steady_clock::now() - connectedAt;
            counters.status = !connecting                                    ? BhapticsStatus::Waiting
                            : !accepted && since > std::chrono::seconds(5)   ? BhapticsStatus::Rejected
                            : devices                                        ? BhapticsStatus::Playing
                            : since > std::chrono::seconds(3)                ? BhapticsStatus::Connected
                                                                             : BhapticsStatus::Waiting;
        }
        connected = connecting;
        // Never a burst to catch up: a late tick is followed by a whole one.
        next += milliseconds(config.tickMs);
        if (const auto t = steady_clock::now(); next < t) next = t;
    }
    if (connected) sdk.stopAll();
    sdk.wsClose();
}

}
