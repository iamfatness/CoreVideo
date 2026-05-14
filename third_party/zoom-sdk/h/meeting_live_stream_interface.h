/**
 * @file meeting_live_stream_interface.h
 * @brief Meeting Service Live Streaming Interface.
 */
#ifndef _MEETING_LIVE_STREAM_INTERFACE_H_
#define _MEETING_LIVE_STREAM_INTERFACE_H_
#include "zoom_sdk_def.h"

BEGIN_ZOOM_SDK_NAMESPACE

/** 
 * @brief Enumeration of live stream status.
 * Here are more detailed structural descriptions.
 */
enum LiveStreamStatus
{
	/** Only for initialization. */
	LiveStreamStatus_None,
	/** In progress. */
	LiveStreamStatus_InProgress,
	/** Be connecting. */
	LiveStreamStatus_Connecting,
	/** Connect timeout. */
	LiveStreamStatus_Start_Failed_Timeout,
	/** Failed to start live streaming. */
	LiveStreamStatus_Start_Failed,
	/** Live stream ends. */
	LiveStreamStatus_Ended,
};

/**
 * @brief information of raw live stream info.
 * Here are more detailed structural descriptions..
 */
struct RawLiveStreamInfo
{
	/** User ID. */
	unsigned int userId;
	/** Broadcast URL */
	const zchar_t* broadcastUrl;
	/** Broadcast Name */
	const zchar_t* broadcastName;
	RawLiveStreamInfo()
	{
		userId = 0;
		broadcastUrl = nullptr;
		broadcastName = nullptr;
	}
};

/**
 * @class IRequestRawLiveStreamPrivilegeHandler
 * @brief Process after the host receives the requirement from the user to give the raw live stream privilege.
 */
class IRequestRawLiveStreamPrivilegeHandler
{
public:
	virtual ~IRequestRawLiveStreamPrivilegeHandler() {};
	
	/**
	 * @brief Gets the request ID.
	 * @return If the function succeeds, it returns the request ID. Otherwise, this function fails and returns nullptr.
	 */
	virtual const zchar_t* GetRequestId() = 0;
	
	/**
	 * @brief Gets the user ID who requested privilege.
	 * @return If the function succeeds, it returns the user ID. Otherwise, this function returns 0.
	 */
	virtual unsigned int GetRequesterId() = 0;
	
	/**
	 * @brief Gets the user name who requested privileges.
	 * @return If the function succeeds, it returns the user name. Otherwise, this function fails and returns nullptr.
	 */
	virtual const zchar_t* GetRequesterName() = 0;
	
	/**
	 * @brief Gets the broadcast URL.
	 * @return If the function succeeds, it returns the broadcast URL. Otherwise, this function fails and returns nullptr.
	 */
	virtual const zchar_t* GetBroadcastUrl() = 0;
	
	/**
	 * @brief Gets the broadcast name.
	 * @return If the function succeeds, it returns the broadcast name. Otherwise, this function fails and returns nullptr.
	 */
	virtual const zchar_t* GetBroadcastName() = 0;
	
	/**
	 * @brief Grants the user permission to start raw live stream and then destroys this instance.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError GrantRawLiveStreamPrivilege() = 0;
	
	/**
	 * @brief Denies the user permission to start raw live stream and then destroys this instance.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError DenyRawLiveStreamPrivilege() = 0;
};

/**
 * @class IMeetingLiveStreamCtrlEvent
 * @brief Live stream meeting controller callback event.
 */
class IMeetingLiveStreamCtrlEvent
{
public:
	/**
	 * @brief Callback event when live stream status changes.
	 * @param status Live stream status.
	 */
	virtual void onLiveStreamStatusChange(LiveStreamStatus status) = 0;
	
	/**
	 * @brief Callback event when the current user's raw live streaming privilege changes.
	 * @param bHasPrivilege Specify whether or not the user has privileg.
	 */
	virtual void onRawLiveStreamPrivilegeChanged(bool bHasPrivilege) = 0;
	
	/**
	 * @brief Callback event when the current user's request has time out.
	 */
	virtual void onRawLiveStreamPrivilegeRequestTimeout() = 0;
	
