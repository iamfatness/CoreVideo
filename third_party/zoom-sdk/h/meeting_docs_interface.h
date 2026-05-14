
#ifndef _MEETING_DOCS_INTERFACE_H_
#define _MEETING_DOCS_INTERFACE_H_
#include "zoom_sdk_def.h"

BEGIN_ZOOM_SDK_NAMESPACE

/**
 * @brief Enumeration of docs share status.
 */
typedef enum
{
	SDKDocsStatus_None = 0,
	/**User starts sharing docs. */
	SDKDocsStatus_Start,
	/**User stops sharing docs. */
	SDKDocsStatus_Stop
}SDKDocsStatus;

/**
 * @brief Enumeration of docs share option.
 */
typedef enum
{
	/**A wrong option, such as the meeting not supporting docs. */
	SDKDocsShareOption_None = 0,
	/**Only host can share docs. */
	SDKDocsShareOption_HostShare,
	/**Anyone can share docs, but only one doc can be shared at a time, and only host can take over another's sharing. */
	SDKDocsShareOption_HostGrabShare,
	/**Anyone can share docs, but only one doc can be shared at a time, and anyone can take over another's sharing. */
	SDKDocsShareOption_AllGrabShare
}SDKDocsShareOption;

/**
 * @brief Enumeration of docs create option.
 */
typedef enum
{
	/**A wrong option, maybe the meeting does not support docs. */
	SDKDocsCreateOption_None = 0,
	/**Only the host can initiate new docs. */
	SDKDocsCreateOption_HostOnly,
	/**Users under the same account can initiate new docs. */
	SDKDocsCreateOption_AccountUsers,
	/**All participants can initiate new docs. */
	SDKDocsCreateOption_All
}SDKDocsCreateOption;

/**
 * @brief Share source info of Zoom docs.
 */
class IZoomSDKDocSharingSourceInfo
{
public:
	/**
	 * @brief Gets the status of Zoom docs sharing
	 * @return Value defined in SDKDocsStatus enum.
	 */
	virtual SDKDocsStatus GetStatus() = 0;

	/**
	 * @brief Gets the title of the sharing Zoom docs.
	 * @return The title of the sharing Zoom docs.
	 */
	virtual const zchar_t* GetDocTitle() = 0;

	/**
	 * @brief Gets the share source id of the sharing Zoom docs
	 * @return The share source id of the sharing Zoom docs.
	 */
	virtual unsigned int GetShareSourceID() = 0;

	/**
	 * @brief Gets the user id who is sharing Zoom docs.
	 * @param The user ID who is sharing Zoom docs.
	 */
	virtual unsigned int GetUserID() = 0;

	virtual ~IZoomSDKDocSharingSourceInfo() {};
};

/**
 * @brief Docs controller callback event.
 */
class IMeetingDocsCtrlEvent
{
public:
	virtual ~IMeetingDocsCtrlEvent() {}

	/**
	 * @brief Callback for document status change
	 * @param pInfo Document sharing source information.
	 * @note Notifies when document sharing starts/stops. All meeting participants receive this event.
	 */
	virtual void onDocsSharingSourceInfoChanged(IZoomSDKDocSharingSourceInfo* pInfo) = 0;

	/**
	 * @brief Callback for document sharing permission changes
	 * @param createOption Specifies who can create new documents.
	 * @param option Specifies who can share documents.
	 * @note Triggered when document creation/sharing permissions are modified.
	 */
	virtual void onDocsPermissionChanged(SDKDocsCreateOption createOption, SDKDocsShareOption option) = 0;
	
};

/**
 * @brief Meeting Docs helper interface
 */
class IMeetingDocsController
{
public:
	virtual ~IMeetingDocsController() {}

	/**
	 * @brief Configure the meeting docs controller callback event handler.
	 * @param pEvent An object pointer to the IMeetingDocsCtrlEvent that receives the meeting docs callback event. 
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 * @note remarks The SDK use pEvent to transmit the callback event to the user's application. If the function is not called or fails, the user's application is unable to retrieve the callback event.
	 */
	virtual SDKError SetEvent(IMeetingDocsCtrlEvent* pEvent) = 0;

	/**
	 * @brief Determines whether the current meeting supports the docs or not.
	 * @return true indicates to support.
	 */
	virtual bool IsSupportDocs() = 0;

	/**
	 * @brief Determines whether the current user can start sharinge the doc or not.
	 * @param [out] reason The reason why no one can start sharing the doc.
	 * @return true indicates you can start sharing the doc.
	 */
	virtual bool CanStartShareDocs(CannotShareReasonType& reason) = 0;

