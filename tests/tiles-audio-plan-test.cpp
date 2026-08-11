// tests/tiles-audio-plan-test.cpp
// The decision logic behind auto-created participant audio (src/zoom-tiles-audio-plan.h).
//
// Two assertions in here are load-bearing, and both are about damage rather
// than about features:
//
//   1. A participant already owned by a DIFFERENT Tiles source yields no
//      Create. Two walls showing the same person must not carry that person's
//      voice twice — doubling the mix is the exact artefact the whole
//      group-based audio topology exists to prevent.
//   2. A participant who leaves the wall yields Mute, never a removal. A
//      source that vanishes mid-show takes its fader, its filters and any
//      operator tuning with it, and in Auto mode the wall reflows constantly.

#include "zoom-tiles-audio-plan.h"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

static const std::string kSelf  = "uuid-self";
static const std::string kOther = "uuid-other-tiles-source";

static std::vector<ParticipantInfo> roster_of(
    const std::vector<std::pair<uint32_t, std::string>> &people)
{
    std::vector<ParticipantInfo> out;
    for (const auto &p : people) {
        ParticipantInfo info;
        info.user_id      = p.first;
        info.display_name = p.second;
        out.push_back(info);
    }
    return out;
}

static TilesAudioPlanParams params_on()
{
    TilesAudioPlanParams p;
    p.self_uuid = kSelf;
    p.enabled   = true;
    return p;
}

static const TilesAudioAction *find_action(const TilesAudioPlan &plan,
                                           TilesAudioActionKind kind,
                                           uint32_t id)
{
    for (const auto &a : plan.actions)
        if (a.kind == kind && a.participant_id == id) return &a;
    return nullptr;
}

static bool has_any_for(const TilesAudioPlan &plan, uint32_t id)
{
    for (const auto &a : plan.actions)
        if (a.participant_id == id) return true;
    return false;
}

