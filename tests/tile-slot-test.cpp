// Pins the stale-participant rules for a reused tile slot. The bug these guard
// against is that a reassigned tile could keep showing the previous
// participant's face indefinitely when the incoming assignee's camera is off.

#include "zoom-tile-slot.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace {

bool test_accepts_only_the_current_assignee()
{
    TileSlotState slot;
    slot.assign(1001);

    if (!slot.accepts_frame(1001)) {
        std::cerr << "slot rejected its own participant's frame\n";
        return false;
    }
    if (slot.accepts_frame(2002)) {
        std::cerr << "slot accepted another participant's frame\n";
        return false;
    }
    // The engine always stamps an id, but an unreported one must not blank the
    // tile.
    if (!slot.accepts_frame(0)) {
        std::cerr << "slot rejected an unattributed frame\n";
        return false;
    }
    return true;
}

bool test_reassign_invalidates_the_stored_frame()
{
    TileSlotState slot;
    slot.assign(1001);

    const uint64_t stamped = slot.epoch();
    if (!slot.frame_is_current(stamped)) {
        std::cerr << "a freshly stamped frame should be current\n";
        return false;
    }

    if (!slot.assign(2002)) {
        std::cerr << "assign to a new participant should report a change\n";
        return false;
    }
    if (slot.frame_is_current(stamped)) {
        std::cerr << "the previous assignee's stored frame survived reassignment\n";
        return false;
    }
    // The new assignee's first frame becomes current.
    if (!slot.frame_is_current(slot.epoch())) {
        std::cerr << "the new assignee's frame should be current\n";
        return false;
    }
    return true;
}

bool test_redundant_assign_keeps_the_tile_alive()
{
    TileSlotState slot;
    slot.assign(1001);
    const uint64_t stamped = slot.epoch();

    if (slot.assign(1001)) {
        std::cerr << "reassigning the same participant should report no change\n";
        return false;
    }
    if (!slot.frame_is_current(stamped)) {
        std::cerr << "a redundant update blanked a healthy tile\n";
        return false;
    }
    return true;
}

bool test_unstamped_frame_is_never_current()
{
    TileSlotState slot;
    if (slot.frame_is_current(0)) {
        std::cerr << "an unstamped frame must never be shown\n";
        return false;
    }
    return true;
}

// The exact failure the guards exist for: a slot showing A is repointed at B,
// B's camera is off, and A's frames are still arriving from the engine.
// Neither the in-flight frames nor the already-stored one may keep A on air.
bool test_reassigned_tile_does_not_keep_showing_the_old_participant()
{
    const uint32_t kAlice = 1001, kBob = 2002;

    TileSlotState slot;
    slot.assign(kAlice);
    const uint64_t alice_frame = slot.epoch();  // Alice's frame is stored

    slot.assign(kBob);

    // 1. Frames still in flight for Alice must be dropped, not stored.
    if (slot.accepts_frame(kAlice)) {
        std::cerr << "in-flight frame from the previous assignee was accepted\n";
        return false;
    }
    // 2. Alice's already-stored frame must stop being shown immediately, even
    //    though Bob has sent nothing yet — the tile goes neutral instead.
    if (slot.frame_is_current(alice_frame)) {
        std::cerr << "previous assignee stayed on air after reassignment\n";
        return false;
    }
    // 3. Bob's first frame is accepted and shown.
    if (!slot.accepts_frame(kBob)) {
        std::cerr << "the new assignee's frame was rejected\n";
        return false;
    }
    if (!slot.frame_is_current(slot.epoch())) {
        std::cerr << "the new assignee's frame was not shown\n";
        return false;
    }
    return true;
}

