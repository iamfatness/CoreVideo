#include "zoom-auth.h"
#include <obs-module.h>
#include <zoom_sdk.h>
#include <setting_service_interface.h>

#if defined(WIN32)
#include <windows.h>
static std::wstring to_zstr(const std::string &utf8)
{
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring wide(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], len);
    if (!wide.empty() && wide.back() == L'\0') wide.pop_back();
    return wide;
}
#else
static std::string to_zstr(const std::string &s) { return s; }
#endif

ZoomAuth &ZoomAuth::instance() { static ZoomAuth inst; return inst; }

void ZoomAuth::add_state_callback(void *key, StateCallback cb)
{
    std::lock_guard<std::mutex> lk(m_cbs_mtx);
    if (cb) m_state_cbs[key] = std::move(cb);
    else    m_state_cbs.erase(key);
}

void ZoomAuth::remove_state_callback(void *key)
{
    std::lock_guard<std::mutex> lk(m_cbs_mtx);
    m_state_cbs.erase(key);
}

void ZoomAuth::fire_state_cbs()
{
    // Copy under lock, fire outside to avoid re-entrant lock acquisition
    // (callbacks may call add_state_callback / remove_state_callback).
    std::vector<StateCallback> cbs;
    {
        std::lock_guard<std::mutex> lk(m_cbs_mtx);
        for (auto &[key, cb] : m_state_cbs)
            if (cb) cbs.push_back(cb);
    }
    for (auto &cb : cbs)
        cb(m_state);
}

bool ZoomAuth::init(const std::string &sdk_key, const std::string &sdk_secret)
{
    if (m_sdk_init) {
        blog(LOG_WARNING, "[obs-zoom-plugin] SDK already initialised");
        return true;
    }
    if (sdk_key.empty() || sdk_secret.empty()) {
        blog(LOG_ERROR, "[obs-zoom-plugin] SDK key/secret must not be empty");
        return false;
    }

    ZOOMSDK::InitParam init_param;
    init_param.strWebDomain       = L"https://zoom.us";
    init_param.enableGenerateDump = true;
    init_param.enableLogByDefault = false;

    // Required on all platforms to suppress the Zoom UI and receive raw-data callbacks.
    init_param.obConfigOpts.optionalFeatures = ENABLE_CUSTOMIZED_UI_FLAG;

    // Use heap memory for video raw data — stack frames can be 1080p+ (>3 MB each).
    init_param.rawdataOpts.videoRawdataMemoryMode = ZOOMSDK::ZoomSDKRawDataMemoryModeHeap;
    init_param.rawdataOpts.audioRawdataMemoryMode = ZOOMSDK::ZoomSDKRawDataMemoryModeHeap;

    ZOOMSDK::SDKError err = ZOOMSDK::InitSDK(init_param);
    if (err != ZOOMSDK::SDKERR_SUCCESS) {
        blog(LOG_ERROR, "[obs-zoom-plugin] InitSDK failed: %d", static_cast<int>(err));
        return false;
    }

    err = ZOOMSDK::CreateAuthService(&m_auth_service);
    if (err != ZOOMSDK::SDKERR_SUCCESS || !m_auth_service) {
        blog(LOG_ERROR, "[obs-zoom-plugin] CreateAuthService failed: %d", static_cast<int>(err));
        ZOOMSDK::CleanUPSDK();
        return false;
    }

    m_auth_service->SetEvent(this);
    m_sdk_init = true;
    blog(LOG_INFO, "[obs-zoom-plugin] Zoom SDK v%ls initialised",
         ZOOMSDK::GetSDKVersion());
    return true;
}

bool ZoomAuth::authenticate(const std::string &jwt_token)
{
    if (!m_sdk_init || !m_auth_service) {
        blog(LOG_ERROR, "[obs-zoom-plugin] Cannot authenticate: SDK not initialised");
        return false;
    }
    if (jwt_token.empty()) {
        blog(LOG_ERROR, "[obs-zoom-plugin] JWT token must not be empty");
        return false;
    }

    // Store the converted token as a member so the pointer stays valid for the
    // entire duration of the async SDKAuth call (SDKAuth copies the AuthContext
    // struct by value but the jwt_token field is a raw pointer).
    m_wide_jwt = to_zstr(jwt_token);
    m_last_jwt  = jwt_token;

    ZOOMSDK::AuthContext ctx;
    ctx.jwt_token = m_wide_jwt.c_str();

    ZOOMSDK::SDKError err = m_auth_service->SDKAuth(ctx);
    if (err != ZOOMSDK::SDKERR_SUCCESS) {
        blog(LOG_ERROR, "[obs-zoom-plugin] SDKAuth failed: %d", static_cast<int>(err));
        return false;
    }

    m_state = ZoomAuthState::Authenticating;
    fire_state_cbs();
    blog(LOG_INFO, "[obs-zoom-plugin] Authentication requested");
    return true;
}

