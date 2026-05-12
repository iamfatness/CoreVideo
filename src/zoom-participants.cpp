#include "zoom-participants.h"
#include <obs-module.h>
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
                              ZOOMSDK::IMeetingAudioController        *audio_ctrl,
                              ZOOMSDK::IMeetingVideoController        *video_ctrl)
{
    m_ctrl = part_ctrl;
    if (part_ctrl) {
        part_ctrl->SetEvent(this);
        rebuild_roster();
    }
    m_audio_ctrl = audio_ctrl;
    if (audio_ctrl) audio_ctrl->SetEvent(this);
    m_video_ctrl = video_ctrl;
    if (video_ctrl) video_ctrl->SetEvent(this);
    fire();
}

void ZoomParticipants::detach()
{
    if (m_video_ctrl) {
        m_video_ctrl->SetEvent(nullptr);
        m_video_ctrl = nullptr;
    }
    if (m_audio_ctrl) {
        m_audio_ctrl->SetEvent(nullptr);
        m_audio_ctrl = nullptr;
    }
    if (m_ctrl) {
        m_ctrl->SetEvent(nullptr);
        m_ctrl = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_roster.clear();
        m_active_speaker = 0;
    }
    fire();
}

ParticipantInfo ZoomParticipants::user_to_info(ZOOMSDK::IUserInfo *u)
{
    ParticipantInfo info;
    info.user_id    = u->GetUserID();
    info.has_video  = u->IsVideoOn();
    info.is_talking = u->IsTalking();
    info.is_muted   = u->IsAudioMuted();

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
    // Snapshot under lock; dispatch to main thread so callbacks can safely call
    // SDK functions (e.g. createRenderer) without SDK re-entrancy issues, and
    // so they are serialized with source creation / destruction.
    std::vector<RosterCallback> callbacks;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (auto &[key, cb] : m_cbs)
            if (cb) callbacks.push_back(cb);
    }
    if (callbacks.empty()) return;

    struct Payload { std::vector<RosterCallback> cbs; };
    auto *p = new Payload{std::move(callbacks)};
    obs_queue_task(OBS_TASK_UI, [](void *ptr) {
        auto *d = static_cast<Payload *>(ptr);
        for (auto &cb : d->cbs) cb();
        delete d;
    }, p, false);
}

void ZoomParticipants::add_roster_callback(void *key, RosterCallback cb)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (cb)
        m_cbs[key] = std::move(cb);
    else
        m_cbs.erase(key);
}

void ZoomParticipants::remove_roster_callback(void *key)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_cbs.erase(key);
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
void ZoomParticipants::onCreateCompanionRelation(unsigned int, unsigned int) {}
void ZoomParticipants::onRemoveCompanionRelation(unsigned int) {}

// ── IMeetingAudioCtrlEvent ────────────────────────────────────────────────────

void ZoomParticipants::onUserActiveAudioChange(ZOOMSDK::IList<unsigned int> *lst)
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_active_speaker = (lst && lst->GetCount() > 0) ? lst->GetItem(0) : 0;
        for (auto &p : m_roster)
            p.is_talking = false;
        if (lst) {
            for (int i = 0; i < lst->GetCount(); ++i) {
                const uint32_t uid = lst->GetItem(i);
                for (auto &p : m_roster) {
                    if (p.user_id == uid) {
                        p.is_talking = true;
                        break;
                    }
                }
            }
        }
    }
    fire();
}

void ZoomParticipants::onUserAudioStatusChange(
    ZOOMSDK::IList<ZOOMSDK::IUserAudioStatus *> *lst, const zchar_t *)
{
    if (!lst) return;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (int i = 0; i < lst->GetCount(); ++i) {
            auto *status = lst->GetItem(i);
            if (!status) continue;
            const uint32_t uid = status->GetUserId();
            const auto audio_status = status->GetStatus();
            for (auto &p : m_roster) {
                if (p.user_id != uid) continue;
                p.is_muted = audio_status == ZOOMSDK::Audio_Muted ||
                             audio_status == ZOOMSDK::Audio_Muted_ByHost ||
                             audio_status == ZOOMSDK::Audio_MutedAll_ByHost;
                break;
            }
        }
    }
    fire();
}
void ZoomParticipants::onHostRequestStartAudio(ZOOMSDK::IRequestStartAudioHandler *) {}
void ZoomParticipants::onJoin3rdPartyTelephonyAudio(const zchar_t *) {}
void ZoomParticipants::onMuteOnEntryStatusChange(bool) {}

// ── IMeetingVideoCtrlEvent ────────────────────────────────────────────────────

void ZoomParticipants::onUserVideoStatusChange(unsigned int userId,
                                               ZOOMSDK::VideoStatus status)
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        const bool on = (status == ZOOMSDK::Video_ON);
        for (auto &p : m_roster) {
            if (p.user_id == static_cast<uint32_t>(userId)) {
                p.has_video = on;
                break;
            }
        }
    }
    fire();
}
