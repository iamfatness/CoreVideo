#pragma once

#include <atomic>
#include <cstdint>
#if defined(_WIN32)
#include <windows.h>
#endif
#if __has_include(<rawdata/rawdata_audio_helper_interface.h>)
#include <rawdata/rawdata_audio_helper_interface.h>
#else
#include <rawdata_audio_helper_interface.h>
#endif

// SDK capture source with no physical device and no outgoing PCM. Keep alive
// until after CleanUPSDK; raw audio RECEIVE subscriptions are independent.
class EngineSilentMic final : public ZOOMSDK::IZoomSDKVirtualAudioMicEvent {
public:
    ZOOMSDK::SDKError install(ZOOMSDK::IZoomSDKAudioRawDataHelper *helper)
    {
        reset();
        const auto result = helper ? helper->setExternalAudioSource(this)
                                   : ZOOMSDK::SDKERR_UNINITIALIZE;
        m_ready.store(result == ZOOMSDK::SDKERR_SUCCESS);
        return result;
    }
    void reset() { m_ready.store(false); }
    bool ready() const { return m_ready.load(); }
    void onMicInitialize(ZOOMSDK::IZoomSDKAudioRawDataSender *) override {}
    void onMicStartSend() override {}
    void onMicStopSend() override {}
    void onMicUninitialized() override {}

private:
    std::atomic<bool> m_ready{false};
};
