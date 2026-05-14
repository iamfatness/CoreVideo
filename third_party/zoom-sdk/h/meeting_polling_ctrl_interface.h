/**
 * @file meeting_polling_ctrl_interface.h
 * @brief Meeting Service Polling Interface.
 */
#ifndef _MEETING_POLLING_CTRL_INTERFACE_H_
#define _MEETING_POLLING_CTRL_INTERFACE_H_
#include "zoom_sdk_def.h"

BEGIN_ZOOM_SDK_NAMESPACE

/**
 * @brief Enumeration of polling status.
 */
typedef enum
{
	/** The initial status */
	SDKPollingStatus_Initial, 
	/**  User started polling. */
	SDKPollingStatus_Started,    
	/**  User shared polling result. */
	SDKPollingStatus_ShareResult, 
	/**  User stopped polling. */
	SDKPollingStatus_Stopped,     
}SDKPollingStatus;

/**
 * @brief Enumeration of polling type.
 */
typedef enum
{
	SDKPollingType_Unknown,
	SDKPollingType_Poll,
	SDKPollingType_Quiz
}SDKPollingType;

/**
 * @brief Enumeration of polling question type.
 */
typedef enum
{
	SDKPollingQuestionType_Unknown,
	SDKPollingQuestionType_Single,
	SDKPollingQuestionType_Multi,
	SDKPollingQuestionType_Matching,
	SDKPollingQuestionType_RankOrder,
	SDKPollingQuestionType_ShortAnswer,
	SDKPollingQuestionType_LongAnswer,
	SDKPollingQuestionType_FillBlank,
	SDKPollingQuestionType_RatingScale,
	SDKPollingQuestionType_Dropdown,
}SDKPollingQuestionType;


/**
 * @brief Enumeration of polling action type.
 */
typedef enum
{
	SDKPollingActionType_Unknown,
	SDKPollingActionType_Start,
	SDKPollingActionType_Stop,
	SDKPollingActionType_ShareResult,
	SDKPollingActionType_StopShareResult,
	SDKPollingActionType_Duplicate,
	SDKPollingActionType_Delete,
	SDKPollingActionType_Submit,
	SDKPollingActionType_Error
}SDKPollingActionType;

/**
 * @brief Polling item interface.
 */
class IPollingItem
{
public:
	virtual ~IPollingItem() {}
	virtual const zchar_t* GetPollingID() = 0;
	virtual const zchar_t* GetPollingName() = 0;
	virtual SDKPollingType GetPollingType() = 0;
	virtual SDKPollingStatus GetPollingStatus() = 0;
	virtual unsigned int GetPollingQuestionCount() = 0;
	virtual unsigned int GetTotalVotedUserCount() = 0;
	virtual bool IsLibraryPolling() = 0;
};

/**
 * @brief Polling answer item interface.
 */
class IPollingAnswerItem
{
public:
	virtual ~IPollingAnswerItem() {}
	virtual const zchar_t* GetPollingID() = 0;
	virtual const zchar_t* GetPollingQuestionID() = 0;
	virtual const zchar_t* GetPollingSubQuestionID() = 0;
	virtual const zchar_t* GetPollingAnswerID() = 0;
	virtual const zchar_t* GetPollingAnswerName() = 0;
	virtual const zchar_t* GetPollingAnsweredContent() = 0;
	virtual bool IsChecked() = 0; /** This property has no meaning for the correct answer. */
};

/**
 * @brief Polling question item interface.
 */
class IPollingQuestionItem
{
public:
	virtual ~IPollingQuestionItem() {}
	virtual const zchar_t* GetPollingID() = 0;
	virtual const zchar_t* GetPollingQuestionID() = 0;
	virtual const zchar_t* GetPollingQuestionName() = 0;
	virtual SDKPollingQuestionType GetPollingQuestionType() = 0;
	virtual unsigned int GetAnsweredCount() = 0;
	virtual bool IsRequired() = 0;
	virtual IList<IPollingQuestionItem*>* GetPollingSubQuestionItemList() = 0; /** Only matching, fill blank or rank order question has subquestion */
	virtual IList<IPollingAnswerItem*>* GetPollingAnswerItemList() = 0;
};

/**
 * @brief Polling answer result item interface.
 */