void ZoomAuth::shutdown()
{
    if (!m_sdk_init) return;

    if (m_setting_service) {
        ZOOMSDK::DestroySettingService(m_setting_service);
        m_setting_service = nullptr;
    }

    if (m_auth_service) {
        m_auth_service->SetEvent(nullptr);
        ZOOMSDK::DestroyAuthService(m_auth_service);
        m_auth_service = nullptr;
    }

    // CleanUPSDK must NOT be called from inside any SDK callback.
    // OBS calls this from obs_module_unload / frontend EXIT event — safe.
    ZOOMSDK::CleanUPSDK();
    m_sdk_init = false;
    m_state    = ZoomAuthState::Unauthenticated;
    m_last_jwt.clear();
    blog(LOG_INFO, "[obs-zoom-plugin] Zoom SDK shut down");
}

// ── IAuthServiceEvent ────────────────────────────────────────────────────────

void ZoomAuth::apply_quality_preferences()
{
    // The setting service only exists after a successful SDKAuth — calling
    // CreateSettingService before that returns SDKERR_SERVICE_FAILED.
    ZOOMSDK::SDKError err = ZOOMSDK::CreateSettingService(&m_setting_service);
    if (err != ZOOMSDK::SDKERR_SUCCESS || !m_setting_service) {
        blog(LOG_WARNING,
             "[obs-zoom-plugin] CreateSettingService failed (%d) — "
             "HD video preference will use SDK default",
             static_cast<int>(err));
        m_setting_service = nullptr;
        return;
    }

    ZOOMSDK::IVideoSettingContext *vs = m_setting_service->GetVideoSettings();
    if (!vs) {
        blog(LOG_WARNING,
             "[obs-zoom-plugin] Video setting context unavailable — "
             "HD video preference will use SDK default");
        return;
    }

    err = vs->EnableHDVideo(true);
    if (err == ZOOMSDK::SDKERR_SUCCESS) {
        blog(LOG_INFO,
             "[obs-zoom-plugin] HD video enabled (IsHDVideoEnabled=%d). "
             "Group HD / 1080p streams require account entitlement.",
             vs->IsHDVideoEnabled() ? 1 : 0);
    } else {
        // SDKERR_NO_PERMISSION here typically means the account tier
        // doesn't include Group HD — not a hard failure, the meeting
        // will just negotiate at 720p or lower.
        blog(LOG_WARNING,
             "[obs-zoom-plugin] EnableHDVideo(true) returned %d — "
             "the account may not be entitled to Group HD",
             static_cast<int>(err));
    }
}

void ZoomAuth::onAuthenticationReturn(ZOOMSDK::AuthResult ret)
{
    if (ret == ZOOMSDK::AUTHRET_SUCCESS) {
        m_state = ZoomAuthState::Authenticated;
        blog(LOG_INFO, "[obs-zoom-plugin] Authentication succeeded");
        apply_quality_preferences();
    } else {
        m_state = ZoomAuthState::Failed;
        blog(LOG_ERROR, "[obs-zoom-plugin] Authentication failed: %d", static_cast<int>(ret));
    }
    fire_state_cbs();
}

void ZoomAuth::onLoginReturnWithReason(ZOOMSDK::LOGINSTATUS ret,
                                       ZOOMSDK::IAccountInfo *,
                                       ZOOMSDK::LoginFailReason reason)
{
    blog(LOG_WARNING, "[obs-zoom-plugin] onLoginReturnWithReason: status=%d reason=%d",
         static_cast<int>(ret), static_cast<int>(reason));
}

void ZoomAuth::onLogout()
{
    m_state = ZoomAuthState::Unauthenticated;
    blog(LOG_INFO, "[obs-zoom-plugin] Logged out");
    fire_state_cbs();
}

void ZoomAuth::onZoomIdentityExpired()
{
    blog(LOG_WARNING, "[obs-zoom-plugin] Zoom identity expired — re-authenticating");
    if (!m_last_jwt.empty()) {
        authenticate(m_last_jwt);
    } else {
        m_state = ZoomAuthState::Failed;
        fire_state_cbs();
    }
}

void ZoomAuth::onZoomAuthIdentityExpired()
{
    blog(LOG_WARNING, "[obs-zoom-plugin] Auth identity expiring — re-authenticating proactively");
    if (!m_last_jwt.empty())
        authenticate(m_last_jwt);
}

#if defined(WIN32)
void ZoomAuth::onNotificationServiceStatus(
    ZOOMSDK::SDKNotificationServiceStatus status,
    ZOOMSDK::SDKNotificationServiceError error)
{
    blog(LOG_INFO, "[obs-zoom-plugin] Notification service status=%d error=%d",
         static_cast<int>(status), static_cast<int>(error));
}
#endif
