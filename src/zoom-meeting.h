#pragma once
#include <string>
#include <functional>
enum class MeetingState { Idle, Joining, InMeeting, Leaving, Failed };
class ZoomMeeting {
public:
    static ZoomMeeting &instance();
    bool join(const std::string &meeting_id, const std::string &passcode = "");
    void leave();
    MeetingState state() const { return m_state; }
    using VideoFrameCallback = std::function<void(const uint8_t *, uint32_t, uint32_t)>;
    using AudioCallback      = std::function<void(const float *, size_t, uint32_t)>;
    using StateCallback      = std::function<void(MeetingState)>;
    void on_video_frame(VideoFrameCallback cb) { m_video_cb = cb; }
    void on_audio(AudioCallback cb)            { m_audio_cb = cb; }
    void on_state_change(StateCallback cb)     { m_state_cb = cb; }
private:
    ZoomMeeting() = default;
    MeetingState       m_state = MeetingState::Idle;
    VideoFrameCallback m_video_cb;
    AudioCallback      m_audio_cb;
    StateCallback      m_state_cb;
};