	/**
	 * @brief Callback event when another user's raw live streaming privilege has changed.
	 * @param userid The ID of the user whose privilege has changed.
	 * @param bHasPrivilege Specify whether the user has privilege or not.
	 */
	virtual void onUserRawLiveStreamPrivilegeChanged(unsigned int userid, bool bHasPrivilege) = 0;
	
	/**
	 * @brief Callback event when a user requests raw live streaming privilege.
	 * @param handler A pointer to the IRequestRawLiveStreamPrivilegeHandler.
	 */
	virtual void onRawLiveStreamPrivilegeRequested(IRequestRawLiveStreamPrivilegeHandler* handler) = 0;
	
	/**
	 * @brief Callback event when users start/stop raw live streaming.
	 * @param liveStreamList A list of users with an active raw live stream.
	 */
	virtual void onUserRawLiveStreamingStatusChanged(IList<RawLiveStreamInfo>* liveStreamList) = 0;
	
	/**
	 * @brief Callback event when the live stream reminder enable status changes.
	 * @param enable true indicates the live stream reminder is enabled.
	 */
	virtual void onLiveStreamReminderStatusChanged(bool enable) = 0;
	
	/**
	 * @brief Callback event when the live stream reminder enable status change fails.
	 */
	virtual void onLiveStreamReminderStatusChangeFailed() = 0;
	
	/**
	 * @brief Callback event when the meeting or webinar user has nearly reached the meeting capacity, like 80% or 100% for the meeting or webinar capacity. 
	 * The host can start live stream to let unjoined user watch live stream.
	 * @param percent Proportion of joined users to the total capacity.
	 */
	virtual void onUserThresholdReachedForLiveStream(int percent) = 0;
};

/**
 * @class IMeetingLiveStreamItem
 * @brief Live stream of current meeting.
 */
class IMeetingLiveStreamItem
{
public:
	/**
	 * @brief Gets the URL of the live stream meeting.
	 * @return If the function succeeds, it returns the URL of the live stream meeting. Otherwise, this function fails and returns nullptr.
	 */
	virtual const zchar_t* GetLiveStreamURL() = 0;
	
	/**
	 * @brief Gets the description of live stream.
	 * @return If the function succeeds, it returns the description of live stream. Otherwise, this function fails and returns nullptr.
	 */
	virtual const zchar_t* GetLiveStreamURLDescription() = 0;
	
	/**
	 * @brief Gets the viewer URL of the live stream meeting.
	 * @return If the function succeeds, it returns the viewer URL of the live stream meeting. Otherwise, this function fails and returns nullptr.
	 */
	virtual const zchar_t* GetLiveStreamViewerURL() = 0;
	
	virtual ~IMeetingLiveStreamItem() {};
};

/**
 * @class IMeetingLiveStreamController
 * @brief Live stream meeting controller interface.
 */
class IMeetingLiveStreamController
{
public:
	/**
	 * @brief Sets live stream meeting callback event handler
	 * @param pEvent A pointer to the IMeetingLiveStreamCtrlEvent that receives live stream event. 
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError SetEvent(IMeetingLiveStreamCtrlEvent* pEvent) = 0;
	
	/**
	 * @brief Determines if it is able to start live streaming.
	 * @return If it is enabled to start the live streaming, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError CanStartLiveStream() = 0;
	
	/**
	 * @brief Starts live streaming.
	 * @param item_ A pointer to the IMeetingLiveStreamItem created via IMeetingLiveStreamController::GetSupportLiveStreamURL() API. 
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError StartLiveStream(IMeetingLiveStreamItem* item_) = 0;
	
	/**
	 * @brief Starts live streaming.
	 * @param streamingURL The URL of live streaming.
	 * @param streamingKey The key of live streaming. 
	 * @param broadcastURL The broadcast URL of live-stream.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 * @note Get the parameters from the third party of live stream service
	 */
	virtual SDKError StartLiveStreamWithSteamingURL(const zchar_t* streamingURL, const zchar_t* streamingKey, const zchar_t* broadcastURL) = 0;
	
