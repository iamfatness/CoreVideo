#pragma once
#include "zoom_sdk_def.h"

namespace ZOOMSDK {

enum VideoStatus { Video_ON = 0, Video_OFF = 1, Video_Failed = 2 };
enum VideoConnectionQuality { VideoConnectionQuality_Unknown = 0, VideoConnectionQuality_Bad = 1, VideoConnectionQuality_Normal = 2, VideoConnectionQuality_Good = 3 };
enum CameraControlRequestType { CameraControlRequestType_RequestControl = 0, CameraControlRequestType_GiveUpControl = 1 };
enum CameraControlRequestResult { CameraControlRequestResult_Approve = 0, CameraControlRequestResult_Decline = 1 };

class IRequestStartVideoHandler {
public:
    virtual ~IRequestStartVideoHandler() {}
};

class ICameraControlRequestHandler {
public:
    virtual ~ICameraControlRequestHandler() {}
};

class IMeetingVideoCtrlEvent {
public:
    virtual void onUserVideoStatusChange(unsigned int userId, VideoStatus status) = 0;
    virtual void onActiveSpeakerVideoUserChanged(unsigned int userId) = 0;
    virtual void onActiveVideoUserChanged(unsigned int userId) = 0;
    virtual void onSpotlightedUserListChangeNotification(IList<unsigned int> *list) = 0;
    virtual void onHostRequestStartVideo(IRequestStartVideoHandler *handler) = 0;
    virtual void onUserVideoQualityChanged(VideoConnectionQuality quality, unsigned int userId) = 0;
    virtual void onHostVideoOrderUpdated(IList<unsigned int> *list) = 0;
    virtual void onLocalVideoOrderUpdated(IList<unsigned int> *list) = 0;
    virtual void onFollowHostVideoOrderChanged(bool follow) = 0;
    virtual void onVideoAlphaChannelStatusChanged(bool enabled) = 0;
    virtual void onCameraControlRequestReceived(unsigned int userId, CameraControlRequestType type, ICameraControlRequestHandler *handler) = 0;
    virtual void onCameraControlRequestResult(unsigned int userId, CameraControlRequestResult result) = 0;
    virtual ~IMeetingVideoCtrlEvent() {}
};

class IMeetingVideoController {
public:
    virtual SDKError SetEvent(IMeetingVideoCtrlEvent *event) = 0;
    virtual ~IMeetingVideoController() {}
};

} // namespace ZOOMSDK
