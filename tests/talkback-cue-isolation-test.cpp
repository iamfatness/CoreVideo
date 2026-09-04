// tests/talkback-cue-isolation-test.cpp
// The talkback audio cue must be structurally incapable of reaching program
// or ISO, same as the talkback tap itself.
//
// This is a SOURCE-LEVEL invariant test, in the exact shape of
// tests/talkback-isolation-test.cpp (the tap's version) -- read that file's
// header comment for why a source scan is the right test here: the
// alternative is a runtime test needing OBS, a meeting, and a recording, and
// the property is simple enough to state as "these symbols never appear in
// this file".
//
// Why this file needs its OWN copy of the guarantee rather than relying on
// the tap's: talkback-cue.cpp is a second, independent place audio-shaped
// code was added to this plugin. Nothing stops a future "let's route the
// cue through OBS so it lands on the monitor mix" change from landing here
// instead of in talkback-tap.cpp, where the tap's test would never see it.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static int failures = 0;

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

int main(int argc, char **argv)
{
    // The build passes the source path in, so the test does not depend on
    // the working directory ctest happens to use.
    if (argc < 2) {
        std::cerr << "FAIL: expected the path to talkback-cue.cpp as argv[1]\n";
        return 1;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        std::cerr << "FAIL: could not open " << argv[1] << "\n";
        return 1;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();

    // Routing APIs: any of these would put the cue into a mix. Same list
    // talkback-isolation-test.cpp forbids in talkback-tap.cpp -- this file
    // must never gain a reason to call any of them either.
    check(src.find("obs_source_output_audio") == std::string::npos,
          "talkback-cue.cpp calls obs_source_output_audio -- that ROUTES "
          "audio into OBS's mix and would put the cue on program");
    check(src.find("obs_set_output_source") == std::string::npos,
          "talkback-cue.cpp calls obs_set_output_source -- that assigns a "
          "source to an output channel, which is program");
    check(src.find("obs_source_set_audio_mixers") == std::string::npos,
          "talkback-cue.cpp calls obs_source_set_audio_mixers -- that "
          "changes which program tracks a source feeds");
    check(src.find("obs_sceneitem_add") == std::string::npos,
          "talkback-cue.cpp adds a scene item -- the cue must never appear "
          "in a scene");
    check(src.find("obs_source_set_monitoring_type") == std::string::npos,
          "talkback-cue.cpp calls obs_source_set_monitoring_type -- "
          "OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT routes a source's audio "
          "into program/output, exactly the leak the structural guarantee "
          "exists to rule out");
    check(src.find("obs_source_set_audio_active") == std::string::npos,
          "talkback-cue.cpp calls obs_source_set_audio_active -- forcing a "
          "source's audio path active independent of its own visibility/mix "
          "state can pull it into outputs the cue would never otherwise "
          "reach");
    // The cue plays through the OS, not through any OBS source at all --
    // unlike the tap, it has no legitimate reason to touch obs_source_*
    // audio APIs whatsoever. Broaden the net accordingly: no libobs symbol
    // prefix should appear here at all.
    check(src.find("obs_source_") == std::string::npos,
          "talkback-cue.cpp references an obs_source_* symbol -- this file "
          "plays a generated tone through the OS and has no legitimate "
          "reason to touch any OBS source API");

    // The playback API we DO rely on must still be there: if a refactor
    // removes it, the cue silently stops playing and this test should say
    // so rather than passing because all the forbidden symbols are also
    // absent.
    check(src.find("PlaySound") != std::string::npos,
          "talkback-cue.cpp no longer calls PlaySound -- either the cue is "
          "broken or it now plays some other way this test hasn't been "
          "taught to recognise");

    if (failures == 0)
        std::cout << "talkback-cue-isolation: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