// A tile is only "silent" relative to its current assignment. Keying the
// self-heal retry off "never delivered any frame" permanently excludes any slot
// that once showed somebody, which is precisely the slot that needs retrying
// after being repointed at a participant who has not joined yet.
bool test_silence_is_measured_per_assignment()
{
    TileSlotState slot;
    slot.assign(1001);

    uint64_t last_delivered = 0;  // mirrors TileFeed::frame_epoch
    if (slot.frame_is_current(last_delivered)) {
        std::cerr << "a slot with no frame yet should count as silent\n";
        return false;
    }

    last_delivered = slot.epoch();  // Alice's camera comes up
    if (!slot.frame_is_current(last_delivered)) {
        std::cerr << "a delivering slot should not be retried\n";
        return false;
    }

    slot.assign(2002);  // repointed at Bob, who has not joined
    if (slot.frame_is_current(last_delivered)) {
        std::cerr << "a repointed slot must count as silent again, even though "
                     "it delivered frames for its previous assignee\n";
        return false;
    }

    last_delivered = slot.epoch();  // Bob finally sends
    if (!slot.frame_is_current(last_delivered)) {
        std::cerr << "the new assignee's delivery should end the retries\n";
        return false;
    }
    return true;
}

// The ordering contract behind begin_frame(). assign() publishes the
// participant id before bumping the epoch, so sampling the epoch first makes
// the pair (epoch, id) coherent: the id can only ever be newer than the epoch,
// never older.
//
// Sampling the id first breaks that — a repoint landing between the two loads
// yields the OLD id with the NEW epoch, so the outgoing participant's frame is
// accepted and stamped as current. This drives the real API concurrently and
// asserts the invariant that rules that pairing out.
bool test_begin_frame_never_pairs_a_stale_id_with_a_fresh_epoch()
{
    TileSlotState slot;
    std::atomic<bool> stop{false};

    // Assignment k sets participant id k and produces epoch k + 1, so a
    // coherent sample always satisfies id >= epoch - 1.
    std::thread writer([&] {
        uint32_t next = 1;
        while (!stop.load(std::memory_order_relaxed)) slot.assign(next++);
    });

    bool ok = true;
    uint64_t accepted = 0;
    uint64_t epoch_advances = 0;
    uint64_t previous_stamp = slot.epoch();

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline &&
           !(accepted > 100000 && epoch_advances > 1000)) {
        // Stand in for an arriving frame attributed to whoever the slot looks
        // to be showing right now.
        const uint32_t event_id = slot.participant_id();
        if (event_id == 0) continue;

        uint64_t stamp = 0;
        if (!slot.begin_frame(event_id, stamp)) continue;
        ++accepted;
        if (stamp != previous_stamp) { ++epoch_advances; previous_stamp = stamp; }

        // Accepting event_id means that at begin_frame's id load the slot still
        // pointed at event_id — i.e. the epoch was exactly event_id + 1. A
        // stamp sampled before that load therefore cannot be newer. A stamp
        // newer than that is proof the epoch was read after the id, which is
        // the race: this frame would be stamped as belonging to an assignment
        // that had already moved on.
        if (stamp > static_cast<uint64_t>(event_id) + 1) {
            std::cerr << "frame for participant " << event_id
                      << " stamped with epoch " << stamp << " (max legal "
                      << event_id + 1
                      << "): a superseded participant would be treated as "
                         "current\n";
            ok = false;
            break;
        }
    }

    stop.store(true, std::memory_order_relaxed);
    writer.join();

    // Guards against a vacuous pass on a machine where the writer never
    // interleaved with the reader.
    if (ok && epoch_advances == 0) {
        std::cerr << "no concurrent reassignment observed: the ordering "
                     "contract was never exercised\n";
        ok = false;
    }
    return ok;
}

}  // namespace

int main()
{
    if (!test_accepts_only_the_current_assignee()) return 1;
    if (!test_reassign_invalidates_the_stored_frame()) return 1;
    if (!test_redundant_assign_keeps_the_tile_alive()) return 1;
    if (!test_unstamped_frame_is_never_current()) return 1;
    if (!test_reassigned_tile_does_not_keep_showing_the_old_participant()) return 1;
    if (!test_silence_is_measured_per_assignment()) return 1;
    if (!test_begin_frame_never_pairs_a_stale_id_with_a_fresh_epoch()) return 1;

    std::cout << "tile-slot: all tests passed\n";
    return 0;
}
