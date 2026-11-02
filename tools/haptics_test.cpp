// ERVR's body haptics on bHaptics devices, without the game: whether the bHaptics Player and the devices
// are there, which motor is where, and every pattern the game plays, each named with what it should feel
// like. It plays through the game's own path (BodyMixer, BhapticsOutput). Ported from Spidy's
// apps/haptics_test.cpp.
//
//   ervr_haptics_test [check] [map] [effects] [stream]   (none: all four)
//     --only NAME        effects: just the one named (strikes, hits, ...)
//     --step             map: wait for Enter after each place
//     --level N          strength in percent (100)
//     --dry              no Player: the frames as text, 0 (.) to 9
//     --app-id ID --api-key KEY
//                        the bHaptics app, if not this build's (also the ERVR_BHAPTICS_APP_ID and
//                        ERVR_BHAPTICS_API_KEY variables)
#include "bhaptics_output.h"
#include "body_haptics.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace ervr;
namespace {
constexpr float kPi = 3.14159265f;
// Where the patterns go: the devices, or the console.
class Sink {
public:
    virtual ~Sink() = default;
    virtual void Post(const BodyEvent&) = 0;
    virtual void Set(const BodyState&) = 0;
    virtual void Hold(const BodyFrame&, float seconds) = 0;
    virtual void Wait(double seconds) = 0;
};
class Devices final : public Sink {
public:
    explicit Devices(BhapticsOutput& out) : out_(out) {}
    void Post(const BodyEvent& e) override { out_.Post(e); }
    void Set(const BodyState& s) override { state_ = s; out_.Set(s); }
    void Hold(const BodyFrame& f, float seconds) override { out_.Hold(f, seconds); }
    // The state again and again while it waits, as the pad worker gives it every tick: it lasts 300 ms.
    void Wait(double seconds) override {
        using clock = std::chrono::steady_clock;
        const auto end = clock::now() + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(seconds));
        for (auto now = clock::now(); now < end; now = clock::now()) {
            out_.Set(state_);
            std::this_thread::sleep_for(std::min<clock::duration>(end - now, std::chrono::milliseconds(20)));
        }
    }
private:
    BhapticsOutput& out_;
    BodyState state_;
};
// The frames as the output thread would make them, a line for each tick with a motor on: the vest's
// front and back (four rows, the wearer's left first), the left and right sleeve, the visor.
class Console final : public Sink {
public:
    explicit Console(int level) : level_(level) {}
    void Post(const BodyEvent& e) override { mixer_.Post(e, now_); }
    void Set(const BodyState& s) override { mixer_.Set(s); }
    void Hold(const BodyFrame& f, float seconds) override { mixer_.Hold(f, now_, seconds); }
    void Wait(double seconds) override {
        for (double end = now_ + seconds; now_ < end - 1e-9; now_ += .04) {
            const BodyFrame f = mixer_.Render(now_);
            bool any = false;
            for (unsigned d = 0; d < kBodyDevices; ++d) any = any || f.Any((BodyDevice)d);
            if (any) std::printf("%7.0f ms  %s\n", (now_ - since_) * 1000, Line(f).c_str());
        }
    }
    void Mark() { since_ = now_; }
private:
    // A motor as the output thread sends it, in tenths rounded up (1: 3-10%, 2: 11-20% ... 9: over 80%);
    // '.' off, or under kBhapticsMinimumLevel.
    char Digit(float v) const {
        const auto percent = (int)std::lround(std::clamp(v, 0.f, 1.f) * (float)level_);
        return percent < kBhapticsMinimumLevel ? '.' : (char)('0' + std::clamp((percent + 9) / 10, 1, 9));
    }
    std::string Line(const BodyFrame& f) const {
        std::string out;
        for (const bool back : {false, true}) {
            out += back ? " B[" : "F[";
            for (unsigned row = 0; row < 4; ++row) {
                for (unsigned side = 0; side < 4; ++side) out += Digit(f.At(BodyDevice::Vest, BodyVestMotor(back, row, side)));
                out += row < 3 ? "|" : "]";
            }
        }
        for (const auto& [label, device] : {std::pair{" L[", BodyDevice::LeftArm}, std::pair{" R[", BodyDevice::RightArm}}) {
            out += label;
            for (unsigned m = 0; m < 3; ++m) out += Digit(f.At(device, m));
            out += ']';
        }
        out += " H[";
        for (unsigned i = 0; i < 4; ++i) out += Digit(f.At(BodyDevice::Head, BodyVisorMotor(i)));
        return out + ']';
    }
    BodyMixer mixer_;
    double now_ = 0, since_ = 0;
    int level_;
};

