#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include "../third_party/zoom-sdk/h/meeting_participants_ctrl_interface.h"

struct ParticipantInfo {
    uint32_t    user_id      = 0;
    std::string display_name;
    bool        has_video    = false;
    bool        is_talking   = false;
};

class ZoomParticipants : public ZOOMSDK::IMeetingParticipantsCtrlEvent {
public:
    static ZoomParticipants &instance();

    void attach(ZOOMSDK::IMeetingParticipantsController *ctrl);
    void detach();

    std::vector<ParticipantInfo> roster() const;
    uint32_t active_speaker_id() const;

    using RosterCallback = std::function<void()>;
    void on_roster_change(RosterCallback cb) { m_cb = std::move(cb); }

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
#if defined(WIN32)
    void onCreateCompanionRelation(unsigned int parentUserID,
                                   unsigned int childUserID) override;
    void onRemoveCompanionRelation(unsigned int childUserID) override;
#endif

private:
    ZoomParticipants() = default;
    void rebuild_roster();
    ParticipantInfo user_to_info(ZOOMSDK::IUserInfo *u);
    void fire();

    ZOOMSDK::IMeetingParticipantsController *m_ctrl = nullptr;
    mutable std::mutex              m_mtx;
    std::vector<ParticipantInfo>    m_roster;
    uint32_t                        m_active_speaker = 0;
    RosterCallback                  m_cb;
};
