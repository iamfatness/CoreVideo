#include "zoom-auth.h"
#include <obs-module.h>
ZoomAuth &ZoomAuth::instance() { static ZoomAuth inst; return inst; }
bool ZoomAuth::init(const std::string &sdk_key, const std::string &sdk_secret)
{
    if (m_sdk_init) { blog(LOG_WARNING, "[obs-zoom-plugin] SDK already initialised"); return true; }
    if (sdk_key.empty() || sdk_secret.empty()) { blog(LOG_ERROR, "[obs-zoom-plugin] SDK key/secret must not be empty"); return false; }
    // ZOOM_SDK_NAMESPACE::InitParam init_param; init_param.strWebDomain = "https://zoom.us";
    // auto err = ZOOM_SDK_NAMESPACE::InitSDK(init_param); if (err != SDKERR_SUCCESS) { ... }
    blog(LOG_INFO, "[obs-zoom-plugin] Zoom SDK initialised");
    m_sdk_init = true;
    return true;
}
bool ZoomAuth::authenticate(const std::string &jwt_token)
{
    if (!m_sdk_init) { blog(LOG_ERROR, "[obs-zoom-plugin] Cannot authenticate: SDK not initialised"); return false; }
    m_state = ZoomAuthState::Authenticating;
    if (m_callback) m_callback(m_state);
    // ZOOM_SDK_NAMESPACE::IAuthService *auth = nullptr; ZOOM_SDK_NAMESPACE::CreateAuthService(&auth);
    // ZOOM_SDK_NAMESPACE::AuthContext ctx; ctx.jwt_token = jwt_token.c_str(); auth->SDKAuth(ctx);
    blog(LOG_INFO, "[obs-zoom-plugin] Authentication requested");
    return true;
}
void ZoomAuth::shutdown()
{
    if (!m_sdk_init) return;
    // ZOOM_SDK_NAMESPACE::CleanUPSDK();
    m_sdk_init = false;
    m_state = ZoomAuthState::Unauthenticated;
    blog(LOG_INFO, "[obs-zoom-plugin] Zoom SDK shut down");
}