BodyState Live() { BodyState s; s.live = true; return s; }
BodyEvent At(BodyKind kind, int hand = -1, float strength = 1, float azimuth = 0, float height = .4f, bool placed = true) {
    BodyEvent e; e.kind = kind; e.hand = (int8_t)hand; e.strength = strength; e.azimuth = azimuth; e.height = height; e.placed = placed;
    return e;
}
// Changes the state over `seconds`, `apply` given 0..1 along the way.
void Sweep(Sink& sink, double seconds, const std::function<void(BodyState&, float)>& apply) {
    const int steps = (int)(seconds / .04);
    for (int i = 0; i <= steps; ++i) {
        BodyState s = Live();
        apply(s, (float)i / (float)steps);
        sink.Set(s);
        sink.Wait(.04);
    }
    sink.Set(Live());
}

struct Demo { const char* name; const char* feel; std::function<void(Sink&)> play; };
const std::vector<Demo>& Demos() {
    static const std::vector<Demo> all = {
        {"strikes", "a lazy blow (LEFT arm), a full swing (RIGHT arm and shoulder), a two-handed one (both)",
         [](Sink& s) {
             s.Post(At(BodyKind::Strike, 0, .15f)); s.Wait(.6);
             s.Post(At(BodyKind::Strike, 1, 1.f)); s.Wait(.6);
             s.Post(At(BodyKind::Strike, -1, .8f)); s.Wait(.6);
         }},
        {"shield", "a block on your LEFT arm (and a little in the chest), then a deflect: sharp, both arms, the face",
         [](Sink& s) {
             s.Post(At(BodyKind::Block, 0, .6f)); s.Wait(.6);
             s.Post(At(BodyKind::Deflect, 0)); s.Wait(.7);
         }},
        {"two_hand", "both hands close on the weapon, then one lets go",
         [](Sink& s) {
             s.Post(At(BodyKind::TwoHand)); s.Wait(.5);
             s.Post(At(BodyKind::OneHand)); s.Wait(.5);
         }},
        {"winded", "a swing with no stamina: two soft heaves in your chest",
         [](Sink& s) { s.Post(At(BodyKind::Winded, 1)); s.Wait(.9); }},
        {"casts", "a spell leaves your RIGHT hand, then your LEFT",
         [](Sink& s) {
             s.Post(At(BodyKind::Cast, 1)); s.Wait(.5);
             s.Post(At(BodyKind::Cast, 0)); s.Wait(.5);
         }},
        {"hits", "hit from AHEAD, your LEFT, BEHIND, your RIGHT, from ABOVE (the shoulders), then a heavy one ahead "
                 "(the face and the arms too), then one from nowhere known (the chest and the back)",
         [](Sink& s) {
             for (const float az : {0.f, -kPi / 2, kPi, kPi / 2}) { s.Post(At(BodyKind::Hit, -1, .25f, az)); s.Wait(.8); }
             s.Post(At(BodyKind::Hit, -1, .25f, 0.f, BodyHitHeight(4.f, 2.f))); s.Wait(.8);
             s.Post(At(BodyKind::Hit, -1, 1.f, 0.f)); s.Wait(.8);
             s.Post(At(BodyKind::Hit, -1, .25f, 0.f, .4f, false)); s.Wait(.8);
         }},
        {"heal", "a flask: warmth spreading out from the middle of your chest, then round your back",
         [](Sink& s) { s.Post(At(BodyKind::Heal, -1, .8f)); s.Wait(1.2); }},
        {"low_hp", "low HP: the heart racing, high on your chest's left",
         [](Sink& s) { Sweep(s, 3.5, [](BodyState& st, float) { st.health = .15f; }); s.Wait(.3); }},
        {"death", "death: everything at once, let go of slowly, the head first",
         [](Sink& s) { s.Post(At(BodyKind::Death)); s.Wait(2.); }},
        {"fall", "a long fall: a faint wind round your waist, growing; then the landing, from the waist up",
         [](Sink& s) {
             Sweep(s, 2.5, [](BodyState& st, float u) { st.airborne = true; st.velocity = {0, -(10 + 30 * u), 0}; });
             s.Post(At(BodyKind::Landing, -1, .9f)); s.Wait(.8);
         }},
        {"landings", "a short drop: the waist's row only; then a hard one: from the waist up",
         [](Sink& s) {
             s.Post(At(BodyKind::Landing, -1, .3f)); s.Wait(.8);
             s.Post(At(BodyKind::Landing, -1, 1.f)); s.Wait(.8);
         }},
        {"rumble", "the game's rumble: the heavy motor round your waist, the light one at your shoulders, then both",
         [](Sink& s) {
             Sweep(s, 1.2, [](BodyState& st, float u) { st.rumble = {std::sin(u * kPi), 0.f}; });
             Sweep(s, 1.2, [](BodyState& st, float u) { st.rumble = {0.f, std::sin(u * kPi)}; });
             Sweep(s, 1.2, [](BodyState& st, float) { st.rumble = {.8f, .8f}; });
             s.Wait(.3);
         }},
        {"hello", "the devices found: the whole body twice",
         [](Sink& s) { s.Post(At(BodyKind::Hello)); s.Wait(1.); }},
    };
    return all;
}

