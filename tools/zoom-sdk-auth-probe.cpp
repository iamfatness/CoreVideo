#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <auth_service_interface.h>
#include <zoom_sdk.h>

#if defined(_WIN32)
static std::wstring to_zoom_string(const std::string &utf8)
{
    if (utf8.empty())
        return {};
    const int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        utf8.c_str(), -1, nullptr, 0);
    if (len <= 0)
        return {};
    std::wstring wide(static_cast<size_t>(len), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            utf8.c_str(), -1, wide.data(), len);
    if (written <= 0)
        return {};
    if (!wide.empty() && wide.back() == L'\0')
        wide.pop_back();
    return wide;
}
#else
static std::string to_zoom_string(const std::string &s) { return s; }
#endif

class AuthProbe final : public ZOOMSDK::IAuthServiceEvent {
public:
    void onAuthenticationReturn(ZOOMSDK::AuthResult ret) override
    {
        result = static_cast<int>(ret);
        done = true;
        std::cout << "AUTH_CALLBACK code=" << result.load() << std::endl;
    }

    void onLoginReturnWithReason(ZOOMSDK::LOGINSTATUS,
                                 ZOOMSDK::IAccountInfo *,
                                 ZOOMSDK::LoginFailReason) override
    {
    }
    void onLogout() override {}
    void onZoomIdentityExpired() override {}
    void onZoomAuthIdentityExpired() override {}
#if defined(WIN32)
    void onNotificationServiceStatus(ZOOMSDK::SDKNotificationServiceStatus,
                                     ZOOMSDK::SDKNotificationServiceError) override
    {
    }
#endif

    std::atomic<bool> done{false};
    std::atomic<int> result{-1};
};

int main(int argc, char **argv)
{
    const char *jwt_env = std::getenv("COREVIDEO_PROBE_JWT");
    const char *public_key_env = std::getenv("COREVIDEO_PROBE_PUBLIC_APP_KEY");
    if ((!jwt_env || !*jwt_env) && (!public_key_env || !*public_key_env)) {
        std::cerr << "COREVIDEO_PROBE_JWT or COREVIDEO_PROBE_PUBLIC_APP_KEY is required"
                  << std::endl;
        return 2;
    }

    const bool customized_ui = argc > 1 && std::string(argv[1]) == "--custom-ui";

    ZOOMSDK::InitParam init_param;
#if defined(WIN32)
    init_param.strWebDomain = L"https://zoom.us";
#else
    init_param.strWebDomain = "https://zoom.us";
#endif
    init_param.enableGenerateDump = true;
    init_param.enableLogByDefault = true;
    init_param.rawdataOpts.videoRawdataMemoryMode =
        ZOOMSDK::ZoomSDKRawDataMemoryModeHeap;
    init_param.rawdataOpts.audioRawdataMemoryMode =
        ZOOMSDK::ZoomSDKRawDataMemoryModeHeap;
    if (customized_ui)
        init_param.obConfigOpts.optionalFeatures = ENABLE_CUSTOMIZED_UI_FLAG;

    std::cout << "INIT custom_ui=" << (customized_ui ? 1 : 0) << std::endl;
    ZOOMSDK::SDKError err = ZOOMSDK::InitSDK(init_param);
    std::cout << "InitSDK code=" << static_cast<int>(err) << std::endl;
    if (err != ZOOMSDK::SDKERR_SUCCESS)
        return 10 + static_cast<int>(err);

    ZOOMSDK::IAuthService *auth = nullptr;
    err = ZOOMSDK::CreateAuthService(&auth);
    std::cout << "CreateAuthService code=" << static_cast<int>(err)
              << std::endl;
    if (err != ZOOMSDK::SDKERR_SUCCESS || !auth) {
        ZOOMSDK::CleanUPSDK();
        return 20 + static_cast<int>(err);
    }

    AuthProbe probe;
    auth->SetEvent(&probe);
    auto jwt = to_zoom_string(jwt_env ? jwt_env : "");
    auto public_app_key =
        to_zoom_string(public_key_env ? public_key_env : "");
    ZOOMSDK::AuthContext ctx;
    if (public_key_env && *public_key_env) {
        ctx.publicAppKey = public_app_key.c_str();
        std::cout << "AUTH_MODE public_app_key" << std::endl;
    } else {
        ctx.jwt_token = jwt.c_str();
        std::cout << "AUTH_MODE jwt" << std::endl;
    }
    err = auth->SDKAuth(ctx);
    std::cout << "SDKAuth call code=" << static_cast<int>(err) << std::endl;
    if (err != ZOOMSDK::SDKERR_SUCCESS) {
        auth->SetEvent(nullptr);
        ZOOMSDK::DestroyAuthService(auth);
        ZOOMSDK::CleanUPSDK();
        return 30 + static_cast<int>(err);
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(20);
    while (!probe.done && std::chrono::steady_clock::now() < deadline) {
#if defined(_WIN32)
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    if (!probe.done)
        std::cout << "AUTH_CALLBACK timeout=1" << std::endl;

    auth->SetEvent(nullptr);
    ZOOMSDK::DestroyAuthService(auth);
    ZOOMSDK::CleanUPSDK();

    if (!probe.done)
        return 40;
    return probe.result == static_cast<int>(ZOOMSDK::AUTHRET_SUCCESS)
        ? 0
        : 100 + probe.result.load();
}
