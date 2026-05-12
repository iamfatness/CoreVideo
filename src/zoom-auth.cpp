#include "zoom-auth.h"
#include <obs-module.h>
// zoom_sdk.h provides InitSDK, CleanUPSDK, CreateAuthService — copy from SDK h/ folder
#include "zoom_sdk.h"
#include <string>

#if defined(WIN32)
// zchar_t is wchar_t on Windows
static std::wstring to_zstr(const std::string &s)
{
    return std::wstring(s.begin(), s.end());
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

    ZOOM_SDK_NAMESPACE::InitParam init_param;
    init_param.strWebDomain      = L"https://zoom.us";
    init_param.enableGenerateDump = true;

    ZOOM_SDK_NAMESPACE::SDKError err = ZOOM_SDK_NAMESPACE::InitSDK(init_param);
    if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
        blog(LOG_ERROR, "[obs-zoom-plugin] InitSDK failed: %d", static_cast<int>(err));
        return false;
    }

    err = ZOOM_SDK_NAMESPACE::CreateAuthService(&m_auth_service);
    if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS || !m_auth_service) {
        blog(LOG_ERROR, "[obs-zoom-plugin] CreateAuthService failed: %d", static_cast<int>(err));
        ZOOM_SDK_NAMESPACE::CleanUPSDK();
        return false;
    }

    m_auth_service->SetEvent(this);
    m_sdk_init = true;
    blog(LOG_INFO, "[obs-zoom-plugin] Zoom SDK initialised");
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

    ZOOM_SDK_NAMESPACE::AuthContext ctx;
    ctx.jwt_token = wide_token.c_str();

    ZOOM_SDK_NAMESPACE::SDKError err = m_auth_service->SDKAuth(ctx);
    if (err != ZOOM_SDK_NAMESPACE::SDKERR_SUCCESS) {
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
        m_auth_service = nullptr;
    }
    ZOOM_SDK_NAMESPACE::CleanUPSDK();
    m_sdk_init = false;
    m_state    = ZoomAuthState::Unauthenticated;
    blog(LOG_INFO, "[obs-zoom-plugin] Zoom SDK shut down");
}

// ── IAuthServiceEvent callbacks ──────────────────────────────────────────────

void ZoomAuth::onAuthenticationReturn(ZOOM_SDK_NAMESPACE::AuthResult ret)
{
    if (ret == ZOOM_SDK_NAMESPACE::AUTHRET_SUCCESS) {
        m_state = ZoomAuthState::Authenticated;
        blog(LOG_INFO, "[obs-zoom-plugin] Authentication succeeded");
    } else {
        m_state = ZoomAuthState::Failed;
        blog(LOG_ERROR, "[obs-zoom-plugin] Authentication failed: %d", static_cast<int>(ret));
    }
    if (m_callback) m_callback(m_state);
}

void ZoomAuth::onLoginReturnWithReason(ZOOM_SDK_NAMESPACE::LOGINSTATUS ret,
                                       ZOOM_SDK_NAMESPACE::IAccountInfo *,
                                       ZOOM_SDK_NAMESPACE::LoginFailReason reason)
{
    // JWT-only flow does not use login; log unexpected calls
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
    ZOOM_SDK_NAMESPACE::SDKNotificationServiceStatus status,
    ZOOM_SDK_NAMESPACE::SDKNotificationServiceError error)
{
    blog(LOG_INFO, "[obs-zoom-plugin] Notification service status=%d error=%d",
         static_cast<int>(status), static_cast<int>(error));
}
#endif