// One place at a time: what should be felt where.
void Map(Sink& sink, bool step, bool dry) {
    struct Place { std::string what; BodyFrame frame; };
    std::vector<Place> places;
    const float strength = .6f;
    for (const bool back : {false, true})
        for (const unsigned rows : {0u, 2u})
            for (const unsigned sides : {0u, 2u}) {
                Place p;
                p.what = std::string("vest ") + (back ? "BACK" : "FRONT") + ", " + (rows ? "lower" : "upper") + " " +
                         (sides ? "RIGHT" : "LEFT") + " (your " + (sides ? "right" : "left") + ")";
                for (unsigned row = rows; row < rows + 2; ++row)
                    for (unsigned side = sides; side < sides + 2; ++side) p.frame.At(BodyDevice::Vest, BodyVestMotor(back, row, side)) = strength;
                places.push_back(p);
            }
    for (const unsigned side : {0u, 1u, 2u, 3u}) {
        Place p;
        p.what = "vest, one column front and back, " + std::to_string(side + 1) + " of 4 from your LEFT";
        for (unsigned row = 0; row < 4; ++row)
            for (const bool back : {false, true}) p.frame.At(BodyDevice::Vest, BodyVestMotor(back, row, side)) = strength;
        places.push_back(p);
    }
    for (const int hand : {0, 1}) {
        Place whole;
        whole.what = std::string(hand ? "RIGHT" : "LEFT") + " sleeve, all three motors";
        for (unsigned m = 0; m < 3; ++m) whole.frame.At(BodyArm(hand), m) = strength;
        places.push_back(whole);
        for (unsigned m = 0; m < 3; ++m) {
            Place one;
            one.what = std::string(hand ? "RIGHT" : "LEFT") + " sleeve, motor " + std::to_string(m) + " alone (where on the arm?)";
            one.frame.At(BodyArm(hand), m) = strength;
            places.push_back(one);
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        Place p;
        p.what = "visor, motor " + std::to_string(i + 1) + " of 4 from your LEFT temple";
        p.frame.At(BodyDevice::Head, BodyVisorMotor(i)) = strength;
        places.push_back(p);
    }
    sink.Set(Live());
    for (const auto& p : places) {
        std::cout << "  " << p.what << std::endl;
        sink.Hold(p.frame, .9f);
        sink.Wait(1.5);
        if (step && !dry) {
            std::cout << "    (Enter for the next)" << std::flush;
            std::string ignored;
            std::getline(std::cin, ignored);
        }
    }
}
// A steady frame, then a slow ramp: whether frames sent a tick at a time feel like one steady buzz, or pulse.
void Stream(Sink& sink) {
    sink.Set(Live());
    BodyFrame whole;
    for (unsigned d = 0; d < kBodyDevices; ++d)
        for (unsigned m = 0; m < BodyMotorCount((BodyDevice)d); ++m) whole.At((BodyDevice)d, m) = .4f;
    std::cout << "  every motor at 40% for 3 seconds: one steady buzz? (a pulse or a flicker: say so)" << std::endl;
    sink.Hold(whole, 3);
    sink.Wait(3.4);
    std::cout << "  the whole vest from 10% up to 100% and down again over 4 seconds: smooth?" << std::endl;
    for (int i = 0; i <= 40; ++i) {
        const float u = (float)i / 40;
        BodyFrame f;
        for (unsigned m = 0; m < 32; ++m) f.At(BodyDevice::Vest, m) = .1f + .9f * std::sin(u * kPi);
        sink.Hold(f, .1f);
        sink.Wait(.1);
    }
    sink.Wait(.3);
}
const char* Env(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? v : nullptr;
}
} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> sections;
    std::string only, appId = BhapticsBuiltAppId(), apiKey = BhapticsBuiltApiKey();
    bool dry = false, step = false;
    int level = 100;
    if (const char* v = Env("ERVR_BHAPTICS_APP_ID")) appId = v;
    if (const char* v = Env("ERVR_BHAPTICS_API_KEY")) apiKey = v;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool value = i + 1 < argc;
        if (a == "--dry") dry = true;
        else if (a == "--step") step = true;
        else if (a == "--only" && value) only = argv[++i];
        else if (a == "--level" && value) level = std::clamp(std::atoi(argv[++i]), 0, 100);
        else if (a == "--app-id" && value) appId = argv[++i];
        else if (a == "--api-key" && value) apiKey = argv[++i];
        else if (a == "check" || a == "map" || a == "effects" || a == "stream") sections.push_back(a);
        else {
            std::cerr << "usage: ervr_haptics_test [check] [map] [effects] [stream] [--only NAME] [--step] "
                         "[--level N] [--dry] [--app-id ID --api-key KEY]\neffects:";
            for (const auto& d : Demos()) std::cerr << ' ' << d.name;
            std::cerr << '\n';
            return 2;
        }
    }
    if (sections.empty()) sections = {"check", "map", "effects", "stream"};
    const auto wants = [&](const char* s) { return std::find(sections.begin(), sections.end(), s) != sections.end(); };

    BhapticsOutput out;
    Console console(level);
    Devices devices(out);
    Sink& sink = dry ? (Sink&)console : (Sink&)devices;
    int result = 0;
    if (!dry) {
        BhapticsConfig config;
        config.library = BhapticsLibraryBeside();
        config.appId = appId;
        config.apiKey = apiKey;
        std::wcout << L"bHaptics library: " << config.library << std::endl;
        std::cout << "bHaptics app: " << (appId.empty() ? "none - give --app-id and --api-key" : appId)
                  << (apiKey.empty() ? ", no API key" : ", API key set") << std::endl;
        out.Start(config);
        out.Level(level);
        out.Set(Live());
        // The Player connects in the background: up to fifteen seconds, then a moment more for the
        // devices to be asked for.
        BhapticsStatus shown = BhapticsStatus::Off;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        for (;;) {
            const auto c = out.Counters();
            if (c.status != shown) { shown = c.status; std::cout << "status: " << BhapticsStatusName(shown) << std::endl; }
            const bool done = c.status == BhapticsStatus::Playing || c.status == BhapticsStatus::NoLibrary ||
                              c.status == BhapticsStatus::NoApp || c.status == BhapticsStatus::NoPlayer ||
                              c.status == BhapticsStatus::Failed || c.status == BhapticsStatus::Rejected;
            if (done || std::chrono::steady_clock::now() >= deadline) break;
            out.Set(Live());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto c = out.Counters();
        std::cout << "devices on: " << BhapticsDeviceNames(c.devices) << std::endl;
        switch (c.status) {
        case BhapticsStatus::NoLibrary:
            std::cout << "bhaptics_library.dll is missing beside this program, or is not the bHaptics SDK's." << std::endl;
            return 2;
        case BhapticsStatus::NoApp:
            std::cout << "This build has no bHaptics app: give --app-id and --api-key (the Developer Portal app's Settings)." << std::endl;
            return 2;
        case BhapticsStatus::NoPlayer:
            std::cout << "The bHaptics Player is not installed: bhaptics.com/software/player" << std::endl;
            return 1;
        case BhapticsStatus::Failed:
            std::cout << "The bHaptics SDK would not start." << std::endl;
            return 1;
        case BhapticsStatus::Rejected:
            std::cout << "The bHaptics Player did not take this app: give the workspace ID and API key from the "
                         "Developer Portal (not the app's own ID), and deploy the app with at least one event." << std::endl;
            return 1;
        case BhapticsStatus::Waiting:
            std::cout << "No answer from the bHaptics Player: is it running? Is the app deployed on the Developer "
                         "Portal, with at least one event?" << std::endl;
            return 1;
        case BhapticsStatus::Connected:
            std::cout << "Connected, but no device is on: pair them in the bHaptics Player." << std::endl;
            result = 1;
            break;
        default:
            break;
        }
        if (wants("check")) {
            const auto info = out.DeviceInfo();
            std::cout << "the Player's devices: " << (info.empty() ? "(nothing)" : info) << std::endl;
        }
    }
    Console* shown = dry ? &console : nullptr;
    if (wants("map")) {
        std::cout << "\nMAP: each place for a moment, at 60%. Say where you feel it, if it is not there." << std::endl;
        if (shown) shown->Mark();
        Map(sink, step, dry);
    }
    if (wants("effects")) {
        std::cout << "\nEFFECTS:" << std::endl;
        bool found = only.empty();
        for (const auto& d : Demos()) {
            if (!only.empty() && only != d.name) continue;
            found = true;
            std::cout << "  " << d.name << ": " << d.feel << std::endl;
            if (shown) shown->Mark();
            sink.Set(Live());
            d.play(sink);
            sink.Wait(.4);
        }
        if (!found) { std::cerr << "no effect " << only << '\n'; return 2; }
    }
    if (wants("stream")) {
        std::cout << "\nSTREAM:" << std::endl;
        if (shown) shown->Mark();
        Stream(sink);
    }
    if (!dry) {
        const auto c = out.Counters();
        std::cout << "\nframes sent " << c.frames << ", refused " << c.refused << ", devices seen " << BhapticsDeviceNames(c.devicesSeen) << std::endl;
        result = c.refused ? 1 : result;
        out.Stop();
    }
    return result;
}
