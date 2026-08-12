// Precedence between a source's own speaker-director properties and the
// global (dock-owned) settings.
//
// The incident this guards (2026-08-11, live show): ZoomSource::apply_settings()
// runs on every source load and property refresh, not only on a user edit, and
// it copied its own exclusion properties into the global settings
// unconditionally. A source that had never been given an exclusion therefore
// wrote 0 over the one the dock held. That is not cosmetic — the excluded
// participant became eligible, got promoted, and was dethroned again the moment
// the exclusion came back, and a dethrone sets directed=0, the one
// SpeakerDirector path that promotes with neither hold nor sensitivity applied.
// On air that was sub-second active-speaker cuts.

#include "speaker-settings-merge.h"

#include <iostream>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

static SpeakerGlobalSettings dock_settings()
{
    SpeakerGlobalSettings s;
    s.sensitivity_ms = 300;
    s.hold_ms        = 2000;
    s.exclude_1      = 16790528;
    s.exclude_2      = 0;
    return s;
}

int main()
{
    // --- A source carrying nothing must leave the global settings alone ---
    {
        const SpeakerSourceValues carries_nothing;
        const SpeakerGlobalSettings out =
            merge_speaker_settings(dock_settings(), carries_nothing);
        check(out.exclude_1 == 16790528,
              "a source with no exclusion property wiped the dock's exclusion");
        check(out.sensitivity_ms == 300,
              "a source with no sensitivity property overwrote the dock's value");
        check(out.hold_ms == 2000,
              "a source with no hold property overwrote the dock's value");
    }

    // --- An explicit value on the source wins ---
    {
        SpeakerSourceValues carries_exclusion;
        carries_exclusion.has_exclude_1 = true;
        carries_exclusion.exclude_1     = 16778240;
        const SpeakerGlobalSettings out =
            merge_speaker_settings(dock_settings(), carries_exclusion);
        check(out.exclude_1 == 16778240,
              "an explicit exclusion on the source did not take effect");
        check(out.hold_ms == 2000,
              "an explicit exclusion disturbed an unrelated value");
    }

    // --- Explicitly clearing on the source is still an edit, and must apply ---
    {
        SpeakerSourceValues clears_exclusion;
        clears_exclusion.has_exclude_1 = true;
        clears_exclusion.exclude_1     = 0;
        const SpeakerGlobalSettings out =
            merge_speaker_settings(dock_settings(), clears_exclusion);
        check(out.exclude_1 == 0,
              "explicitly clearing the exclusion on the source was ignored");
    }

    // --- Timing values follow the same rule, independently of each other ---
    {
        SpeakerSourceValues carries_hold;
        carries_hold.has_hold = true;
        carries_hold.hold_ms  = 4000;
        const SpeakerGlobalSettings out =
            merge_speaker_settings(dock_settings(), carries_hold);
        check(out.hold_ms == 4000, "an explicit hold did not take effect");
        check(out.sensitivity_ms == 300,
              "an explicit hold overwrote sensitivity, which the source did not carry");
    }

    if (failures == 0)
        std::cout << "speaker-settings-merge tests passed\n";
    return failures == 0 ? 0 : 1;
}
