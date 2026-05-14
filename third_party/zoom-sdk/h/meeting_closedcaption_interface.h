/**
 * @file meeting_claosecaptioin_interface.h
 * @brief Meeting Service Closed Caption Interface.
 * @note Valid for both ZOOM style and user custom interface mode.
 */
#ifndef _MEETING_CLOSEDCAPTION_INTERFACE_H_
#define _MEETING_CLOSEDCAPTION_INTERFACE_H_
#include "zoom_sdk_def.h"

BEGIN_ZOOM_SDK_NAMESPACE
/**
 * @brief Enumerations of the type for live transcription status.
 */
typedef enum
{
	/** not start */
	SDK_LiveTranscription_Status_Stop = 0,  
	/** start */
	SDK_LiveTranscription_Status_Start = 1,  
	/** user sub */
	SDK_LiveTranscription_Status_User_Sub = 2, 
	/** connecting */
	SDK_LiveTranscription_Status_Connecting = 10,	 
}SDKLiveTranscriptionStatus;

/**
 * @brief Enumerations of the type for live transcription operation type.
 */
typedef enum
{
	SDK_LiveTranscription_OperationType_None = 0,
	SDK_LiveTranscription_OperationType_Add,
	SDK_LiveTranscription_OperationType_Update,
	SDK_LiveTranscription_OperationType_Delete,
	SDK_LiveTranscription_OperationType_Complete,
	SDK_LiveTranscription_OperationType_NotSupported,
}SDKLiveTranscriptionOperationType;

/**
 * @class ILiveTranscriptionLanguage
 * @brief live transcription language interface.
 */
class ILiveTranscriptionLanguage
{
public:
	virtual int GetLTTLanguageID() = 0;
	virtual const zchar_t* GetLTTLanguageName() = 0;
	virtual ~ILiveTranscriptionLanguage() {};
};

/**
 * @class ILiveTranscriptionMessageInfo
 * @brief live transcription message interface.
 */
class ILiveTranscriptionMessageInfo
{
public:
	/**
	 * @brief Gets the message ID of the current message.
	 * @return If the function succeeds, it returns the message ID of the current message. Otherwise, this function fails and returns a string of length zero(0).
	 */
	virtual const zchar_t* GetMessageID() = 0;

	/**
	 * @brief Gets the speaker's ID.
	 * @return If the function succeeds, it returns the speaker ID.
	 */
	virtual unsigned int GetSpeakerID() = 0;

	/**
	 * @brief Gets the speaker's name.
	 * @return If the function succeeds, it returns the speaker name. 
	 */
	virtual const zchar_t* GetSpeakerName() = 0;

	/**
	 * @brief Gets the content of the current message.
	 * @return If the function succeeds, it returns the message content. 
	 */
	virtual const zchar_t* GetMessageContent() = 0;

	/**
	 * @brief Gets the timestamp of the current message.
	 * @return If the function succeeds, it returns the message timestamp. 
	 */
	virtual time_t GetTimeStamp() = 0;

	/**
	 * @brief Gets the type of the current message.
	 * @return If the function succeeds, it returns the message type.
	 */
	virtual SDKLiveTranscriptionOperationType GetMessageOperationType() = 0;

	virtual ~ILiveTranscriptionMessageInfo() {};
};

/**
 * @class ICCRequestHandler
 * @brief The helper to handle the requested of start captions.
 * When IsRequestTranslationOn is true, use ICCRequestTranslationOnHandler::ApproveStartCaptionsRequest() to approve start captions request.
 * When isRequestTranslationOn is false, use ICCRequestTranslationOffHandler::ApproveStartCaptionsRequest(int languageId) to approve start captions request.
 */
class ICCRequestHandler
{
public:
	/**
	 * @brief Determines whether the request is to start captions with translation on.
	 * @return true if the request is to start captions with translation on. Otherwise, false.
	 */
	virtual bool IsRequestTranslationOn() = 0;

	/**
	 * @brief Gets the user ID of the user who sent the request.
	 * @return If the function succeeds, it returns the user ID.
	 */
	virtual unsigned int GetSenderUserId() = 0;

