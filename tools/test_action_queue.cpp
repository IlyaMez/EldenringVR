#include "action_queue_snapshot.h"
#include <cstdio>
#include <cstring>

namespace {
int failures = 0;
void Check(bool passed, const char* message) {
    if (!passed) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
bool Read(const void* from, void* to, size_t size) { std::memcpy(to, from, size); return true; }

struct Queue {
    ervr::ActionQueueEntry entries[4]{};
    ervr::ActionQueueVector vector;
    Queue() {
        vector = {1, reinterpret_cast<uintptr_t>(entries), reinterpret_cast<uintptr_t>(entries),
                  reinterpret_cast<uintptr_t>(entries + 4)};
    }
    void Size(size_t count) { vector.last = vector.first + count * sizeof(entries[0]); }
    ervr::ActionQueueSnapshot Snapshot() { return ervr::ReadActionQueue(&vector, Read); }
};
}

int main() {
    using namespace ervr;
    Check(ActionRequestRollbackAllowed(0, 0, 0x218, 0), "transient TAE cancels can survive a zero-step pass");
    Check(!ActionRequestRollbackAllowed(0, 1, 0x218, 0x218), "new input restriction blocks rollback");
    Check(!ActionRequestRollbackAllowed(1, 0, 0x218, 0x218), "removed input restriction blocks rollback");
    Check(!ActionRequestRollbackAllowed(0, 0, 0x219, 0x218), "do not undo a consumed HKS queue flush");
    Check(!ActionRequestRollbackAllowed(0, 0, 0x219, 0x219), "pending HKS flush also blocks rollback");
    Check(!ActionRequestRollbackAllowed(0, 0, 0x218, 0x219), "new HKS queue flush blocks rollback");
    Check(!ActionRequestRollbackAllowed(0, 0, 0, 1u << 10), "falling-state change blocks rollback");
    Check(!ActionRequestRollbackAllowed(0, 0, 1u << 11, 0), "cancel-disable change blocks rollback");
    Check(!ActionRequestRollbackAllowed(0, 0, 0, 1u << 31), "unknown TAE flag change blocks rollback");
    Check(ActionRequestRollbackAllowed(4, 4, 0x618, 0x400), "unchanged persistent flags allow transient restoration");
    int writes = 0;
    auto write = [&](void* to, const void* from, size_t size) {
        ++writes; std::memcpy(to, from, size); return true;
    };

    // Idle slot switching needs the queue-only bit 37; scalar cancel masks lack it.
    Queue queue;
    queue.entries[0] = {0, 0, (1ull << 37) | (1ull << 9)};
    queue.entries[1] = {10000000, 0, (1ull << 2) | (1ull << 35)};
    queue.Size(2);
    const auto before = queue.Snapshot();
    Check(before.valid && before.count == 2, "snapshot captures both animation states");
    Check(ActionQueueMask(before) & (1ull << 37), "snapshot keeps slot-switch permission");
    queue.Size(0);
    queue.entries[0].actions = 0;
    Check(RestoreActionQueue(&queue.vector, before, queue.Snapshot(), write), "restore a cleared cancel queue");
    Check(queue.vector.last == before.storage.last && queue.entries[0].actions == before.entries[0].actions &&
          queue.entries[1].actions == before.entries[1].actions, "restore queue entries and logical length");
    Check(queue.vector.allocator == before.storage.allocator && queue.vector.first == before.storage.first &&
          queue.vector.end == before.storage.end, "leave ownership fields intact");

    queue.entries[1].actions = 0;
    Check(RestoreActionQueue(&queue.vector, before, queue.Snapshot(), write) &&
          queue.entries[1].actions == before.entries[1].actions, "restore changed masks at unchanged length");
    writes = 0;
    Check(!RestoreActionQueue(&queue.vector, before, queue.Snapshot(), write) && !writes,
          "unchanged queues cause no writes");

    Queue replacement;
    replacement.Size(2);
    writes = 0;
    Check(!RestoreActionQueue(&replacement.vector, before, replacement.Snapshot(), write) && !writes,
          "reallocation never restores old backing pointers or data");
    auto changedOwner = queue.Snapshot();
    ++changedOwner.storage.allocator;
    Check(!CanRestoreActionQueue(before, changedOwner), "allocator replacement invalidates snapshot");
    auto changedCapacity = queue.Snapshot();
    changedCapacity.storage.end += sizeof(ActionQueueEntry);
    Check(!CanRestoreActionQueue(before, changedCapacity), "capacity replacement invalidates snapshot");

    // A failed data write must not publish entries the game cannot read.
    queue.Size(0);
    const uintptr_t emptyEnd = queue.vector.last;
    writes = 0;
    auto failWrite = [&](void*, const void*, size_t) { ++writes; return false; };
    Check(!RestoreActionQueue(&queue.vector, before, queue.Snapshot(), failWrite) &&
          writes == 1 && queue.vector.last == emptyEnd, "failed payload write leaves logical length alone");

    const auto empty = queue.Snapshot();
    queue.Size(1);
    writes = 0;
    Check(RestoreActionQueue(&queue.vector, empty, queue.Snapshot(), write) &&
          writes == 1 && queue.vector.last == queue.vector.first, "empty snapshot clears only logical length");

    ActionQueueVector malformed = queue.vector;
    size_t count = 0;
    malformed.last = malformed.first - sizeof(ActionQueueEntry);
    Check(!ActionQueueBounds(malformed, count), "reject inverted begin/end");
    malformed = queue.vector; malformed.last += 1;
    Check(!ActionQueueBounds(malformed, count), "reject partial entry");
    malformed = queue.vector; malformed.last = malformed.end + sizeof(ActionQueueEntry);
    Check(!ActionQueueBounds(malformed, count), "reject size exceeding capacity");
    malformed = queue.vector; malformed.first = 0;
    Check(!ActionQueueBounds(malformed, count), "reject partially null storage");
    malformed = queue.vector; malformed.allocator = 0;
    Check(!ActionQueueBounds(malformed, count), "reject missing allocator for live storage");
    malformed = queue.vector;
    malformed.last = malformed.first + (ActionQueueSnapshot::MaxEntries + 1) * sizeof(ActionQueueEntry);
    malformed.end = malformed.last;
    Check(!ActionQueueBounds(malformed, count), "reject oversized snapshot before reading payload");
    malformed = queue.vector; malformed.end = malformed.first + 4097 * sizeof(ActionQueueEntry);
    Check(!ActionQueueBounds(malformed, count), "reject implausible capacity");
    const ActionQueueVector unallocated{};
    Check(ActionQueueBounds(unallocated, count) && !count, "accept empty unallocated vector");

    queue.Size(1);
    int reads = 0;
    const auto unreadable = ReadActionQueue(&queue.vector, [&](const void* from, void* to, size_t size) {
        if (++reads > 1) return false;
        return Read(from, to, size);
    });
    Check(!unreadable.valid && !CanRestoreActionQueue(before, unreadable), "unreadable payload cannot be restored");
    if (!failures) std::puts("action queue regression checks passed");
    return failures ? 1 : 0;
}
