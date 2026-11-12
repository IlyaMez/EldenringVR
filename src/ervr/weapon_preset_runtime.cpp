#include "weapon_preset_runtime.h"
#include "log.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>
#include <memory>

namespace ervr {
namespace {
struct Entry {
    uint64_t key = 0;
    ResolvedWeaponPreset preset;
};
struct Library {
    std::array<Entry, 2048> entries{};
    size_t count = 0;
    uint32_t revision = 0;
};
struct Applied {
    ResolvedWeaponPreset hands[2];
    uint32_t ids[2]{};
    uint16_t types[2]{};
};
WeaponSnapshot<Library> g_library;
WeaponSnapshot<Applied> g_applied;
bool g_waitNeutral = false;
Applied g_workerApplied; // pad worker only
struct Hand {
    std::atomic<uint64_t> identity{0};
    std::atomic<float> tip{0}, speed{0};
    std::atomic<int> event{-1}, action{0};
};
Hand g_hands[2];
// Process-scoped worker: the game's process-exit path must not destroy a joinable std::thread under DllMain.
std::thread* g_io = nullptr;
std::mutex g_stopMutex;
std::condition_variable g_stopCv;
bool g_stop = false;
uint64_t Key(uint32_t id, uint16_t type, int hand) {
    return (uint64_t)(id ? id : (0x80000000u | type)) * 4 + (hand + 1);
}
ResolvedWeaponPreset Find(const Library& lib, uint32_t id, uint16_t type, int hand) {
    for (uint64_t key : {Key(id, type, hand), Key(id, type, -1), Key(0, type, hand), Key(0, type, -1)}) {
        auto begin = lib.entries.begin(), end = begin + lib.count;
        auto it = std::lower_bound(begin, end, key, [](const Entry& e, uint64_t k) { return e.key < k; });
        if (it != end && it->key == key)
            return it->preset;
    }
    return {};
}
void WriteLive(const std::filesystem::path& dir) {
    const auto temp = dir / L"weapon-live.tmp", file = dir / L"weapon-live.txt";
    std::ofstream out(temp, std::ios::trunc);
    if (!out)
        return;
    out << GetCurrentProcessId() << ' ' << GetTickCount64() << '\n';
    for (const auto& h : g_hands) {
        const auto id = h.identity.load();
        out << (uint32_t)id << ' ' << (uint16_t)(id >> 32) << ' ' << h.tip.load() << ' ' << h.speed.load() << ' '
            << h.event.load() << ' ' << h.action.load() << '\n';
    }
    out.close();
    if (out)
        MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
}
void Run(std::filesystem::path dir) {
    const auto file = dir / L"Presets" / L"library.json";
    std::filesystem::file_time_type last{};
    bool seen = false;
    std::string lastError;
    for (;;) {
        try {
            std::error_code ec;
            const bool exists = std::filesystem::exists(file, ec);
            auto stamp = exists ? std::filesystem::last_write_time(file, ec) : std::filesystem::file_time_type{};
            if (!ec && (!seen || stamp != last)) {
                WeaponPresetStore store;
                std::string why;
                if (LoadWeaponPresetStore(file, store, why)) {
                    auto library = std::make_unique<Library>();
                    library->revision = store.revision;
                    for (const auto& a : store.assignments) {
                        auto& e = library->entries[library->count++];
                        e.key = Key(a.weapon, a.family, a.hand);
                        for (const auto& p : store.presets)
                            if (p.id == a.preset) {
                                e.preset = CompileWeaponPreset(p);
                                break;
                            }
                    }
                    std::sort(library->entries.begin(), library->entries.begin() + library->count,
                              [](const auto& a, const auto& b) { return a.key < b.key; });
                    if (g_library.Publish(*library)) {
                        last = stamp;
                        seen = true;
                        lastError.clear();
                        LOGI("[weapons] preset library revision %u loaded (%zu assignments)", store.revision,
                             store.assignments.size());
                    }
                } else if (why != lastError) {
                    LOGW("[weapons] keeping last valid preset library: %s", why.c_str());
                    lastError = why;
                }
            }
            WriteLive(dir);
        } catch (const std::exception& e) {
            LOGW("[weapons] preset IO: %s", e.what());
        }
        std::unique_lock<std::mutex> lock(g_stopMutex);
        if (g_stopCv.wait_for(lock, std::chrono::milliseconds(500), [] { return g_stop; }))
            break;
    }
    DeleteFileW((dir / L"weapon-live.txt").c_str());
}
} // namespace
void WeaponPresetRuntimeStart(const std::wstring& dir) {
    if (g_io)
        return;
    {
        std::lock_guard<std::mutex> lock(g_stopMutex);
        g_stop = false;
    }
    g_io = new std::thread(Run, std::filesystem::path(dir));
}
void WeaponPresetRuntimeStop() {
    {
        std::lock_guard<std::mutex> lock(g_stopMutex);
        g_stop = true;
    }
    g_stopCv.notify_all();
    if (g_io) {
        g_io->join();
        delete g_io;
        g_io = nullptr;
    }
}
void WeaponPresetEquipment(int hand, uint32_t id, uint16_t family) {
    if (hand < 0 || hand > 1)
        return;
    auto& h = g_hands[hand];
    const uint64_t identity = (uint64_t)family << 32 | id;
    if (h.identity.exchange(identity) != identity) {
        h.tip.store(0);
        h.speed.store(0);
        h.event.store(-1);
        h.action.store(0);
    }
}
uint32_t WeaponPresetEquippedId(int hand) {
    return hand >= 0 && hand < 2 ? (uint32_t)g_hands[hand].identity.load() : 0;
}
bool WeaponPresetApplyPending(bool neutral) {
    Applied next = g_workerApplied;
    bool changed = false, cancel = false, equipment = false;
    bool read = g_library.Read([&](const Library& lib) {
        for (int h = 0; h < 2; ++h) {
            const auto identity = g_hands[h].identity.load();
            const auto id = (uint32_t)identity;
            const auto type = (uint16_t)(identity >> 32);
            const auto desired = id ? Find(lib, id, type, h) : ResolvedWeaponPreset{};
            if (id != next.ids[h] || type != next.types[h]) {
                // Without an old or new assignment, preserve ERVR's original equipment behavior.
                cancel = cancel || next.hands[h].fingerprint != 0 || desired.fingerprint != 0;
                next.ids[h] = id;
                next.types[h] = type;
                next.hands[h] = {};
                changed = equipment = true;
            } else if (neutral && desired.fingerprint != next.hands[h].fingerprint) {
                next.hands[h] = desired;
                changed = cancel = true;
            }
        }
    });
    if (!read)
        return g_waitNeutral;
    if (equipment && cancel)
        g_waitNeutral = true;
    if (neutral && !equipment)
        g_waitNeutral = false;
    if (changed) {
        if (!g_applied.Publish(next))
            return true;
        g_workerApplied = next;
    }
    return cancel || g_waitNeutral;
}

ResolvedWeaponPreset AppliedWeaponPreset(int hand) {
    ResolvedWeaponPreset out;
    if (hand >= 0 && hand < 2)
        g_applied.Read([&](const Applied& p) { out = p.hands[hand]; });
    return out;
}
void WeaponPresetTrace(int hand, float tip, float speed, WeaponEvent event, WeaponAction action) {
    if (hand < 0 || hand > 1)
        return;
    auto& h = g_hands[hand];
    h.tip.store(tip);
    h.speed.store(speed);
    h.event.store((int)event);
    h.action.store((int)action);
}
} // namespace ervr
