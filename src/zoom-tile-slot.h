#pragma once

#include <atomic>
#include <cstdint>

// The assignment and staleness bookkeeping for one tile slot, factored out of
// the OBS source so the rules can be exercised without a running OBS or Zoom
// engine.
//
// A tile slot outlives the participants shown in it: the operator repoints a
// slot at somebody else and the same engine subscription is reused. That
// creates two distinct ways for the wrong face to end up on air, and this class
// exists to close both:
//
//  1. A frame *in flight* for the previous assignee. The engine dispatches
//     frames asynchronously, so frames decoded for the old participant can
//     arrive after the slot was repointed. Storing one re-arms the tile with
//     the wrong person.
//  2. A frame *already stored* for the previous assignee. It must stop being
//     shown the instant the slot is repointed — otherwise, if the incoming
//     assignee's camera is off, the outgoing participant stays on air for the
//     rest of the session.
//
// Both checks are lock-free so the reassignment path never has to take the
// mutex that guards a slot's pixels.
class TileSlotState {
public:
    // Repoints the slot. Returns true when this was a real change, in which
    // case any frame stored or in flight for the previous assignee is
    // invalidated. Reassigning to the same participant is a no-op, so a
    // redundant settings update does not blank a healthy tile.
    bool assign(uint32_t participant_id)
    {
        if (m_participant_id.load(std::memory_order_acquire) == participant_id)
            return false;
        m_participant_id.store(participant_id, std::memory_order_release);
        m_epoch.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    uint32_t participant_id() const
    {
        return m_participant_id.load(std::memory_order_acquire);
    }

    // The stamp to record against a frame captured now.
    uint64_t epoch() const { return m_epoch.load(std::memory_order_acquire); }

    // A frame event carries the participant the engine actually decoded it for.
    // Anything that is not the current assignee belongs to a previous
    // assignment and must not re-arm the tile. An id of 0 means the engine did
    // not report one; accept it rather than blanking the tile.
    bool accepts_frame(uint32_t event_participant_id) const
    {
        return event_participant_id == 0 ||
               event_participant_id == participant_id();
    }

    // Decides whether to keep an arriving frame and, if so, what epoch to stamp
    // it with. Callers must use this rather than pairing epoch() with
    // accepts_frame() themselves, because only one of the two possible orders
    // is safe and the unsafe one fails rarely enough to survive testing.
    //
    // The epoch MUST be sampled first. assign() stores the new participant id
    // and *then* bumps the epoch, so:
    //
    //  - A repoint completing before this epoch load: the acquire load observes
    //    the bumped epoch, which synchronizes-with assign()'s fetch_add, so the
    //    id store sequenced before it is visible and the id check below rejects
    //    the frame.
    //  - A repoint completing after this epoch load: the frame is stamped with
    //    the superseded epoch, so frame_is_current() later rejects it.
    //
    // Sampling the id first leaves a window where a repoint lands between the
    // two loads: the id check passes against the OLD participant while the
    // frame is stamped with the NEW epoch, so a frame of the outgoing
    // participant is treated as current and stays on air until the incoming one
    // sends a frame — forever, if their camera is off. The acquire semantics of
    // the epoch load also stop the compiler or CPU reordering the id load above
    // it, so the source order here is the guarantee.
    bool begin_frame(uint32_t event_participant_id, uint64_t &stamp) const
    {
        stamp = epoch();
        return accepts_frame(event_participant_id);
    }

    // A stored frame is showable only while the slot still points at the
    // assignment it was captured under. Epochs start at 1, so a frame that was
    // never stamped (0) is never current.
    static bool frame_is_current_at(uint64_t frame_epoch, uint64_t at_epoch)
    {
        return frame_epoch != 0 && frame_epoch == at_epoch;
    }

    bool frame_is_current(uint64_t frame_epoch) const
    {
        return frame_is_current_at(frame_epoch, epoch());
    }

private:
    std::atomic<uint32_t> m_participant_id{0};
    std::atomic<uint64_t> m_epoch{1};
};
