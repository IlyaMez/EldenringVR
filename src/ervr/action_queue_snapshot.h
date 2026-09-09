#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ervr {

constexpr uint32_t ActionRequestTransientCancels = 0x27e;   // TAE bits 1..6 and 9
inline bool ActionRequestRollbackAllowed(uint64_t beforeDisabled, uint64_t afterDisabled,
                                        uint32_t beforeTae, uint32_t afterTae) {
    // HKS owns the flush latch (bit 0), persistent bits 10/11 and unknown flags.
    // A pending flush or a changed restriction represents intentional consumption.
    return beforeDisabled == afterDisabled && !(beforeTae & 1u) &&
           !((beforeTae ^ afterTae) & ~ActionRequestTransientCancels);
}

// CSChrActionRequestModule's DLVector<ActionQueueEntry>. Storage remains game-owned.
struct ActionQueueVector {
    uintptr_t allocator = 0, first = 0, last = 0, end = 0;
};
struct ActionQueueEntry {
    int32_t tae = -1;
    uint32_t padding = 0;
    uint64_t actions = 0;
};
static_assert(sizeof(ActionQueueVector) == 0x20);
static_assert(sizeof(ActionQueueEntry) == 0x10);

struct ActionQueueSnapshot {
    static constexpr size_t MaxEntries = 32;
    ActionQueueVector storage{};
    ActionQueueEntry entries[MaxEntries]{};
    size_t count = 0;
    bool valid = false;
};

inline bool ActionQueueBounds(const ActionQueueVector& v, size_t& count) {
    count = 0;
    if (!v.first) return !v.last && !v.end;
    if (!v.allocator || v.last < v.first || v.end < v.last ||
        v.first % alignof(ActionQueueEntry) ||
        (v.last - v.first) % sizeof(ActionQueueEntry) ||
        (v.end - v.first) % sizeof(ActionQueueEntry)) return false;
    count = (v.last - v.first) / sizeof(ActionQueueEntry);
    return count <= ActionQueueSnapshot::MaxEntries &&
           (v.end - v.first) / sizeof(ActionQueueEntry) <= 4096;
}

template<class Read>
ActionQueueSnapshot ReadActionQueue(const void* vector, Read read) {
    ActionQueueSnapshot s;
    if (!read(vector, &s.storage, sizeof(s.storage)) || !ActionQueueBounds(s.storage, s.count)) return s;
    s.valid = !s.count || read(reinterpret_cast<const void*>(s.storage.first), s.entries,
                              s.count * sizeof(ActionQueueEntry));
    return s;
}

inline bool CanRestoreActionQueue(const ActionQueueSnapshot& before, const ActionQueueSnapshot& after) {
    return before.valid && after.valid && before.storage.allocator == after.storage.allocator &&
           before.storage.first == after.storage.first && before.storage.end == after.storage.end;
}

// Only data and logical length may be restored. A reallocation invalidates the snapshot;
// putting an old allocator/begin/capacity back would resurrect freed storage.
template<class Write>
bool RestoreActionQueue(void* vector, const ActionQueueSnapshot& before,
                        const ActionQueueSnapshot& after, Write write) {
    if (!CanRestoreActionQueue(before, after)) return false;
    const size_t bytes = before.count * sizeof(ActionQueueEntry);
    const bool changed = before.count != after.count || (bytes && std::memcmp(before.entries, after.entries, bytes));
    if (!changed) return false;
    if (bytes && !write(reinterpret_cast<void*>(before.storage.first), before.entries, bytes)) return false;
    if (before.storage.last != after.storage.last &&
        !write(static_cast<uint8_t*>(vector) + offsetof(ActionQueueVector, last),
               &before.storage.last, sizeof(before.storage.last))) return false;
    return true;
}

inline uint64_t ActionQueueMask(const ActionQueueSnapshot& s) {
    uint64_t mask = 0;
    if (s.valid) for (size_t i = 0; i < s.count; ++i) mask |= s.entries[i].actions;
    return mask;
}

} // namespace ervr
