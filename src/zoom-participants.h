#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <meeting_service_components/meeting_participants_ctrl_interface.h>
#include <meeting_service_components/meeting_audio_interface.h>
#include <meeting_service_components/meeting_video_interface.h>

struct ParticipantInfo {
    uint32_t    user_id      = 0;
    std::string display_name;
    bool        has_video    = false;
    bool        is_talking   = false;
    bool        is_muted     = false;
};

class ZoomParticipants : public ZOOMSDK::IMeetingParticipantsCtrlEvent,
                         public ZOOMSDK::IMeetingAudioCtrlEvent,
                         public ZOOMSDK::IMeetingVideoCtrlEvent {
public:
    static ZoomParticipants &instance();

    void attach(ZOOMSDK::IMeetingParticipantsController *part_ctrl,
                ZOOMSDK::IMeetingAudioController        *audio_ctrl,
                ZOOMSDK::IMeetingVideoController        *video_ctrl);
    void detach();

    std::vector<ParticipantInfo> roster() const;
    uint32_t active_speaker_id() const;

    using RosterCallback = std::function<void()>;
    void add_roster_callback(void *key, RosterCallback cb);
    void remove_roster_callback(void *key);

    // IMeetingParticipantsCtrlEvent
    void onUserJoin(ZOOMSDK::IList<unsigned int> *lstUserID,
                    const zchar_t *strUserList) override;
    void onUserLeft(ZOOMSDK::IList<unsigned int> *lstUserID,
                    const zchar_t *strUserList) override;
    void onHostChangeNotification(unsigned int userId) override;
    void onLowOrRaiseHandStatusChanged(bool bLow, unsigned int userid) override;
    void onUserNamesChanged(ZOOMSDK::IList<unsigned int> *lstUserID) override;
    void onCoHostChangeNotification(unsigned int userId, bool isCoHost) override;
    void onInvalidReclaimHostkey() override;
    void onAllHandsLowered() override;
    void onLocalRecordingStatusChanged(unsigned int user_id,
                                       ZOOMSDK::RecordingStatus status) override;
    void onAllowParticipantsRenameNotification(bool bAllow) override;
    void onAllowParticipantsUnmuteSelfNotification(bool bAllow) override;
    void onAllowParticipantsStartVideoNotification(bool bAllow) override;
    void onAllowParticipantsShareWhiteBoardNotification(bool bAllow) override;
    void onRequestLocalRecordingPrivilegeChanged(
        ZOOMSDK::LocalRecordingRequestPrivilegeStatus status) override;
    void onAllowParticipantsRequestCloudRecording(bool bAllow) override;
    void onInMeetingUserAvatarPathUpdated(unsigned int userID) override;
    void onParticipantProfilePictureStatusChange(bool bHidden) override;
    void onFocusModeStateChanged(bool bEnabled) override;
    void onFocusModeShareTypeChanged(ZOOMSDK::FocusModeShareType type) override;
    void onBotAuthorizerRelationChanged(unsigned int authorizeUserID) override;
    void onVirtualNameTagStatusChanged(bool bOn, unsigned int userID) override;
    void onVirtualNameTagRosterInfoUpdated(unsigned int userID) override;
    void onGrantCoOwnerPrivilegeChanged(bool canGrantOther) override;
    void onCreateCompanionRelation(unsigned int parentUserID,
                                   unsigned int childUserID) override;
    void onRemoveCompanionRelation(unsigned int childUserID) override;

    // IMeetingVideoCtrlEvent — used to keep has_video flag current
    void onUserVideoStatusChange(unsigned int userId,
                                 ZOOMSDK::VideoStatus status) override;
    void onActiveSpeakerVideoUserChanged(unsigned int userId) override {}
    void onActiveVideoUserChanged(unsigned int userId) override {}
    void onSpotlightedUserListChangeNotification(
        ZOOMSDK::IList<unsigned int> *) override {}
    void onHostRequestStartVideo(
        ZOOMSDK::IRequestStartVideoHandler *) override {}
    void onUserVideoQualityChanged(ZOOMSDK::VideoConnectionQuality,
                                   unsigned int) override {}
    void onHostVideoOrderUpdated(ZOOMSDK::IList<unsigned int> *) override {}
    void onLocalVideoOrderUpdated(ZOOMSDK::IList<unsigned int> *) override {}
    void onFollowHostVideoOrderChanged(bool) override {}
    void onVideoAlphaChannelStatusChanged(bool) override {}
    void onCameraControlRequestReceived(
        unsigned int, ZOOMSDK::CameraControlRequestType,
        ZOOMSDK::ICameraControlRequestHandler *) override {}
    void onCameraControlRequestResult(
        unsigned int, ZOOMSDK::CameraControlRequestResult) override {}

    // IMeetingAudioCtrlEvent — used for reliable active-speaker tracking
    void onUserAudioStatusChange(
        ZOOMSDK::IList<ZOOMSDK::IUserAudioStatus *> *lstAudioStatusChange,
        const zchar_t *strAudioStatusList) override;
    void onUserActiveAudioChange(ZOOMSDK::IList<unsigned int> *plstActiveAudio) override;
    void onHostRequestStartAudio(ZOOMSDK::IRequestStartAudioHandler *handler_) override;
    void onJoin3rdPartyTelephonyAudio(const zchar_t *audioInfo) override;
    void onMuteOnEntryStatusChange(bool bEnabled) override;

private:
    ZoomParticipants() = default;
    void rebuild_roster();
    ParticipantInfo user_to_info(ZOOMSDK::IUserInfo *u);
    void fire();

    ZOOMSDK::IMeetingParticipantsController *m_ctrl       = nullptr;
    ZOOMSDK::IMeetingAudioController        *m_audio_ctrl = nullptr;
    ZOOMSDK::IMeetingVideoController        *m_video_ctrl = nullptr;
    mutable std::mutex              m_mtx;
    std::vector<ParticipantInfo>    m_roster;
    uint32_t                        m_active_speaker = 0;
    std::unordered_map<void *, RosterCallback> m_cbs;
};