class IPollingAnswerResultItem
{
public:
	virtual ~IPollingAnswerResultItem() {}
	virtual const zchar_t* GetPollingID() = 0;
	virtual const zchar_t* GetPollingQuestionID() = 0;
	virtual const zchar_t* GetPollingSubQuestionID() = 0;
	virtual const zchar_t* GetPollingAnswerID() = 0;
	virtual const zchar_t* GetPollingAnswerName() = 0;
	virtual unsigned int GetSelectedCount() = 0;  /** How many participants selected this answer. */
};

/**
 * @class IMeetingPollingCtrlEvent
 * @brief Polling controller callback event.
 */
class IMeetingPollingCtrlEvent
{
public:
	virtual ~IMeetingPollingCtrlEvent() {}
	/**
	 * @brief Polling status changed callback. Use this function to inform the user that the polling has been started, share result or stopped.
	 * @param pollingID Specify the status changed poll's ID.
	 * @param status Specify current polling status.
	 */
	virtual void onPollingStatusChanged(const zchar_t* pollingID, SDKPollingStatus status) = 0;
	/**
	 * @brief Polling result updated callback. This is triggered when a participant submits polling.
	 * @param pollingID Specify the result updated poll's ID.
	 */
	virtual void onPollingResultUpdated(const zchar_t* pollingID) = 0;
	/**
	 * @brief Polling list updated callback. This is triggered when a host adds, edits, duplicates or deletes a polling.
	 */
	virtual void onPollingListUpdated() = 0;
	/**
	 * @brief Polling action result callback. This is triggered when a user perform an action for a poll.
	 * @param actionType Specify the action type.
	 * @param pollingID Specify the action poll's ID.
	 * @param bSuccess Specify whether the action succeeds.
	 * @param errorMsg Specify the error message when the action fails. It is only for  SDKPollingActionType_Error.
	 * @note If actionType is SDKPollingActionType_Error, use errorMsg. This errorMsg may be triggered by any action. 
	 */
	virtual void onPollingActionResult(SDKPollingActionType actionType, const zchar_t* pollingID, bool bSuccess, const zchar_t* errorMsg) = 0;
	/**
	 * @brief Polling question item image downloaded callback.
	 * This is triggered when a user starts or joins a meeting, if any polling question has image, or host uploads an image to a polling question in the meeting.
	 * @param questionID Specify the polling question's ID.
	 * @param path Specify the image downloaded path. 
	 */
	virtual void onPollingQuestionImageDownloaded(const zchar_t* questionID, const zchar_t* path) = 0;
	/**
	 * @brief Polling elapsed time callback.
	 * This is triggered when a host starts a poll or a user changes to host from non host.
	 * @param uElapsedtime Specify the polling elapsed time in milliseconds.
	 */

	virtual void onPollingElapsedTime(const zchar_t* pollingID, unsigned int uElapsedtime) = 0;
	/**
	 * @brief Callback event for the user get right answer list privilege changed. This is triggered when the host calls EnableGetRightAnswerList in a quiz.
	 * @param bCan Specify whether the user has get right answer list privilege or not.
	 */
	virtual void onGetRightAnswerListPrivilege(bool bCan) = 0;
	/**
	 * @brief polling inactive callback. When the user starts a meeting in another device by same account, this callback is triggered. Then call CanDoPolling return false.
	 */
	virtual void onPollingInactive() = 0;
};
/**
 * @brief Polling controller interface.
 */

class IMeetingPollingController
{
public:
	/**
	 * @brief Configure the meeting polling controller callback event handler.
	 * @param pEvent An object pointer to the IMeetingPollingCtrlEvent that receives the meeting polling callback event.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 * @note The SDK use pEvent to transmit the callback event to the user's application. If the function is not called or fails, the user's application is unable to retrieve the callback event.
	 */
	virtual SDKError SetEvent(IMeetingPollingCtrlEvent* pEvent) = 0;

	/**
	 * @note Common (for all) 
	 */
	 
