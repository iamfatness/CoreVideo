#include "iso-audio-routing.h"
#include <array>
#include <iostream>

int main()
{
    std::array<int,2> iso{}, normal{};
    int mix=0, audience=0;
    // Alternating, distinguishable participants, plus a meeting-mix callback.
    for (uint32_t speaker : {1u,2u,1u,2u}) {
        mix += 10; // OBS Mix must continue to receive the full meeting.
        bool claimed=false;
        for (uint32_t target=1; target<=2; ++target) {
            if (audio_receives_mix(true,false,true)) iso[target-1] += 1000;
            if (audio_receives_participant(target,speaker,true,true)) iso[target-1] += int(speaker);
            claimed |= audio_claims_audience(true,true);
        }
        if (!claimed) ++audience;
        if (audio_receives_participant(1,speaker,true,false)) normal[0] += int(speaker);
    }
    if (iso != std::array<int,2>{2,4} || audience!=4 || mix!=40 || normal[0]!=2 ||
        !audio_receives_mix(false,false,false) || audio_receives_mix(false,true,false) ||
        !audio_claims_audience(true,false) || audio_receives_participant(0,0,true,true)) {
        std::cerr << "Isolated recording leaked mix/another speaker or changed audience routing\n";
        return 1;
    }
}
