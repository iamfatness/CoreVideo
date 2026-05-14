/**
 * @file direct_share_helper_interface.h
 * @brief Direct Sharing Service Interface.
 */
#ifndef _DIRECT_SHARE_SERVICE_HELPER_INTERFACE_H_
#define _DIRECT_SHARE_SERVICE_HELPER_INTERFACE_H_
#include "zoom_sdk_def.h"
BEGIN_ZOOM_SDK_NAMESPACE

/**
 * @brief Enumeration of direct sharing status.
 * Here are more detailed structural descriptions.
 */
typedef enum DirectShareStatusStatus_Enum
{
	/** Only for initialization. */
	DirectShare_Unknown = 0,
	/** Waiting for enabling the direct sharing. */
	DirectShare_Connecting,
	/** In direct sharing mode. */
	DirectShare_In_Direct_Share_Mode,
	/** End the direct sharing. */
	DirectShare_Ended,
	/** Re-enter the meeting ID/paring code. */
	DirectShare_Need_MeetingID_Or_PairingCode,
	/** Network error. Please try again later. */
	DirectShare_NetWork_Error,
	/** Other errors. Mainly occur in SIP call mode. */
	DirectShare_Other_Error,
	/** Wrong meeting id or sharing key. */
	DirectShare_WrongMeetingID_Or_SharingKey,
	/** Please input new paring code. */
	DirectShare_InputNewParingCode,
	/** Prepare to share data */
	DirectShare_Prepared, 
}DirectShareStatus;

class IDirectShareSpecifyContentHandler
{
public:
	virtual ~IDirectShareSpecifyContentHandler(){};

	virtual IList<ShareType>* GetSupportedDirectShareType() = 0;

	virtual SDKError TryShareApplication(HWND hWnd, bool bShareSound = true, bool bOptimizeVideoClip = true) = 0;

	virtual SDKError TryShareDesktop(const zchar_t* monitorId, bool bShareSound = true, bool bOptimizeVideoClip = true) = 0;

	virtual SDKError Cancel() = 0;
};
/**
 * @class IDirectShareViaMeetingIDOrPairingCodeHandler
 * @brief Direct sharing by meeting ID or pairing code helper interface.
 */
class IDirectShareViaMeetingIDOrPairingCodeHandler
{
public:
	virtual ~IDirectShareViaMeetingIDOrPairingCodeHandler(){};
	
	/**
	 * @brief Try to match with the specified meeting number.
	 * @param meetingNumber Specifies the meeting number.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError TryWithMeetingNumber(UINT64 meetingNumber) = 0;
	
	/**
	 * @brief Try to match with the pairing code.
	 * @param pairingCode Specifies the pairing code.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError TryWithPairingCode(const zchar_t* pairingCode) = 0;
	
	/**
	 * @brief Delete the present direct sharing.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError Cancel() = 0;
};
/**
 * @class IDirectShareServiceHelperEvent
 * @brief Direct sharing helper callback event.
 */
class IDirectShareServiceHelperEvent
{
public:
	virtual ~IDirectShareServiceHelperEvent() {}
	
	/**
	 * @brief The callback event will be triggered if the status of direct sharing changes.
	 * @param status Specifies the status of direct sharing.
	 * @param handler A pointer to the IDirectShareViaMeetingIDOrPairingCodeHandler. It is only valid when the value of status is DirectShare_Need_MeetingID_Or_PairingCode.
	 * @note The SDK user must set the value of the _paring_code or _meeting_number via the functions of IDirectShareViaMeetingIDOrPairingCodeHandler to  start direct sharing.
	 */
	virtual void OnDirectShareStatusUpdate(DirectShareStatus status, IDirectShareViaMeetingIDOrPairingCodeHandler* handler) = 0;
	
	/**
	 * @note Only used for customui mode
	 */
	virtual void OnDirectShareSpecifyContent(IDirectShareSpecifyContentHandler* handler) = 0;
};
/**
 * @class IDirectShareServiceHelper
 * @brief Direct sharing helper Interface.
 */
class IDirectShareServiceHelper
{
public:
	/**
	 * @brief Direct sharing helper callback event handler.
	 * @param pEvent A pointer to the IDirectShareServiceHelperEvent that receives the direct sharing service event. 
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError SetEvent(IDirectShareServiceHelperEvent* pEvent) = 0;
	
	/**
	 * @brief Determines if it is able to start the direct sharing.
	 * @return If it is enabled to start the direct sharing, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError CanStartDirectShare()= 0;
	
	/**
	 * @brief Determines if direct sharing is in progress.
	 * @return true indicates that the direct sharing is in progress.
	 */
	virtual bool	 IsDirectShareInProgress() = 0;
	
	/**
	 * @brief Starts direct sharing.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 * @note This API can only be called by the logged in user.
	 */
	virtual SDKError StartDirectShare() = 0;
	
	/**
	 * @brief Stops direct sharing.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 * @note This API can only be called by the logged in user.
	 */
	virtual SDKError StopDirectShare() = 0;
};

END_ZOOM_SDK_NAMESPACE
#endif