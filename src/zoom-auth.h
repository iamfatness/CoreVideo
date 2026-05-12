#pragma once
#include <string>
#include <functional>
enum class ZoomAuthState { Unauthenticated, Authenticating, Authenticated, Failed };
class ZoomAuth {
public:
    static ZoomAuth &instance();
    bool init(const std::string &sdk_key, const std::string &sdk_secret);
    bool authenticate(const std::string &jwt_token);
    ZoomAuthState state() const { return m_state; }
    void shutdown();
    using StateCallback = std::function<void(ZoomAuthState)>;
    void on_state_change(StateCallback cb) { m_callback = cb; }
private:
    ZoomAuth() = default;
    ZoomAuthState m_state = ZoomAuthState::Unauthenticated;
    StateCallback m_callback;
    bool m_sdk_init = false;
};