	/**
	 * @brief Determines whether the current meeting can do the polling.
	 * @return true indicates you can do polling.
	 */
	virtual bool CanDoPolling() = 0;
	/**
	 * @brief Gets the active poll's ID.
	 * @return The shared result or started poll's ID.
	 */
	virtual const zchar_t* GetActivePollingID() = 0;
	/**
	 * @brief Gets the list of poll's question.
	 * @param pollingID Specify the question's polling ID. 
	 * @return If the function succeeds, the return value is a pointer to the IList<IPollingQuestionItem*>. Otherwise, the return value is nullptr.
	 */
	virtual IList<IPollingQuestionItem*>* GetPollingQuestionItemList(const zchar_t* pollingID) = 0;
	/**
	 * @brief Determines if the right answer item list can be allowed to get.
	 * @param pollingID Specify the right answer's polling ID. 
	 */
	virtual bool CanGetRightAnswerItemList(const zchar_t* pollingID) = 0;
	/**
	 * @brief Gets the list of polling question or subquestion's right answer.
	 * @param pollingID Specify the right answer's polling ID. 
	 * @return If the function succeeds, it returns a pointer to the IList<IPollingAnswerItem*>, Otherwise, the return value is nullptr. Otherwise, this function fails and returns nullptr.
	 */
	virtual IList<IPollingAnswerItem*>* GetPollingRightAnswerItemList(const zchar_t* pollingID) = 0;
	/**
	 * @brief Determines if the answer result list can be show.
	 * @param pollingID Specify the answer result's polling ID. 
	 */
	virtual bool CanShowAnswerResultList(const zchar_t* pollingID) = 0;
	/**
	 * @brief Gets the list of polling answer result item.
	 * @param pollingID Specify the answer result's polling ID. 
	 * @return If the function succeeds, the return value is a pointer to the IList<IPollingAnswerResultItem*>. Otherwise, the return value is nullptr.
	 */
	virtual IList<IPollingAnswerResultItem*>* GetPollingAnswerResultItemList(const zchar_t* pollingID) = 0;
	/**
	 * @brief Gets the polling item object of specified polling ID.
	 * @param pollingID Specify the polling ID where you want to get the information. 
	 * @return If the function succeeds, it returns a pointer to the IPollingItem. Otherwise, this function fails and returns nullptr.
	 */
	virtual IPollingItem* GetPollingItemByID(const zchar_t* pollingID) = 0;
	/**
	 * @brief Gets the polling question's image path.
	 * @param pollingID Specify the answer's polling ID. 
	 * @param questionID Specify the answer's question ID. 
	 * @return If the function succeeds, the return value is the question's image path. Otherwise, the return value is nullptr.
	 */
	virtual const zchar_t* GetPollingQuestionImagePath(const zchar_t* pollingID, const zchar_t* questionID) = 0;
	/**
	 * @brief Gets the polling question's minimum character and maximum character count.
	 * @param pollingID Specify the question's polling ID. 
	 * @param pollingID Specify the question's question ID. 
	 * @param minLen Specify the minimum number of words to submit. 
	 * @param maxLen Specify the maximum number of words to submit. 
	 * @note This function can only be used by short answer question and long answer question. 
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError GetQuestionCharactersLengthRange(const zchar_t* pollingID, const zchar_t* questionID, unsigned int& minLen, unsigned int& maxLen) = 0;
	/**
	 * @brief Determines if the question is case sensitive.
	 * @param pollingID Specify the question's polling ID. 
	 * @param questionID Specify the question's question ID.  
	 * @note This function can only be used by fill blank questions. 
	 */
	virtual bool IsQuestionCaseSensitive(const zchar_t* pollingID, const zchar_t* questionID) = 0;

	/** 
	 * @note Host 
	 */
	