	/**
	 * @brief Stops live streaming.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError StopLiveStream() = 0;
	
	/**
	 * @brief Gets the list of URL and associated information used by live streaming in the current meeting. 
	 * @return If the function succeeds, the return value is the meeting information to be live streamed. Otherwise, the return value is nullptr.
	 * @deprecated Use \link GetSupportLiveStreamItems \endlink instead.
	 */
	virtual IList<IMeetingLiveStreamItem* >* GetSupportLiveStreamURL() = 0;
	
	/**
	 * @brief Gets the list of live stream information items in the current meeting.
	 * @return If the function succeeds, the return value is the live stream item list. Otherwise, the return value is nullptr.
	 */
	virtual IList<IMeetingLiveStreamItem* >* GetSupportLiveStreamItems() = 0;
	
	/**
	 * @brief Gets the current live stream object. 
	 * @return If the function succeeds, the return value is the current live stream object. Otherwise, the return value is nullptr.
	 */
	virtual IMeetingLiveStreamItem* GetCurrentLiveStreamItem() = 0;
	
	/**
	 * @brief Gets live stream status of current meeting.
	 * @return If the function succeeds, the return value is the live stream status of current meeting.
	 */
	virtual LiveStreamStatus GetCurrentLiveStreamStatus() = 0;
	
	/**
	 * @brief Query Whether the meeting supports raw live streams.
	 * @return true if supported. Otherwise, false.
	 */
	virtual bool IsRawLiveStreamSupported() = 0;
	
	/**
	 * @brief Whether if the current user is able to start raw live streaming.
	 * @return If the current user is able to start raw live streaming, the return value is SDKERR_SUCCESS. Otherwise it fails,and returns nullptr.
	 */
	virtual SDKError CanStartRawLiveStream() = 0;
	
	/**
	 * @brief Sends a request to enable the SDK to start a raw live stream.
	 * @param broadcastURL The broadcast URL of the live-stream.
	 * @param broadcastName The broadcast name of the live-stream.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS and the SDK will send the request. Otherwise it fails and the request will not be sent.
	 */
	virtual SDKError RequestRawLiveStreaming(const zchar_t* broadcastURL, const zchar_t* broadcastName) = 0;
	
	/**
	 * @brief Starts raw live streaming.
	 * @param broadcastURL The broadcast URL of the live-stream.
	 * @param broadcastName The broadcast name of the live-stream.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError StartRawLiveStreaming(const zchar_t* broadcastURL, const zchar_t* broadcastName) = 0;
	
	/**
	 * @brief Stops raw live streaming.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError StopRawLiveStream() = 0;
	
	/**
	 * @brief Remove the raw live stream privilege.
	 * @param userID Specify the ID of the user whose privilege will be removed.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise it fails.
	 */
	virtual SDKError RemoveRawLiveStreamPrivilege(unsigned int userid) = 0;
	
	/**
	 * @brief Gets a list of current active raw live streams.
	 * @return If the function succeeds, it returns a pointer to the IList object. Otherwise, this function fails and returns nullptr.
	 */
	virtual IList<RawLiveStreamInfo>* GetRawLiveStreamingInfoList() = 0;
	
	/**
	 * @brief Gets the list of users'IDs who have raw live stream privileges.
	 * @return If the function succeeds, it returns a pointer to the IList object. Otherwise, this function fails and returns nullptr.
	 */
	virtual IList<unsigned int>* GetRawLiveStreamPrivilegeUserList() = 0;
	
	/**
	 * @brief Checks if the live stream reminder is enabled.
	 * @return true indicates the live stream reminder is enabled.
	 * @note When the live stream reminder is enabled, the new join user is notified that the meeting is at capacity but that they can watch the meeting live stream with the callback IMeetingServiceEvent::onMeetingFullToWatchLiveStream when the meeting user has reached the meeting capability.
	 */
	virtual bool IsLiveStreamReminderEnabled() = 0;
	
	/**
	 * @brief Checks if the current user can enable/disable the live stream reminder.
	 * @return true indicates the current user can enable/disable the live stream reminder.
	 */
	virtual bool CanEnableLiveStreamReminder() = 0;
	
	/**
	 * @brief Enables or disable the live stream reminder.
	 * @param enable true indicates enable the live stream reminder. false means disable the live stream reminder.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise it fails.
	 */
	virtual SDKError EnableLiveStreamReminder(bool enable) = 0;
};
END_ZOOM_SDK_NAMESPACE
#endif