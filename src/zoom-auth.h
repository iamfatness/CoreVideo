#pragma once
#include <string>
#include <functional>
#include <auth_service_interface.h>

enum class ZoomAuthState { Unauthenticated, Authenticating, Authenticated, Failed };

class ZoomAuth : public ZOOMSDK::IAuthServiceEvent {
public:
    static ZoomAuth &instance();
    bool init(const std::string &sdk_key, const std::string &sdk_secret);
    bool authenticate(const std::string &jwt_token);
    ZoomAuthState state() const { return m_state; }
    void shutdown();

    using StateCallback = std::function<void(ZoomAuthState)>;
    void on_state_change(StateCallback cb) { m_callback = cb; }

    // IAuthServiceEvent
    void onAuthenticationReturn(ZOOMSDK::AuthResult ret) override;
    void onLoginReturnWithReason(ZOOMSDK::LOGINSTATUS ret,
                                 ZOOMSDK::IAccountInfo *pAccountInfo,
                                 ZOOMSDK::LoginFailReason reason) override;
    void onLogout() override;
    void onZoomIdentityExpired() override;
    void onZoomAuthIdentityExpired() override;
#if defined(WIN32)
    void onNotificationServiceStatus(
        ZOOMSDK::SDKNotificationServiceStatus status,
        ZOOMSDK::SDKNotificationServiceError error) override;
#endif

private:
    ZoomAuth() = default;
    ZoomAuthState          m_state        = ZoomAuthState::Unauthenticated;
    StateCallback          m_callback;
    bool                   m_sdk_init     = false;
    ZOOMSDK::IAuthService *m_auth_service = nullptr;
    // Kept alive for the duration of the async SDKAuth call.
#if defined(WIN32)
    std::wstring           m_wide_jwt;
#else
    std::string            m_wide_jwt;
#endif
    std::string            m_last_jwt; // used for auto re-auth on identity expiry
};
