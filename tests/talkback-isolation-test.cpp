// tests/talkback-isolation-test.cpp
// Talkback audio must be structurally incapable of reaching program or ISO.
//
// This is a SOURCE-LEVEL invariant test: it reads the talkback tap's own
// source and asserts it uses only observing APIs, never routing ones. That is
// blunt, and deliberately so -- the alternative is a runtime test that would
// need OBS, a meeting, and a recording, and the property being protected is
// simple enough to state as "these symbols never appear in this file".
//
// The guarantee has two halves and only ONE of them is ours. Our half: a
// capture callback observes a source and cannot add it to a mix, and ISO
// records inbound audio only, so talkback cannot reach it by construction.
// The other half is the operator's: if they pick a source that is itself live
// on a program track, their voice reaches the stream through OBS's own
// routing, entirely outside our path. That is why the dock warns about the
// chosen source's enabled mixer tracks -- see the spec. This test protects
// our half; nothing in code can protect theirs.
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
    // The build passes the source path in, so the test does not depend on the
    // working directory ctest happens to use.
    if (argc < 2) {
        std::cerr << "FAIL: expected the path to talkback-tap.cpp as argv[1]\n";
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

    // Routing APIs: any of these would put talkback into a mix.
    check(src.find("obs_source_output_audio") == std::string::npos,
          "talkback-tap.cpp calls obs_source_output_audio -- that ROUTES audio "
          "into OBS's mix and would put the director on program");
    check(src.find("obs_set_output_source") == std::string::npos,
          "talkback-tap.cpp calls obs_set_output_source -- that assigns a "
          "source to an output channel, which is program");
    check(src.find("obs_source_set_audio_mixers") == std::string::npos,
          "talkback-tap.cpp calls obs_source_set_audio_mixers -- that changes "
          "which program tracks a source feeds");
    check(src.find("obs_sceneitem_add") == std::string::npos,
          "talkback-tap.cpp adds a scene item -- talkback must never appear in "
          "a scene");
    // F5 review-round fix: the original four symbols missed the most
    // plausible route someone would actually add. "Let the director hear
    // themselves" is a natural-sounding feature request, and
    // OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT is exactly the enum value that
    // routes a source's audio to program while implementing it.
    check(src.find("obs_source_set_monitoring_type") == std::string::npos,
          "talkback-tap.cpp calls obs_source_set_monitoring_type -- "
          "OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT routes this source's audio "
          "into program/output, exactly the leak the structural guarantee "
          "exists to rule out");
    check(src.find("obs_source_set_audio_active") == std::string::npos,
          "talkback-tap.cpp calls obs_source_set_audio_active -- forcing a "
          "source's audio path active independent of its own visibility/mix "
          "state can pull it into outputs a merely-tapped source would "
          "never reach");

    // The observing API we DO rely on must still be there: if a refactor
    // removes it, talkback silently stops working and this test should say so
    // rather than passing because all the forbidden symbols are also absent.
    check(src.find("obs_source_add_audio_capture_callback") != std::string::npos,
          "talkback-tap.cpp no longer taps via obs_source_add_audio_capture_"
          "callback -- either talkback is broken or it now routes audio some "
          "other way");

    if (failures == 0)
        std::cout << "talkback-isolation: all tests passed\n";
    return failures == 0 ? 0 : 1;
}
