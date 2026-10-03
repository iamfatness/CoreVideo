// EngineShare must survive the Zoom SDK calling back into it on the SAME
// thread that is calling into the SDK.
//
// Field crash, 2026-10-02 (end user, v0.1.48) and six local dumps from
// Aug 9-25: ZoomObsEngine died with an uncaught std::system_error
// (resource_deadlock_would_occur) thrown from EngineShare::
// onRawDataStatusChanged. EngineShare held its mutex while calling
// renderer->subscribe()/unSubscribe()/destroyRenderer(); the SDK delivered the
// status callback synchronously on that thread; the callback locked the same
// mutex; MSVC's std::mutex detected the relock and threw; nothing caught it,
// terminate() ran, and the whole Zoom session died while OBS stayed up. The
// share ran fine until the first re-subscribe during a live share (sharer
// switches content, share restarts, reconnect) -- "works for 30 minutes, then
// the SDK crashes".
//
// No SDK library is linked: createRenderer/destroyRenderer are defined here
// (SDK_API is empty without ZOOM_SDK_DLL_IMPORT), so the fake renderer can
// reproduce the synchronous callbacks the real SDK delivers.
#include "engine-share.h"
#include "engine-writer.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ZOOMSDK;

namespace {

int g_failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

struct FakeRenderer : IZoomSDKRenderer {
    explicit FakeRenderer(IZoomSDKRendererDelegate *d) : delegate(d) {}
    IZoomSDKRendererDelegate *delegate;
    uint32_t subscribed_id = 0;

    SDKError setRawDataResolution(ZoomSDKResolution) override { return SDKERR_SUCCESS; }
    // The real SDK can report the raw-data state change before subscribe()
    // returns, on the caller's thread.
    SDKError subscribe(uint32_t id, ZoomSDKRawDataType) override
    {
        subscribed_id = id;
        delegate->onRawDataStatusChanged(IZoomSDKRendererDelegate::RawData_On);
        return SDKERR_SUCCESS;
    }
    SDKError unSubscribe() override
    {
        subscribed_id = 0;
        delegate->onRawDataStatusChanged(IZoomSDKRendererDelegate::RawData_Off);
        return SDKERR_SUCCESS;
    }
    ZoomSDKResolution getResolution() override { return ZoomSDKResolution_1080P; }
    ZoomSDKRawDataType getRawDataType() override { return RAW_DATA_TYPE_SHARE; }
    uint32_t getSubscribeId() override { return subscribed_id; }
};

int g_created = 0;
int g_destroyed = 0;

ZoomSDKSharingSourceInfo share_info(unsigned int user,
                                    unsigned int source,
                                    SharingStatus status)
{
    ZoomSDKSharingSourceInfo info{};
    info.userid = user;
    info.shareSourceID = source;
    info.status = status;
    return info;
}

// Runs one step and turns an escaping exception into a test failure instead of
// terminate() -- before the fix, the first step throws on MSVC.
template <typename Fn>
void step(const char *what, Fn &&fn)
{
    try {
        fn();
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << what << " threw: " << e.what() << "\n";
        ++g_failures;
    } catch (...) {
        std::cerr << "FAIL: " << what << " threw a non-std exception\n";
        ++g_failures;
    }
}

size_t count_lines(const std::vector<std::string> &lines, const std::string &needle)
{
    size_t n = 0;
    for (const auto &l : lines)
        if (l.find(needle) != std::string::npos) ++n;
    return n;
}

} // namespace

BEGIN_ZOOM_SDK_NAMESPACE
extern "C" {
SDKError createRenderer(IZoomSDKRenderer **out, IZoomSDKRendererDelegate *delegate)
{
    *out = new FakeRenderer(delegate);
    ++g_created;
    return SDKERR_SUCCESS;
}
// The real SDK tells the delegate its renderer is gone; deliver that on the
// caller's thread too.
SDKError destroyRenderer(IZoomSDKRenderer *renderer)
{
    auto *fake = static_cast<FakeRenderer *>(renderer);
    fake->delegate->onRendererBeDestroyed();
    delete fake;
    ++g_destroyed;
    return SDKERR_SUCCESS;
}
}
END_ZOOM_SDK_NAMESPACE

