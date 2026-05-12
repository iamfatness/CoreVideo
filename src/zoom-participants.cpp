#include "zoom-participants.h"
#include <algorithm>
#if defined(WIN32)
#include <windows.h>
#endif

ZoomParticipants &ZoomParticipants::instance()
{
    static ZoomParticipants inst;
    return inst;
}

void ZoomParticipants::attach(ZOOMSDK::IMeetingParticipantsController *part_ctrl,
                              ZOOMSDK::IMeetingAudioController       *audio_ctrl)
{
    m_ctrl = part_ctrl;
    if (part_ctrl) {
        part_ctrl->SetEvent(this);
        rebuild_roster();
    }
    m_audio_ctrl = audio_ctrl;
    if (audio_ctrl) audio_ctrl->SetEvent(this);
    fire();
}

void ZoomParticipants::detach()
{
    if (m_audio_ctrl) {
        m_audio_ctrl->SetEvent(nullptr);
        m_audio_ctrl = nullptr;
    }
    if (m_ctrl) {
        m_ctrl->SetEvent(nullptr);
        m_ctrl = nullptr;
    }
    std::lock_guard<std::mutex> lk(m_mtx);
    m_roster.clear();
    m_active_speaker = 0;
}

ParticipantInfo ZoomParticipants::user_to_info(ZOOMSDK::IUserInfo *u)
{
    ParticipantInfo info;
    info.user_id   = u->GetUserID();
    info.has_video = u->IsVideoOn();
    info.is_talking = u->IsTalking();

    const zchar_t *name = u->GetUserName();
#if defined(WIN32)
    if (name) {
        int len = WideCharToMultiByte(CP_UTF8, 0, name, -1,
                                      nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            info.display_name.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, name, -1,
                                &info.display_name[0], len,
                                nullptr, nullptr);
        }
    }
#else
    if (name) info.display_name = name;
#endif
    return info;
}

void ZoomParticipants::rebuild_roster()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_roster.clear();
    m_active_speaker = 0;
    if (!m_ctrl) return;

    auto *list = m_ctrl->GetParticipantsList();
    if (!list) return;

    for (int i = 0; i < list->GetCount(); ++i) {
        unsigned int uid = list->GetItem(i);
        auto *u = m_ctrl->GetUserByUserID(uid);
        if (!u) continue;
        auto info = user_to_info(u);
        m_roster.push_back(info);
        if (info.is_talking) m_active_speaker = uid;
    }
}

void ZoomParticipants::fire()
{
    if (m_cb) m_cb();
}

std::vector<ParticipantInfo> ZoomParticipants::roster() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_roster;
}

uint32_t ZoomParticipants::active_speaker_id() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_active_speaker;
}

// ── IMeetingParticipantsCtrlEvent ─────────────────────────────────────────────

void ZoomParticipants::onUserJoin(ZOOMSDK::IList<unsigned int> *lst, const zchar_t *)
{
    if (!lst || !m_ctrl) return;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (int i = 0; i < lst->GetCount(); ++i) {
            auto *u = m_ctrl->GetUserByUserID(lst->GetItem(i));
            if (!u) continue;
            auto info = user_to_info(u);
            auto it = std::find_if(m_roster.begin(), m_roster.end(),
                [&](const ParticipantInfo &p){ return p.user_id == info.user_id; });
            if (it != m_roster.end()) *it = info;
            else m_roster.push_back(info);
            if (info.is_talking) m_active_speaker = info.user_id;
        }
    }
    fire();
}

void ZoomParticipants::onUserLeft(ZOOMSDK::IList<unsigned int> *lst, const zchar_t *)
{
    if (!lst) return;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (int i = 0; i < lst->GetCount(); ++i) {
            unsigned int uid = lst->GetItem(i);
            m_roster.erase(std::remove_if(m_roster.begin(), m_roster.end(),
                [uid](const ParticipantInfo &p){ return p.user_id == uid; }),
                m_roster.end());
            if (m_active_speaker == uid) m_active_speaker = 0;
        }
    }
    fire();
}

void ZoomParticipants::onUserNamesChanged(ZOOMSDK::IList<unsigned int> *lst)
{
    if (!lst || !m_ctrl) return;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (int i = 0; i < lst->GetCount(); ++i) {
            unsigned int uid = lst->GetItem(i);
            auto *u = m_ctrl->GetUserByUserID(uid);
            if (!u) continue;
            auto info = user_to_info(u);
            for (auto &p : m_roster) {
                if (p.user_id == uid) { p = info; break; }
            }
        }
    }
    fire();
}

// Remaining callbacks — no action needed for roster tracking
void ZoomParticipants::onHostChangeNotification(unsigned int) {}
void ZoomParticipants::onLowOrRaiseHandStatusChanged(bool, unsigned int) {}
void ZoomParticipants::onCoHostChangeNotification(unsigned int, bool) {}
void ZoomParticipants::onInvalidReclaimHostkey() {}
void ZoomParticipants::onAllHandsLowered() {}
void ZoomParticipants::onLocalRecordingStatusChanged(unsigned int,
                                                     ZOOMSDK::RecordingStatus) {}
void ZoomParticipants::onAllowParticipantsRenameNotification(bool) {}
void ZoomParticipants::onAllowParticipantsUnmuteSelfNotification(bool) {}
void ZoomParticipants::onAllowParticipantsStartVideoNotification(bool) {}
void ZoomParticipants::onAllowParticipantsShareWhiteBoardNotification(bool) {}
void ZoomParticipants::onRequestLocalRecordingPrivilegeChanged(
    ZOOMSDK::LocalRecordingRequestPrivilegeStatus) {}
void ZoomParticipants::onAllowParticipantsRequestCloudRecording(bool) {}
void ZoomParticipants::onInMeetingUserAvatarPathUpdated(unsigned int) {}
void ZoomParticipants::onParticipantProfilePictureStatusChange(bool) {}
void ZoomParticipants::onFocusModeStateChanged(bool) {}
void ZoomParticipants::onFocusModeShareTypeChanged(ZOOMSDK::FocusModeShareType) {}
void ZoomParticipants::onBotAuthorizerRelationChanged(unsigned int) {}
void ZoomParticipants::onVirtualNameTagStatusChanged(bool, unsigned int) {}
void ZoomParticipants::onVirtualNameTagRosterInfoUpdated(unsigned int) {}
void ZoomParticipants::onGrantCoOwnerPrivilegeChanged(bool) {}
#if defined(WIN32)
void ZoomParticipants::onCreateCompanionRelation(unsigned int, unsigned int) {}
void ZoomParticipants::onRemoveCompanionRelation(unsigned int) {}
#endif

// ── IMeetingAudioCtrlEvent ────────────────────────────────────────────────────

void ZoomParticipants::onUserActiveAudioChange(ZOOMSDK::IList<unsigned int> *lst)
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_active_speaker = (lst && lst->GetCount() > 0) ? lst->GetItem(0) : 0;
    }
    fire();
}

void ZoomParticipants::onUserAudioStatusChange(
    ZOOMSDK::IList<ZOOMSDK::IUserAudioStatus *> *, const zchar_t *) {}
void ZoomParticipants::onHostRequestStartAudio(ZOOMSDK::IRequestStartAudioHandler *) {}
void ZoomParticipants::onJoin3rdPartyTelephonyAudio(const zchar_t *) {}
void ZoomParticipants::onMuteOnEntryStatusChange(bool) {}