	/**
	 * @brief Declines the request to start captions.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError Deny() = 0;

	virtual ~ICCRequestHandler() {};
};

class ICCRequestTranslationOnHandler : public ICCRequestHandler
{
public:
	/**
	 * @brief Approves the start captions request.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError ApproveStartCaptionsRequest() = 0;

	virtual ~ICCRequestTranslationOnHandler() {};
};

class ICCRequestTranslationOffHandler : public ICCRequestHandler
{
public:
	/**
	 * @brief Approves the start captions request.
	 * @param languageId The language to be set for all participants in meeting.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError ApproveStartCaptionsRequest(int languageId) = 0;

	virtual ~ICCRequestTranslationOffHandler() {};
};


/**
 * @class IClosedCaptionControllerEvent
 * @brief Closed Caption controller callback event.
 */
class IClosedCaptionControllerEvent
{
public:
	virtual ~IClosedCaptionControllerEvent() {}

	/**
	 * @brief Callback event when the user is assigned to send Closed Caption messages.
	 */
	virtual void onAssignedToSendCC(bool bAssigned) = 0;

	/** 
	 * @brief Callback event when the user receives Closed Caption messages.
	 * If the meeting supports multi-language transcription and the host sets manual captions to true,
	 * attendees must set the translation language id to -1 to receive closed captioned messages.
	 * @param ccMsg: an object pointer to the closed caption message content. 
	 * @param sender_id: the sender id of a closed caption message. 
	 * @param time: the time when a caption message was sent.
	 */
	virtual void onClosedCaptionMsgReceived(const zchar_t* ccMsg, unsigned int sender_id, time_t time) = 0;

	/**
	 * @brief live transcription status changed callback.
	 * @param status: the live transcription status.
	 * @note This callback is triggered when the live transcription status changes. It can be triggered by calling StartLiveTranscription() or by calling SetTranslationLanguage() for the first time before calling StartLiveTranscription().
	 */
	virtual void onLiveTranscriptionStatus(SDKLiveTranscriptionStatus status) = 0;

	/**
	 * @brief original language message received callback.
	 * @param messageInfo The spoken language message.
	 */
	virtual void onOriginalLanguageMsgReceived(ILiveTranscriptionMessageInfo* messageInfo) = 0;

	/**
	 * @brief live transcription message received callback.
	 * @param messageInfo The live transcription message.
	 */
	virtual void onLiveTranscriptionMsgInfoReceived(ILiveTranscriptionMessageInfo* messageInfo) = 0;

	/**
	 * @brief The translation message error callback.
	 * @param speakingLanguage: an object of the spoken message language. 
	 * @param transcriptLanguageId: an object of the message language you want to translate.
	 */
	virtual void onLiveTranscriptionMsgError(ILiveTranscriptionLanguage* spokenLanguage, ILiveTranscriptionLanguage* transcriptLanguage) = 0;

	/**
	 * @brief only host can receive this callback.
	 * @param requester_id: the request user id, if param bAnonymous is true, the requester_id is 0, no meanings. 
	 * @param bAnonymous: specify the request whether user anonymous request.
	 */
	virtual void onRequestForLiveTranscriptReceived(unsigned int requester_id, bool bAnonymous) = 0;

	/**
	 * @brief request live transcription status changed callback.
	 * @param bEnabled Specify the request live transcription status is enable, true enable, false disabled.
	 */
	virtual void onRequestLiveTranscriptionStatusChange(bool bEnabled) = 0;

	/**
	 * @brief Sink the event of captions enabled status changed.
	 * @param bEnabled true indicates the host enables the captions, otherwise means the host disables the captions.
	 */
	virtual void onCaptionStatusChanged(bool bEnabled) = 0;

	/**
	 * @brief Sink the event to start captions request.
	 * @param handler A pointer to the ICCRequestHandler.
	 */
	virtual void onStartCaptionsRequestReceived(ICCRequestHandler* handler) = 0;

	/**
	 * @brief Sink the event to start captions request was approved.
	 */
	virtual void onStartCaptionsRequestApproved() = 0;

	/**
	 * @brief Sink the event of manual captions enablement status change.
	 * @param bEnabled true indicates the host enables the manual captions. Otherwise the host disabled the manual captions.
	 */
	virtual void onManualCaptionStatusChanged(bool bEnabled) = 0;