int main()
{
    const auto roster = roster_of({{11, "Ada"}, {22, "Grace"}, {33, "Katherine"}});

    // ── Off by default ───────────────────────────────────────────────────────
    // The switch is checked before anything else. A disabled feature that still
    // computed actions would be one refactor away from applying them.
    {
        TilesAudioPlanParams p = params_on();
        p.enabled = false;
        const TilesAudioPlan plan = plan_tiles_audio({11, 22}, {}, roster, p);
        if (!plan.actions.empty()) {
            std::cerr << "disabled planner emitted " << plan.actions.size()
                      << " action(s); want 0\n";
            return 1;
        }
    }

    // ── Create for a participant with nothing existing ───────────────────────
    {
        const TilesAudioPlan plan = plan_tiles_audio({11}, {}, roster, params_on());
        const TilesAudioAction *a =
            find_action(plan, TilesAudioActionKind::Create, 11);
        if (!a) {
            std::cerr << "no Create emitted for a participant with no source\n";
            return 1;
        }
        // Track 1 (program, bit 0) plus the first stem track 2 (bit 1).
        if (a->mixers != 0x3u) {
            std::cerr << "first slot mixers " << a->mixers << "; want 3 "
                      << "(track 1 program + track 2 stem)\n";
            return 1;
        }
        if (a->name.find("Ada") == std::string::npos) {
            std::cerr << "created name '" << a->name
                      << "' does not carry the roster display name\n";
            return 1;
        }
    }

    // ── Idempotence: a correct existing source produces no actions ───────────
    // Reconciliation runs on every roster change. If a settled wall emitted
    // actions, it would rewrite sources continuously during a show.
    {
        TilesAudioSourceState st;
        st.participant_id = 11;
        st.owner_uuid     = kSelf;
        st.name           = "Ada (CoreVideo)";
        st.muted          = false;
        st.mixers         = 0x3u;
        const TilesAudioPlan plan =
            plan_tiles_audio({11}, {st}, roster, params_on());
        if (!plan.actions.empty()) {
            std::cerr << "settled wall emitted " << plan.actions.size()
                      << " action(s); want 0\n";
            return 1;
        }
    }

    // ── A returning participant is unmuted, not recreated ────────────────────
    {
        TilesAudioSourceState st;
        st.participant_id = 11;
        st.owner_uuid     = kSelf;
        st.name           = "Ada (CoreVideo)";
        st.muted          = true;
        st.mixers         = 0x3u;
        const TilesAudioPlan plan =
            plan_tiles_audio({11}, {st}, roster, params_on());
        if (!find_action(plan, TilesAudioActionKind::Unmute, 11)) {
            std::cerr << "a muted participant back on the wall was not unmuted\n";
            return 1;
        }
        if (find_action(plan, TilesAudioActionKind::Create, 11)) {
            std::cerr << "a muted existing source was recreated instead of unmuted\n";
            return 1;
        }
    }

    // ── Leaving the wall mutes, never removes ────────────────────────────────
    {
        TilesAudioSourceState st;
        st.participant_id = 22;
        st.owner_uuid     = kSelf;
        st.name           = "Grace (CoreVideo)";
        st.muted          = false;
        st.mixers         = 0x3u;
        const TilesAudioPlan plan =
            plan_tiles_audio({11}, {st}, roster, params_on());
        if (!find_action(plan, TilesAudioActionKind::Mute, 22)) {
            std::cerr << "a participant who left the wall was not muted\n";
            return 1;
        }
    }

    // ── An already-muted absentee is left alone ──────────────────────────────
    {
        TilesAudioSourceState st;
        st.participant_id = 22;
        st.owner_uuid     = kSelf;
        st.name           = "Grace (CoreVideo)";
        st.muted          = true;
        st.mixers         = 0x3u;
        const TilesAudioPlan plan =
            plan_tiles_audio({11}, {st}, roster, params_on());
        if (has_any_for(plan, 22)) {
            std::cerr << "an already-muted absentee was acted on again\n";
            return 1;
        }
    }

    // ── LOAD-BEARING: another Tiles source owns this participant ─────────────
    // No Create, Unmute, or SetMixers. The guard must prevent all action.
    // Two sources for one voice is doubling. The fixture differs from the
    // correct state (muted and wrong mixers) so the guard is what prevents
    // the planner from acting, not just the fact that the state is already right.
    {
        TilesAudioSourceState st;
        st.participant_id = 11;
        st.owner_uuid     = kOther;
        st.name           = "Ada (CoreVideo)";
        st.muted          = true;   // requires Unmute if guard were removed
        st.mixers         = 0x0u;   // requires SetMixers if guard were removed
        const TilesAudioPlan plan =
            plan_tiles_audio({11}, {st}, roster, params_on());
        if (has_any_for(plan, 11)) {
            std::cerr << "acted on a source owned by another Tiles source (muted "
                         "or wrong mixers) — the ownership guard failed\n";
            return 1;
        }
    }

    // ── Another source's absentee is not muted by us ─────────────────────────
    // Muting someone else's source would silence the other wall's audio.
    {
        TilesAudioSourceState st;
        st.participant_id = 33;
        st.owner_uuid     = kOther;
        st.name           = "Katherine (CoreVideo)";
        st.muted          = false;
        st.mixers         = 0x3u;
        const TilesAudioPlan plan =
            plan_tiles_audio({11}, {st}, roster, params_on());
        if (has_any_for(plan, 33)) {
            std::cerr << "muted or altered a source owned by another Tiles "
                         "source\n";
            return 1;
        }
    }

    // ── An orphan is adopted, not duplicated ─────────────────────────────────
    // The Tiles source that made it was deleted; its sources outlive it.
    {
        TilesAudioSourceState st;
        st.participant_id = 11;
        st.owner_uuid     = "";  // orphaned
        st.name           = "Ada (CoreVideo)";
        st.muted          = true;
        st.mixers         = 0u;
        const TilesAudioPlan plan =
            plan_tiles_audio({11}, {st}, roster, params_on());
        if (!find_action(plan, TilesAudioActionKind::Adopt, 11)) {
            std::cerr << "an orphaned source was not adopted\n";
            return 1;
        }
        if (find_action(plan, TilesAudioActionKind::Create, 11)) {
            std::cerr << "an orphaned source was duplicated instead of adopted\n";
            return 1;
        }
        // Adoption must also restore it to service.
        if (!find_action(plan, TilesAudioActionKind::Unmute, 11)) {
            std::cerr << "an adopted source was left muted\n";
            return 1;
        }
        if (!find_action(plan, TilesAudioActionKind::SetMixers, 11)) {
            std::cerr << "an adopted source kept its stale track assignment\n";
            return 1;
        }
    }

    // ── Drifted track assignment is corrected ────────────────────────────────
    {
        TilesAudioSourceState st;
        st.participant_id = 11;
        st.owner_uuid     = kSelf;
        st.name           = "Ada (CoreVideo)";
        st.muted          = false;
        st.mixers         = 0x1u;  // program only; slot 0 should also hold track 2
        const TilesAudioPlan plan =
            plan_tiles_audio({11}, {st}, roster, params_on());
        const TilesAudioAction *a =
            find_action(plan, TilesAudioActionKind::SetMixers, 11);
        if (!a) {
            std::cerr << "a drifted track assignment was not corrected\n";
            return 1;
        }
        if (a->mixers != 0x3u) {
            std::cerr << "corrected mixers " << a->mixers << "; want 3\n";
            return 1;
        }
    }

    // ── The six-track ceiling, degrading honestly ────────────────────────────
    // Track 1 is the program mix and everyone joins it, so tracks 2..6 leave
    // exactly five stems. The sixth participant onward is program-only.
    {
        if (tiles_audio_mixers_for_slot(0) != 0x3u) {
            std::cerr << "slot 0 mixers " << tiles_audio_mixers_for_slot(0)
                      << "; want 3\n";
            return 1;
        }
        if (tiles_audio_mixers_for_slot(4) != 0x21u) {
            std::cerr << "slot 4 mixers " << tiles_audio_mixers_for_slot(4)
                      << "; want 33 (program + track 6)\n";
            return 1;
        }
        if (tiles_audio_mixers_for_slot(5) != 0x1u) {
            std::cerr << "slot 5 mixers " << tiles_audio_mixers_for_slot(5)
                      << "; want 1 (program only — stems exhausted)\n";
            return 1;
        }
        // Every slot must always carry the program bit, or that person goes
        // missing from the live mix entirely.
        for (std::size_t slot = 0; slot < 32; ++slot) {
            if ((tiles_audio_mixers_for_slot(slot) & 0x1u) == 0u) {
                std::cerr << "slot " << slot
                          << " is not on the program track — that person would "
                             "be inaudible live\n";
                return 1;
            }
        }

        const auto big_roster = roster_of({{1, "A"}, {2, "B"}, {3, "C"},
                                           {4, "D"}, {5, "E"}, {6, "F"},
                                           {7, "G"}});
        const TilesAudioPlan plan = plan_tiles_audio(
            {1, 2, 3, 4, 5, 6, 7}, {}, big_roster, params_on());
        if (plan.overflow != 2) {
            std::cerr << "overflow " << plan.overflow
                      << " for a 7-person wall; want 2\n";
            return 1;
        }
        const TilesAudioAction *sixth =
            find_action(plan, TilesAudioActionKind::Create, 6);
        if (!sixth || sixth->mixers != 0x1u) {
            std::cerr << "the sixth participant did not degrade to program-only\n";
            return 1;
        }
    }

    // ── A participant absent from the roster still gets a usable name ────────
    {
        const TilesAudioPlan plan =
            plan_tiles_audio({99}, {}, roster, params_on());
        const TilesAudioAction *a =
            find_action(plan, TilesAudioActionKind::Create, 99);
        if (!a || a->name.empty()) {
            std::cerr << "a participant missing from the roster got no name\n";
            return 1;
        }
        if (a->name.find("99") == std::string::npos) {
            std::cerr << "the fallback name '" << a->name
                      << "' does not identify the participant\n";
            return 1;
        }
    }

    std::cout << "tiles-audio-plan: all tests passed\n";
    return 0;
}