	/**
	 * @brief Determines whether the current user can set docs options or not.
	 * @return true if the current user can set docs options. Otherwise, false.
	 */
	virtual bool CanSetDocsOption() = 0;

	/**
	 * @brief Sets the setting option for who can share docs.
	 * @param option The setting for who can share docs.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError SetDocsShareOption(SDKDocsShareOption option) = 0;

	/**
	 * @brief Gets the setting option for who can share docs.
	 * @param [out] option Setting option for who can share docs.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError GetDocsShareOption(SDKDocsShareOption& option) = 0;

	/**
	 * @brief Sets the option for who can initiate a new doc.
	 * @param option Setting option for who can initiate a new doc.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError SetDocsCreateOption(SDKDocsCreateOption option) = 0;

	/**
	 * @brief Gets the option for who can initiate a new doc.
	 * @param param [out] option Setting option for who can initiate a new doc.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError GetDocsCreateOption(SDKDocsCreateOption& option) = 0;

	/**
	 * @brief Query if other user is sharing docs.
	 * @return true indicates other user is sharing docs.
	 */
	virtual bool IsOtherSharingDocs() = 0;

	/**
	 * @brief Query if the current user is sharing docs successfully.
	 * @return true indicates sharing successfully.
	 */
	virtual bool IsSharingDocsOut() = 0;

	/**
	 * @brief Sets the docs web view and dashboard web view owner window.
	 * @param hOwner Specify the owner window. 
	 * @note Call this function before calling ShowDocsDashboard. Call this function when receiving the meeting status MEETING_STATUS_INMEETING. 
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 * @note Valid only for user custom interface mode.
	 */
	virtual SDKError  SetOwnerWindow(HWND hOwner) = 0;

	/**
	 * @brief Show the docs dashboard web view window.
	 * @param x: The horizontal coordinate value. 
	 * @param y: The vertical coordinate value. 
	 * @note If x == -1 and y = -1, the window position will be in the center of the screen where the owner window is located.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 * @note Valid only for custom user interface mode.
	 */
	virtual SDKError ShowDocsDashboard(int x = -1, int y = -1) = 0;

	/**
	 * @brief Sets the dashboard web view window size.
	 * @param height Specify the height of the window. 
	 * @param width Specify the width of the window. 
	 * @note If height == -1 and width == -1, the window size will be default.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 * @note Valid only for custom user interface mode.
	 */
	virtual SDKError SetDocDashboardSize(int width = -1, int  height = -1) = 0;

	/**
	 * @brief Sets the docs web view window position.
	 * @param x Specifies the X-axis coordinate of the top-left corner of the docs web view window in the parent window.
	 * @param y Specifies the Y-axis coordinate of the top-left corner of the docs web view window in the parent window.
	 * @note If x == -1 and y = -1, the window position will be in the center of the screen where the owner window is located.
	 * @note If you don't call this function, the window position will be in the center of the screen where the owner window is located.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 * @note Valid only for custom user interface mode.
	 */
	virtual SDKError SetDocsViewPos(int x = -1, int y = -1) = 0;

	/**
	 * @brief Sets the docs web view window size.
	 * @param height Specify the height of the docs web view window. 
	 * @param width Specify the width of the docs web view window. 
	 * @note If height == -1 and width == -1, the window size will be default.
	 * @note If you don't call this function, the window size will be default.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 * @note Valid only for custom user interface mode.
	 */
	virtual SDKError SetDocsViewSize(int width = -1, int  height = -1) = 0;

	/**
	 * @brief Show the docs web view window.
	 * @param sourceID The share source ID that is sharing.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 * @note Valid only for custom user interface mode.
	 */
	virtual SDKError ShowDocsViewWindow(unsigned int sourceID) = 0;

	/**
	 * @brief Close the docs web view window.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 * @note Valid only for custom user interface mode.
	 */
	virtual SDKError CloseDocsViewWindow() = 0;

	/**
	 * @brief Gets the docs sharing source information list from the specified sharer.
	 * @param userID The ID of the user who is sharing docs.
	 * @return If the function succeeds, it returns the viewable docs sharing information list. Otherwise, this function fails and returns nullptr.
	 */
	virtual IList<IZoomSDKDocSharingSourceInfo* >* GetDocsSharingSourceInfoList(unsigned int userID) = 0;

	/**
	 * @brief Gets the ID of users who are sharing docs.
	 * @return If the function succeeds, the return value is a list of user ID of all users who are sharing docs. If the function fails, the return value is nullptr.
	 */
	virtual IList<unsigned int>* GetViewableDocsSharingUserList() = 0;
};

END_ZOOM_SDK_NAMESPACE
#endif