int main()
{
    std::vector<std::string> lines;
    EngineIpc::test_sink() = [&lines](const std::string &l) { lines.push_back(l); };

    {
        EngineShare share;
        step("raw media on", [&] { share.set_raw_media_active(true); });
        step("register share source", [&] { share.subscribe("src_a", kIpcInvalidFd); });

        step("share begins (subscribe fires RawData_On re-entrantly)", [&] {
            share.onSharingStatus(share_info(42, 7, Sharing_Other_Share_Begin));
        });
        check(g_created == 1, "share begin creates one renderer");
        check(count_lines(lines, R"("stage":"share_raw_status")") >= 1,
              "the re-entrant RawData_On reached the source");

        step("sharer switches content (unSubscribe + destroy + subscribe re-entrantly)", [&] {
            share.onShareContentNotification(share_info(42, 8, Sharing_Other_Share_Begin));
        });
        check(g_created == 2 && g_destroyed == 1,
              "content switch replaces the renderer exactly once");

        step("same content again is a no-op", [&] {
            share.onShareContentNotification(share_info(42, 8, Sharing_Other_Share_Begin));
        });
        check(g_created == 2, "re-notifying the live source does not re-subscribe");

        step("share ends", [&] {
            share.onSharingStatus(share_info(42, 8, Sharing_Other_Share_End));
        });
        check(g_destroyed == 2, "share end releases the renderer");

        step("share begins again", [&] {
            share.onSharingStatus(share_info(43, 9, Sharing_Other_Share_Begin));
        });
        step("raw media off mid-share", [&] { share.set_raw_media_active(false); });
        check(g_destroyed == 3, "raw media off releases the live renderer");

        step("raw media back on; share restarts", [&] {
            share.set_raw_media_active(true);
            share.onSharingStatus(share_info(43, 9, Sharing_View_Other_Sharing));
        });
        step("last source unsubscribes mid-share", [&] { share.unsubscribe("src_a"); });
        check(g_destroyed == 4, "removing the last source releases the renderer");

        step("source re-registers, share live again", [&] {
            share.subscribe("src_b", kIpcInvalidFd);
            share.onSharingStatus(share_info(43, 9, Sharing_Other_Share_Begin));
        });
        step("unsubscribe_all mid-share", [&] { share.unsubscribe_all(); });
        step("detach", [&] { share.detach(); });
    }
    check(g_created == g_destroyed, "every renderer created was destroyed");
    // The guard below would absorb a relock, so "nothing threw" proves
    // nothing on its own: a callback the guard had to rescue lost its work.
    // None may have needed rescuing.
    check(count_lines(lines, "share_callback_exception") == 0,
          "no callback hit an exception (a relock would surface here)");

    // A callback must never let an exception escape into SDK frames: there it
    // is terminate(), i.e. the whole Zoom session. Make the IPC writer throw
    // from inside a renderer callback and require the engine to absorb it.
    {
        EngineShare share;
        share.set_raw_media_active(true);
        share.subscribe("src_c", kIpcInvalidFd);
        share.onSharingStatus(share_info(50, 11, Sharing_Other_Share_Begin));
        EngineIpc::test_sink() = [](const std::string &l) {
            if (l.find("share_raw_status") != std::string::npos)
                throw std::runtime_error("simulated failure inside a callback");
        };
        bool escaped = false;
        try {
            share.onRawDataStatusChanged(IZoomSDKRendererDelegate::RawData_Off);
        } catch (...) {
            escaped = true;
        }
        check(!escaped, "an exception inside onRawDataStatusChanged must not reach the SDK");
        EngineIpc::test_sink() = nullptr;
        share.detach();
    }

    if (g_failures) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "engine-share reentrancy: all checks passed\n";
    return 0;
}
