#include "engine-silent-mic.h"
#include <cstdlib>
#include <iostream>

using namespace ZOOMSDK;
static void require(bool value)
{
    if (!value) {
        std::cerr << "Silent microphone regression failed\n";
        std::exit(1);
    }
}
struct Sender : IZoomSDKAudioRawDataSender {
    int sent = 0;
    SDKError send(char *, unsigned int, int, ZoomSDKAudioChannel) override
    {
        ++sent;
        return SDKERR_SUCCESS;
    }
};
struct Helper : IZoomSDKAudioRawDataHelper {
    IZoomSDKVirtualAudioMicEvent *source = nullptr;
    int receive_changes = 0;
    SDKError result = SDKERR_SUCCESS;
    SDKError subscribe(IZoomSDKAudioRawDataDelegate *, bool) override
    {
        ++receive_changes;
        return SDKERR_SUCCESS;
    }
    SDKError unSubscribe() override { ++receive_changes; return SDKERR_SUCCESS; }
    SDKError setExternalAudioSource(IZoomSDKVirtualAudioMicEvent *value) override
    {
        source = value;
        return result;
    }
};
int main()
{
    EngineSilentMic mic;
    Helper helper;
    Sender sender;
    require(!mic.ready());
    require(mic.install(nullptr) != SDKERR_SUCCESS && !mic.ready());
    require(mic.install(&helper) == SDKERR_SUCCESS && mic.ready());
    require(helper.source == &mic);
    for (int session = 0; session < 2; ++session) {
        helper.source->onMicInitialize(&sender);
        helper.source->onMicStartSend();
        helper.source->onMicStopSend();
        helper.source->onMicUninitialized();
    }
    require(sender.sent == 0 && helper.receive_changes == 0);
    mic.reset();
    require(!mic.ready());
    require(mic.install(&helper) == SDKERR_SUCCESS && mic.ready());
    helper.result = SDKERR_UNINITIALIZE;
    require(mic.install(&helper) != SDKERR_SUCCESS && !mic.ready());
}