	/**
	 * @brief Callback when the spoken language is changed.
	 * @param spokenLanguage A pointer to the current spoken language object.
	 */
	virtual void onSpokenLanguageChanged(ILiveTranscriptionLanguage* spokenLanguage) = 0;
};

/**
 * @class IClosedCaptionController
 * @brief Closed caption controller interface.
 */
class IClosedCaptionController
{
public:
	/**
	 * @brief Sets the controller event of closed caption(CC).
	 */
	virtual SDKError SetEvent(IClosedCaptionControllerEvent* pEvent) = 0;

	/**
	 * @brief Determines if the current meeting supports the CC feature.
	 */
	virtual bool IsMeetingSupportCC() = 0;

	/**
	 * @brief Query if it is able to assign others to send CC.
	 */
	virtual bool CanAssignOthersToSendCC() = 0;

	/**
	 * @brief Assign a user to send CC.
	 * @note Zero(0) means to assign the current user himself.
	 */
	virtual SDKError AssignCCPrivilege(unsigned int userid, bool bAssigned) = 0;

	/**
	 * @brief Determines if the user can be assigned as a CC sender.
	 */
	virtual bool CanBeAssignedToSendCC(unsigned int userid) = 0;

	/**
	 * @brief Query if the user can send CC.
	 */
	virtual bool CanSendClosedCaption() = 0;

	/**
	 * @brief Sends the CC message.
	 */
	virtual SDKError SendClosedCaption(const zchar_t* ccMsg) = 0;

	/**
	 * @brief Determines if it is enabled to save CC.
	 */
	virtual bool IsSaveCCEnabled() = 0;

	/**
	 * @brief History of saving CC.
	 */
	virtual SDKError SaveCCHistory() = 0;

	/**
	 * @brief Gets the path of saving CC.
	 */
	virtual const zchar_t* GetClosedCaptionHistorySavedPath() = 0;

	/**
	 * @brief Gets the CC URL used by the third party service.
	 */
	virtual const zchar_t* GetClosedCaptionUrlFor3rdParty() = 0;

	/**
	 * @brief Enables or disable manual captions for the meeting.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError EnableMeetingManualCaption(bool bEnable) = 0;

	/**
	 * @brief Determines whether manually added closed captions is enabled for the meeting.
	 */
	virtual bool IsMeetingManualCaptionEnabled() = 0;

	/**
	 * @brief Determines whether the legal notice for Live transcript is available.
	 * @return true indicates the legal notice for Live transcript is available. Otherwise false.
	 */
	virtual bool IsLiveTranscriptLegalNoticeAvailable() = 0;

	/**
	 * @brief Enables or disable to receive original and translated content.If enable this feature,you need start live transcription.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error and returns an error.
	 */
	virtual SDKError EnableReceiveSpokenLanguageContent(bool bEnable) = 0;

	/**
	 * @brief Query if the user can disable captions.
	 * @return true indicates that the host can disable captions.
	 */
	virtual bool CanDisableCaptions() = 0;

	/**
	 * @brief Enables or disable captions.
	 * @param bEnable true indicates that captions are enabled; false means that captions are disabled.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError EnableCaptions(bool bEnable) = 0;

	/**
	 * @brief Query if the captions enabled.
	 * @return true indicates that captions are enabled.
	 */
	virtual bool IsCaptionsEnabled() = 0;

	/**
	 * @brief Determines whether receive original and translated is available.
	 * @return true indicates receive original and translated is available. Otherwise false.
	 */
	virtual bool IsReceiveSpokenLanguageContentEnabled() = 0;

	/**
	 * @brief Gets the CC legal notices prompt.
	 */
	virtual const zchar_t* getLiveTranscriptLegalNoticesPrompt() = 0;

	/**
	 * @brief Gets the CC legal notices explained.
	 */
	virtual const zchar_t* getLiveTranscriptLegalNoticesExplained() = 0;

	/**
	 * @brief Determines whether the live transcription feature is enabled.
	 */
	virtual bool IsLiveTranscriptionFeatureEnabled() = 0;

	/**
	 * @brief Determines whether the multi-language transcription feature is enabled.
	 */
	virtual bool IsMultiLanguageTranscriptionEnabled() = 0;

	/**
	 * @brief Determines whether the translated captions feature is enabled.
	 */
	virtual bool IsTextLiveTranslationEnabled() = 0;

	/**
	 * @brief Gets the current live transcription status.
	 * @return type: the current live transcription status type.
	 */
	virtual SDKLiveTranscriptionStatus GetLiveTranscriptionStatus() = 0;

