#include "zoom-auth.h"
#include <obs-module.h>
#include <zoom_sdk.h>

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
    init_param.strWebDomain      = L"https://zoom.us";
    init_param.enableGenerateDump = true;
    init_param.enableLogByDefault = false;

    // Required to receive raw video/audio data callbacks
#if defined(WIN32)
    init_param.obConfigOpts.optionalFeatures = ENABLE_CUSTOMIZED_UI_FLAG;
#endif

    // Use heap memory for video raw data — stack frames can be 1080p+ (>3 MB each)
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

    auto wide_token = to_zstr(jwt_token);

    ZOOMSDK::AuthContext ctx;
    ctx.jwt_token = wide_token.c_str();

    ZOOMSDK::SDKError err = m_auth_service->SDKAuth(ctx);
    if (err != ZOOMSDK::SDKERR_SUCCESS) {
        blog(LOG_ERROR, "[obs-zoom-plugin] SDKAuth failed: %d", static_cast<int>(err));
        return false;
    }

    m_state = ZoomAuthState::Authenticating;
    if (m_callback) m_callback(m_state);
    blog(LOG_INFO, "[obs-zoom-plugin] Authentication requested");
    return true;
}

void ZoomAuth::shutdown()
{
    if (!m_sdk_init) return;

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
    blog(LOG_INFO, "[obs-zoom-plugin] Zoom SDK shut down");
}

// ── IAuthServiceEvent ────────────────────────────────────────────────────────

void ZoomAuth::onAuthenticationReturn(ZOOMSDK::AuthResult ret)
{
    if (ret == ZOOMSDK::AUTHRET_SUCCESS) {
        m_state = ZoomAuthState::Authenticated;
        blog(LOG_INFO, "[obs-zoom-plugin] Authentication succeeded");
    } else {
        m_state = ZoomAuthState::Failed;
        blog(LOG_ERROR, "[obs-zoom-plugin] Authentication failed: %d", static_cast<int>(ret));
    }
    if (m_callback) m_callback(m_state);
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
    if (m_callback) m_callback(m_state);
}

void ZoomAuth::onZoomIdentityExpired()
{
    blog(LOG_WARNING, "[obs-zoom-plugin] Zoom identity expired — re-authenticate");
    m_state = ZoomAuthState::Failed;
    if (m_callback) m_callback(m_state);
}

void ZoomAuth::onZoomAuthIdentityExpired()
{
    blog(LOG_WARNING, "[obs-zoom-plugin] Auth identity expiring — re-authenticate soon");
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
