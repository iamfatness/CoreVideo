#include "speaker-director.h"
#include "zoom-types.h"

#include <iostream>
#include <vector>
#include <cassert>

static int failures = 0;

static void fail(const char *message)
{
    std::cerr << "FAIL: " << message << "\n";
    ++failures;
}

static ParticipantInfo p(uint32_t id,
                         bool has_video = true,
                         bool is_talking = false,
                         bool is_muted = false)
{
    ParticipantInfo info;
    info.user_id = id;
    info.has_video = has_video;
    info.is_talking = is_talking;
    info.is_muted = is_muted;
    return info;
}

static std::vector<ParticipantInfo> roster(std::initializer_list<ParticipantInfo> people)
{
    return std::vector<ParticipantInfo>(people);
}

static bool check_directed(uint32_t expected)
{
    if (SpeakerDirector::instance().directed_speaker_id() != expected) {
        std::cerr << "Expected directed=" << expected
                  << " got " << SpeakerDirector::instance().directed_speaker_id() << "\n";
        return false;
    }
    return true;
}

int main()
{
    auto &director = SpeakerDirector::instance();

    // --- Basic promotion ---
    director.reset();
    director.configure(500, 2000, true, {});

    uint64_t t = 10000;
    director.update_roster(roster({p(101, true, true), p(102, true, false)}), 101, t);
    if (!check_directed(101)) fail("basic promotion to raw speaker failed");

    // --- Sensitivity window prevents immediate switch ---
    director.reset();
    director.configure(500, 50, true, {});   // very short hold so we can test sensitivity cleanly
    t = 20000;
    director.update_roster(roster({p(201, true, true), p(202, true, true)}), 201, t);
    if (!check_directed(201)) fail("initial speaker not promoted");

    t += 300;
    director.update_roster(roster({p(201, true, false), p(202, true, true)}), 202, t);
    if (!check_directed(201)) fail("switched before sensitivity window");

    // Candidate became 202 at t=20300. Wait full sensitivity (hold is only 50ms).
    t += 500; // candidate_age = 500 >= 500 sensitivity, hold already satisfied
    bool switched = director.tick(t);
    if (!switched) fail("should have switched after sensitivity elapsed");
    if (!check_directed(202)) fail("failed to switch to new speaker after sensitivity");

    // --- Hold time blocks rapid switching ---
    director.reset();
    director.configure(100, 1500, true, {});
    t = 30000;
    director.update_roster(roster({p(301, true, true)}), 301, t);
    if (!check_directed(301)) fail("initial 301");

    t += 200;
    director.update_roster(roster({p(301, true, false), p(302, true, true)}), 302, t);
    t += 150; // candidate age ok, but hold not expired
    director.tick(t);
    if (!check_directed(301)) fail("switched during hold time");

    t += 1400; // now past hold
    director.tick(t);
    if (!check_directed(302)) fail("did not switch after hold expired");

    // --- Manual take supersedes everything ---
    director.reset();
    director.configure(100, 50, true, {});   // short hold
    t = 40000;
    director.update_roster(roster({p(401, true, true), p(402, true, true)}), 401, t);
    if (!check_directed(401)) fail("initial");

    t += 50;
    bool took = director.set_manual_speaker(402, t);
    if (!took) fail("manual take should succeed");
    if (!check_directed(402)) fail("manual take did not promote 402");

    // Even if raw speaker changes, manual should stick
    t += 100;
    director.update_roster(roster({p(401, true, true), p(402, true, false)}), 401, t);
    if (!check_directed(402)) fail("manual speaker was stolen by raw speaker");

    // The manual speaker leaving the roster is treated as a possible
    // transient blip: the pin HOLDS through the absence grace window (the
    // program output keeps showing the last speaker) and only falls back
    // once they have been gone for over a minute.
    t += 50;
    director.update_roster(roster({p(401, true, true)}), 401, t);  // 402 no longer in roster
    if (!check_directed(402)) fail("manual pin should hold through a roster blip");
    t += 61000; // past the absence grace window
    director.update_roster(roster({p(401, true, true)}), 401, t);
    if (!check_directed(401)) fail("did not fall back after manual speaker was gone past grace");

    // --- Manual release (when still valid) keeps current directed (no unnecessary cut) ---
    director.reset();
    director.configure(100, 50, true, {});
    t = 41000;
    director.update_roster(roster({p(411, true, true), p(412, true, true)}), 411, t);
    director.set_manual_speaker(412, t);
    t += 30;
    director.clear_manual_speaker(t);
    if (!check_directed(412)) fail("clearing manual while speaker is still valid should not cut away");

    // --- Muted and no-video participants are filtered ---
    director.reset();
    director.configure(100, 1000, true, {}); // require video
    t = 50000;
    director.update_roster(roster({
        p(501, true, true),
        p(502, false, true),   // no video
        p(503, true, true, true) // muted
    }), 502, t);
    if (!check_directed(501)) fail("should have ignored no-video and muted, picked 501");

    // With require_video=false, audio-only should be allowed
    director.reset();
    director.configure(100, 1000, false, {});
    t = 51000;
    director.update_roster(roster({p(601, false, true)}), 601, t);
    if (!check_directed(601)) fail("require_video=false should allow audio-only speaker");

    // --- Exclusions work ---
    director.reset();
    director.configure(100, 1000, true, {701});
    t = 52000;
    director.update_roster(roster({p(701, true, true), p(702, true, true)}), 701, t);
    if (!check_directed(702)) fail("excluded participant 701 was still chosen");

    // Adding the currently directed speaker to the exclusion list should move
    // the director away as soon as the roster is re-evaluated.
    director.reset();
    director.configure(100, 1000, true, {});
    t = 52500;
    const auto excluded_update_roster = roster({
        p(711, true, true),
        p(712, true, true)
    });
    director.update_roster(excluded_update_roster, 711, t);
    if (!check_directed(711)) fail("initial 711 before dynamic exclusion");
    director.configure(100, 1000, true, {711});
    director.update_roster(excluded_update_roster, 711, t + 10);
    if (!check_directed(712)) fail("dynamic exclusion did not move away from directed speaker");

    // --- Departed speaker: hold through the grace window, then clear ---
    director.reset();
    director.configure(100, 1000, true, {});
    t = 53000;
    director.update_roster(roster({p(801, true, true), p(802, true, false)}), 801, t);
    if (!check_directed(801)) fail("initial 801");

    t += 50;
    // 801 leaves the roster: could be a reconnect blip — the incumbent
    // holds (the output keeps showing the last speaker during silence).
    director.update_roster(roster({p(802, true, false)}), 0, t);
    if (!check_directed(801)) fail("incumbent should hold through a roster blip");

    // A new talker still takes over through the normal candidate flow —
    // the hold never delays a genuine handover.
    t += 2000;
    director.update_roster(roster({p(802, true, true)}), 802, t);
    t += 2000;
    director.update_roster(roster({p(802, true, true)}), 802, t);
    if (!check_directed(802)) fail("did not promote fallback speaker");

    // --- Incumbent holds through mute and video-off (the report that
    // motivated this: 'when the source stops talking, active speaker
    // unsubscribes the last speaker') ---
    director.reset();
    director.configure(100, 1000, true, {});
    t = 55000;
    director.update_roster(roster({p(811, true, true), p(812, true, true)}), 811, t);
    if (!check_directed(811)) fail("initial 811");
    t += 500;
    // 811 mutes and turns video off; nobody else is talking.
    director.update_roster(roster({p(811, false, false, true), p(812, true, false)}), 0, t);
    if (!check_directed(811))
        fail("incumbent must hold after muting / stopping video");
    t += 5000;
    if (director.tick(t))
        fail("tick must not dethrone a muted incumbent during silence");
    if (!check_directed(811))
        fail("incumbent must keep holding through silence");

    // --- Snapshot reports reasonable timing values ---
    director.reset();
    director.configure(600, 2500, true, {});
    t = 60000;
    director.update_roster(roster({p(901, true, true)}), 901, t);
    auto snap = director.snapshot(t + 450);
    if (snap.candidate_elapsed_ms != 0) fail("should have no candidate");
    if (snap.hold_remaining_ms != 2050) fail("hold remaining calculation wrong");

    // --- Candidate snapshot reports pending live switch state ---
    director.reset();
    director.configure(500, 1000, true, {});
    t = 70000;
    director.update_roster(roster({p(1001, true, true), p(1002, true, false)}), 1001, t);
    t += 100;
    director.update_roster(roster({p(1001, true, false), p(1002, true, true)}), 1002, t);
    snap = director.snapshot(t + 250);
    if (snap.directed_speaker_id != 1001) fail("candidate snapshot should keep current directed speaker");
    if (snap.candidate_speaker_id != 1002) fail("candidate snapshot should expose pending speaker");
    if (snap.candidate_elapsed_ms != 250) fail("candidate elapsed calculation wrong");
    if (snap.sensitivity_ms - snap.candidate_elapsed_ms != 250)
        fail("sensitivity remaining calculation wrong");

    // --- Candidate invalidation keeps the current shot until a valid candidate appears ---
    director.reset();
    director.configure(400, 50, true, {});
    t = 72000;
    director.update_roster(roster({p(1101, true, true), p(1102, true, false)}), 1101, t);
    t += 100;
    director.update_roster(roster({p(1101, true, false), p(1102, true, true)}), 1102, t);
    t += 100;
    director.update_roster(roster({p(1101, true, false), p(1102, false, true)}), 1102, t);
    t += 500;
    director.tick(t);
    if (!check_directed(1101)) fail("invalid candidate should not steal directed speaker");

    // --- Manual take rejects participants filtered by require_video ---
    director.reset();
    director.configure(100, 50, true, {});
    t = 73000;
    director.update_roster(roster({p(1201, true, true), p(1202, false, true)}), 1201, t);
    if (director.set_manual_speaker(1202, t + 10)) {
        fail("manual take should reject no-video participant when require_video=true");
    }
    if (!check_directed(1201)) fail("rejected manual take should keep current directed speaker");

    if (failures == 0) {
        std::cout << "All SpeakerDirector tests passed.\n";
        return 0;
    } else {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
}
