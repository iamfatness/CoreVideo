#pragma once
#include <string>

// Encoder placement for ISO sessions.
//
// Hardware encoders have hard concurrency budgets (GeForce NVENC allows a
// fixed number of sessions per system — 8 on current drivers — shared with
// OBS's own program/stream/vertical outputs). Letting an operator start
// 8 ISO feeds + program on NVENC oversubscribes the encoder: sessions fail
// to open or fall behind realtime. Rather than surfacing that as per-row
// encoder errors, sessions are placed automatically: NVENC while the budget
// lasts, then Intel QSV (separate silicon, no shared budget with NVENC),
// then CPU x264. Explicit encoder choices are honored up to the budget and
// overflow down the same chain.
//
// Pure logic — unit tested in tests/iso-encoder-plan-test.cpp.

struct IsoEncoderAvailability {
    bool nvenc = false;
    bool qsv = false;
    bool amf = false;
};

inline int iso_nvenc_default_session_limit()
{
    // GeForce consumer driver session cap (driver 570+, required by the OBS
    // versions we support). Pro GPUs allow more; treating them as 8 is safe.
    return 8;
}

// Chooses the encoder for the next session to be created.
//   requested        "auto" or an explicit ffmpeg encoder name
//   nvenc_remaining  NVENC budget minus OBS's own active NVENC encoders
//                    minus ISO sessions already recording via NVENC
inline std::string iso_choose_session_encoder(
    const std::string &requested,
    int nvenc_remaining,
    const IsoEncoderAvailability &avail)
{
    const bool automatic = requested == "auto";

    if (!automatic && requested != "h264_nvenc") {
        // Explicit non-NVENC choice: QSV/AMF have no meaningful shared
        // session budget on the hardware we target; x264 always works.
        // (Availability probing/fallback for absent hardware happens at
        // start, as before.)
        return requested;
    }

    // "auto", or explicit NVENC: NVENC while the budget lasts.
    if (avail.nvenc && nvenc_remaining > 0)
        return "h264_nvenc";
    if (avail.qsv)
        return "h264_qsv";
    if (avail.amf)
        return "h264_amf";
    return "libx264";
}

// The next encoder to try when a session's encoder fails at startup
// (e.g. the session budget estimate was wrong). Ends at x264, which
// cannot run out of sessions.
inline std::string iso_demote_encoder(const std::string &failed,
                                      const IsoEncoderAvailability &avail)
{
    if (failed == "h264_nvenc") {
        if (avail.qsv)
            return "h264_qsv";
        if (avail.amf)
            return "h264_amf";
        return "libx264";
    }
    if (failed == "h264_qsv") {
        if (avail.amf)
            return "h264_amf";
        return "libx264";
    }
    return "libx264";
}
