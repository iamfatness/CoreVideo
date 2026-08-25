#pragma once
//
// talkback-cue.h — when to play the talkback open/close audio cue, and the
// call that plays it.
//
// THE PROBLEM THIS EXISTS TO FIX: the Zoom channel a talkback key opens is
// created ON THE KEY PRESS, but standing it up is a real round trip
// (CreateChannel, then BeginBatchInviteUsers/AddUserToInvite/
// ExecuteBatchInviteUsers -- see talkback-controller.cpp's
// kTalkbackSessionGraceMs comment for the timing). Nothing carried by that
// channel exists until it completes, so the director's first words land in
// that window and are clipped before anyone can hear them.
//
// The fix is NOT "cue at key press" -- that would tell the operator it's
// safe to talk before it actually is, which is worse than no cue at all: a
// confident operator talking into a channel that isn't there yet is exactly
// today's bug, now with false reassurance attached. The cue has to fire on
// the engine's own CONFIRMED live state (ZoomEngineClient::
// TalkbackSessionStatus::live, the same field evaluate() already uses to
// decide whether to force-close a key that never opened -- see
// talkback-controller.cpp), because that is the actual moment the clipping
// window ends.
//
// Free of Qt / OBS / Zoom SDK dependencies, same as talkback-key.h and
// talkback-channel-owner.h, so the edge-detection decision can be pinned by
// a test with no engine, no meeting, and no sound card.
//
#include <cstdint>

enum class TalkbackCue {
    None,
    Open,
    Close,
};

// Pure edge-detector over the engine's own confirmed `live` value. The
// caller (TalkbackController) holds "the last value seen" across ticks
// (m_last_live) and passes both sides of the transition in; this function
// remembers nothing itself, so a test can drive any sequence of values
// without a timer, a controller, or a mutex.
//
//   false -> true : OPEN  (exactly once, on the transition -- not on every
//                          tick the session stays live)
//   true  -> false: CLOSE (exactly once, on the transition)
//   same  -> same : None  (includes a session that never goes live at all)
inline TalkbackCue talkback_cue_on_live_change(bool prev_live, bool live)
{
    if (!prev_live && live) return TalkbackCue::Open;
    if (prev_live && !live) return TalkbackCue::Close;
    return TalkbackCue::None;
}

// Fires the OS-level cue. Implemented in talkback-cue.cpp, which is the only
// file that may touch playback -- see its header comment for the API choice
// and for what happens when a cue is requested while one is already
// sounding.
//
// MUST NOT BLOCK. Callers are TalkbackController::evaluate() (the Qt main
// thread, on a 25ms QTimer -- see talkback-controller.h) and key_off()
// (which can run from the same timer OR from the control-server's request
// handling, see zoom-control-server.cpp). A blocking sound call from either
// stalls window dragging, every other dock, and every other talkback
// operation for as long as the cue takes to play. talkback-cue.cpp's
// implementation guarantees this by never doing OS playback work on the
// calling thread.
void talkback_play_cue(TalkbackCue cue);