	/**
	 * @brief Query if the user can start live transcription.
	 */
	virtual bool CanStartLiveTranscription() = 0;

	/**
	 * @brief Starts live transcription.
	 * If the meeting allows multi-language transcription,all users can start live transcription.Otherwise only the host can start it
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError StartLiveTranscription() = 0;

	/**
	 * @brief Stops live transcription.
	 * @note If the meeting allows multi-language transcription,all user can stop live transcription.Otherwise only the host can stop it
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError StopLiveTranscription() = 0;

	/**
	 * @brief Enables or disable the ability for attendees to request live transcriptions.
	 * @note If the meeting allows multi-language transcription,the return value is SDKERR_WRONG_USAGE
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError EnableRequestLiveTranscription(bool bEnable) = 0;

	/**
	 * @brief Determines whether the request to start live transcription is enabled.If the return value is true, it is enabled, if false, disabled.
	 * @note If the meeting allows multi-language transcription, the return value is false
	 */
	virtual bool IsRequestToStartLiveTranscriptionEnabled() = 0;

	/** 
	 * @brief Request the host to start live transcription.
	 * @note If the meeting allows multi-language transcription,the return value is SDKERR_WRONG_USAGE
	 * @param bRequestAnonymous true indicates the user anonymous request.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError RequestToStartLiveTranscription(bool bRequestAnonymous) = 0;

	/**
	 * @brief Gets the list of all available speaking languages in meeting.
	 * @return If the function succeeds, the return value is the list of the available spoken languages in a meeting. Otherwise, this function returns an error, the return value is nullptr.	
	 */
	virtual IList<ILiveTranscriptionLanguage*>* GetAvailableMeetingSpokenLanguages() = 0;

	/**
	 * @brief Sets the spoken language of the current user.
	 * @param languageID: The spoken language id.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 * @deprecated Use \link SetMeetingSpokenLanguage \endlink instead.
	 */
	virtual SDKError SetMeetingSpokenLanguage(int languageID) = 0;

	/**
	 * @brief Gets the spoken language of the current user.
	 */
	virtual ILiveTranscriptionLanguage* GetMeetingSpokenLanguage() = 0;

	/**
	 * @brief Gets the list of all available translation languages in a meeting.
	 * @return If the function succeeds, the return value is the list of all available translation languages in a meeting. Otherwise, this function returns an error, the return value is nullptr.
	 */
	virtual IList<ILiveTranscriptionLanguage*>* GetAvailableTranslationLanguages() = 0;

	/**
	 * @brief Sets the translation language of the current user.	
	 * @param languageID: The translation language id.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 * @note If the language id is set to -1, live translation will be disabled.You can receive closed captions if the host sets manual captions to true for the meeting. If you call this method before calling StartLiveTranscription(), it will trigger the onLiveTranscriptionStatus callback when the translation status changes. If translation is already enabled, calling this method again will not trigger the onLiveTranscriptionStatus callback.
	 */
	virtual SDKError SetTranslationLanguage(int languageID) = 0;

	/**
	 * @brief Gets the translation language of the current user.
	 */
	virtual ILiveTranscriptionLanguage* GetTranslationLanguage() = 0;

	/**
	 * @brief Determines whether users can request to start captions.
	 * @return true indicates users can request to start captions. Otherwise false.
	 */
	virtual bool IsSupportRequestCaptions() = 0;

	/**
	 * @brief Determines whether support translation when users request to start captions.
	 * @return true indicates translation is available when users request to start captions. Otherwise false.
	 */
	virtual bool IsSupportTranslationWhenRequestToStartCaptions() = 0;

	/**
	 * @brief the host to start captions. If the host approves your request, you receive the callback onStartCaptionsRequestApproved, and you should start captions of translation there.
	 * @param bEnableTranslation true indicates to enable translation at the same time.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError RequestToStartCaptions(bool bEnableTranslation) = 0;

	/**
	 * @brief Sets the current user's spoken language.
	 * @param languageID The spoken language ID.
	 * @param bIsForAll true indicates set spoken language for all users. false means set the language for myself.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError SetMeetingSpokenLanguage(int languageID, bool bIsForAll) = 0;
};

END_ZOOM_SDK_NAMESPACE
#endif