	/**
	 * @brief Determines if the host can add polling.
	 * @note Only the origin host can add polling. 
	 */
	virtual bool CanAddPolling() = 0;
	/**
	 * @brief Open create polling in a web browser.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError AddPolling() = 0;
	/**
	 * @brief Determines if the host can edit polling.
	 * @param pollingID Specify the edit poll's polling ID. 
	 * @note Only the origin host can edit polling.
	 */
	virtual bool CanEditPolling(const zchar_t* pollingID) = 0;
	/**
	 * @brief Open edit polling in a web browser.
	 * @param pollingID Specify the edit poll's polling ID. 
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError EditPolling(const zchar_t* pollingID) = 0;
	/**
	 * @brief Determines if the host can delete polling.
	 * @param pollingID Specify the delete poll's polling ID. 
	 * @note Only the origin host can delete polling. 
	 */
	virtual bool CanDeletePolling(const zchar_t* pollingID) = 0;
	/**
	 * @brief Delete the polling.
	 * @param pollingID Specify the delete poll's polling ID. 
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError DeletePolling(const zchar_t* pollingID) = 0;
	/**
	 * @brief Determines if the host can duplicate polling.
	 * @param pollingID Specify the duplicate poll's polling ID. 
	 * @note Only the origin host can duplicate polling. 
	 */
	virtual bool CanDuplicatePolling(const zchar_t* pollingID) = 0;
	/**
	 * @brief Duplicate the polling.
	 * @param pollingID Specify the duplicate poll's polling ID. 
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError DuplicatePolling(const zchar_t* pollingID) = 0;
	/**
	 * @brief Determines if the user can view polling results in the browser.
	 * @param pollingID Specify the poll's polling ID. 
	 * @note Only the origin host can view. 
	 */
	virtual bool CanViewPollingResultFromBrowser(const zchar_t* pollingID) = 0;
	/**
	 * @brief Open a polling result in the web browser.
	 * @param pollingID Specify the poll's polling ID. 
	 */
	virtual SDKError ViewPollingResultFromBrowser(const zchar_t* pollingID) = 0;
	/**
	 * @brief Gets the list of poll item. 
	 * @return If the function succeeds, the return value is a pointer to the IList<IPollingItem*>. Otherwise, the return value is nullptr.
	 */
	virtual IList<IPollingItem*>* GetPollingItemList() = 0;
	/**
	 * @brief Determines if the host or co-host can start the polling.
	 * @param pollingID Specify the poll's polling ID. 
	 */
	virtual bool CanStartPolling(const zchar_t* pollingID) = 0;
	/**
	 * @brief Starts the polling.
	 * @param pollingID Specify the poll's polling ID. 
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError StartPolling(const zchar_t* pollingID) = 0;
	/**
	 * @brief Stops the polling.
	 * @param pollingID Specify the poll's polling ID. 
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError StopPolling(const zchar_t* pollingID) = 0;
	/**
	 * @brief Determines if the host or co-host can restart the polling.
	 * @param pollingID Specify the poll's polling ID. 
	 */
	virtual bool CanRestartPolling(const zchar_t* pollingID) = 0;
	/**
	 * @brief Restart the polling.
	 * @param pollingID Specify the poll's polling ID. 
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError RestartPolling(const zchar_t* pollingID) = 0;
	/**
	 * @brief Determines if the host or co-host can share the poll's result.
	 * @param pollingID Specify the poll's polling ID. 
	 */
	virtual bool CanSharePollingResult(const zchar_t* pollingID) = 0;
	/**
	 * @brief Share the poll's result.
	 * @param pollingID Specify the poll's polling ID. 
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError StartSharePollingResult(const zchar_t* pollingID) = 0;
	/**
	 * @brief Stops the poll's result.
	 * @param pollingID Specify the poll's polling ID. 
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError StopSharePollingResult(const zchar_t* pollingID) = 0;
	/**
	 * @brief Sets to enable show right answer to participants when share quiz's result. 
	 * @param bEnable true indicates to enable. false not.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError EnableGetRightAnswerList(bool bEnable) = 0;
	/**
	 * @brief Determines if the host can download poll's result from browser.
	 * @note Only the origin host can download. 
	 */
	virtual bool CanDownloadResult() = 0;
	/**
	 * @brief Download all stopped poll's result from browser.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError DownLoadResult() = 0;

	/** 
	 * @note Attendee 
	 */
	 
	/**
	 * @brief Attendee set answer's check.
	 * @param answerItem Specify the answer item to set.
	 * @param bCheck Specify you want to select the answer or not. 
	 * @note This function should only be used by single, matching, rank order, multi, rating scale or drop down questions. 
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError SetAnswerCheck(IPollingAnswerItem* answerItem, bool bCheck) = 0;
	/**
	 * @brief Attendee set answer's content.
	 * @param answerItem Specify the answer item to set.
	 * @param answerText Specify you want to reply answer's content. 
	 * @note This function should only be used by fill blank, short answer or long answer questions. 
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError SetAnswerContent(IPollingAnswerItem* answerItem, const zchar_t* answerText) = 0;
	/**
	 * @brief Determines if the attendee can submit the polling.
	 * @param pollingID Specify the submit poll's polling ID. 
	 */
	virtual bool CanSubmitPolling(const zchar_t* pollingID) = 0;
	/**
	 * @brief Submit the polling.
	 * @param pollingID Specify the submit poll's polling ID. 
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError SubmitPolling(const zchar_t* pollingID) = 0;
};

END_ZOOM_SDK_NAMESPACE
#endif