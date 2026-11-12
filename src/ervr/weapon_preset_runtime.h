#pragma once
#include "weapon_presets.h"
#include <atomic>
#include <array>

namespace ervr {
// One writer, arbitrary short readers. Generation tokens prevent ABA while a reader acquires a slot.
// Sequentially consistent counters/publication make the writer's zero-reader check and acquisition ordered.
// Nothing allocates, waits for another thread, or reclaims memory on the reader path.
template <class T> class WeaponSnapshot {
    struct Slot {
        std::atomic<uint32_t> readers{0};
        T value{};
    };
    mutable std::array<Slot, 3> slots_{};
    std::atomic<uint64_t> token_{0};

  public:
    bool Publish(const T& value) {
        const auto token = token_.load();
        for (size_t i = 0; i < 3; ++i)
            if (i != token % 3 && slots_[i].readers.load() == 0) {
                slots_[i].value = value;
                token_.store((token / 3 + 1) * 3 + i);
                return true;
            }
        return false;
    }
    template <class F> bool Read(F&& consume) const {
        for (int attempt = 0; attempt < 4; ++attempt) {
            const auto token = token_.load();
            auto& slot = slots_[token % 3];
            slot.readers.fetch_add(1);
            if (token == token_.load()) {
                consume(slot.value);
                slot.readers.fetch_sub(1);
                return true;
            }
            slot.readers.fetch_sub(1);
        }
        return false;
    }
};
void WeaponPresetRuntimeStart(const std::wstring& ervrDirectory);
void WeaponPresetRuntimeStop();
// Game thread publishes validated identity; no IO. ID is normalized through originEquipWep.
void WeaponPresetEquipment(int hand, uint32_t weapon, uint16_t family);
// Pad worker only. True means context changed: cancel held/queued actions for this tick.
// New tuning waits for neutral, between actions. Rendering reads only the applied snapshot.
bool WeaponPresetApplyPending(bool neutral);
ResolvedWeaponPreset AppliedWeaponPreset(int hand);
uint32_t WeaponPresetEquippedId(int hand);
void WeaponPresetTrace(int hand, float tip, float handSpeed, WeaponEvent event, WeaponAction action);
} // namespace ervr
