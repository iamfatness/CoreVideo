#pragma once

// How a screen-share output's health and lifecycle differ from a camera
// output's. Pure and Qt/OBS-free so the rules are pinned by a host test
// (tests/share-output-policy-test.cpp) rather than living inline in
// zoom-source.cpp, where no test can reach them.
//
// Until 2026-11-14 a share output went through exactly the camera rules, and
// three of them are wrong for a share:
//
// 1. STALENESS. A camera delivers 15-30 fps; 3 s without a frame means the
//    feed died. Zoom's share is content-adaptive -- 1-5 fps on a static slide
//    was measured live (2026-08-01) -- so 3 s sits on the edge of a perfectly
//    healthy share. The Output Manager graded it "Video stale", and the 1 s
//    health timer resubscribed it on a backoff for as long as the slide sat
//    still.
//
// 2. QUALITY UPGRADE. The share property sheet hides Resolution, so every
//    share output carries the 1080p default, and any share that is not
//    1080-line (a 1440x900 laptop, a 1728x1117 window) read as "Zoom
//    delivered lower resolution" and entered the upgrade retry loop. A share
//    IS the sharer's screen size; no resubscribe can change it.
//
// 3. SHARE END. Nothing cleared a share output when the presenter stopped
//    sharing. The engine drops its renderer and stops writing frames, and the
//    last frame stayed frozen on program until somebody shared again.
//
// Rule 3's signal is the roster's is_sharing_screen flag, which both engines
// push the moment the active sharer changes. It is deliberately NOT trusted
// alone: frames and roster updates ride different dispatch lanes (media
// events vs. the ordered control lane), and a sharer the engine could not
// match to a participant row would read as "nobody sharing" while frames
// flow. So a clear also requires that frames have stopped for
// kShareEndGraceNs. A genuine share end stops frames instantly; a live share
// that the roster has not caught up with keeps delivering them.
//
// The frame gap alone is not enough either (code review, 2026-11-14): a
// static slide can go far longer than a second without a frame, so for a
// static share the roster is the only thing standing between it and a blank
// output -- and both engines zero the active sharer on any momentarily empty
// viewable-share list. So "nobody sharing" must also have PERSISTED for
// kShareRosterDebounceNs. And because a clear on a wrong roster would
// otherwise be permanent for a static slide (nothing re-delivers its frame),
// zoom-source.cpp resubscribes on the roster's false->true edge while the
// output is empty; the engine then rebuilds the idle renderer, and a fresh
// subscription re-delivers the current picture.

#include <cstdint>

// See rule 1. Comfortably above a ~1 fps static share, and the engine side
// rebuilds an idle share renderer on the resubscribe this triggers (see
// engine-share.cpp / main-macos.mm), so a genuinely stalled share still
// recovers.
inline constexpr uint64_t kShareStaleVideoNs = 10'000'000'000ULL;

// See rule 3. Long enough that in-flight frames from a share that just ended
// have all landed, short enough that the frozen frame is gone before an
// operator notices it.
inline constexpr uint64_t kShareEndGraceNs = 1'000'000'000ULL;

// See rule 3: how long the roster must keep saying "nobody sharing".
inline constexpr uint64_t kShareRosterDebounceNs = 2'000'000'000ULL;

inline uint64_t video_stale_threshold_ns(bool screen_share,
                                         uint64_t camera_threshold_ns)
{
    return screen_share ? kShareStaleVideoNs : camera_threshold_ns;
}

// Nobody sharing is a normal state of a meeting, not a fault to recover.
inline bool share_wants_stale_recovery(bool roster_has_sharer)
{
    return roster_has_sharer;
}

// See rule 2.
inline bool quality_upgrade_applies(bool screen_share)
{
    return !screen_share;
}

// See rule 3. `roster_false_since_ns` is when the roster last went from
// showing a sharer to not (0 = unknown, which never clears); `showing_frame`
// is whether the output currently has a picture on it (a real frame or a
// placeholder); `last_frame_ns` is when the last real frame arrived, 0 if
// none has.
inline bool share_should_clear(bool roster_has_sharer,
                               uint64_t roster_false_since_ns,
                               bool showing_frame, uint64_t last_frame_ns,
                               uint64_t now_ns)
{
    if (roster_has_sharer || !showing_frame)
        return false;
    if (roster_false_since_ns == 0 || roster_false_since_ns > now_ns ||
        now_ns - roster_false_since_ns < kShareRosterDebounceNs)
        return false;
    if (last_frame_ns == 0)
        return true;
    if (last_frame_ns >= now_ns)
        return false;
    return now_ns - last_frame_ns >= kShareEndGraceNs;
}
