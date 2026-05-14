/**
 * @file meeting_h323_helper_interface.h
 * @brief H.323 helper Interface of meeting service
 * @note Valid for both ZOOM style and user custom interface mode.
 */
#ifndef _MEETING_H323_HELPER_INTERFACE_H_
#define _MEETING_H323_HELPER_INTERFACE_H_
#include "zoom_sdk_def.h"
#include <string>

BEGIN_ZOOM_SDK_NAMESPACE


/**
 * @brief Enumeration of H.323 callout status.
 * Here are more detailed structural descriptions.
 */
enum H323CalloutStatus
{
	/** Used only for initialization. */
	H323Callout_Unknown, 
	/** Call out successfully. */
	H323Callout_Success, 
	/** Bell during the call. */
	H323Callout_Ring,   
	/** Call timeout. */
	H323Callout_Timeout, 
	/** Call fails. */
	H323Callout_Failed, 
	/** Busy */
	H323Callout_Busy,	
	/** Decline */
	H323Callout_Decline, 
};

/**
 * @brief Enumeration of H.323 pairing status.
 * Here are more detailed structural descriptions.
 */
enum H323ParingResult
{
	/** Used only for initialization. */
	H323Paring_Unknown, 
	/** Pairing successful. */
	H323Paring_Success,
	/** The meeting being paired does not exist. */
	H323Paring_Meeting_Not_Exist,
	/** Pairing code does not exist. */
	H323Paring_Paringcode_Not_Exist,
	/** No pairing privilege. */
	H323Paring_No_Privilege, 
	/** Other issues. */
	H323Paring_Other_Error, 
};
/**
 * @class IMeetingH323HelperEvent
 * @brief Meeting H.323 Helper callback event.
 */
class IMeetingH323HelperEvent
{
public:
	virtual ~IMeetingH323HelperEvent() {}
	/**
 	 * @brief Callback event when the calling status of H.323 device changes.
	 * @param status H.323 device calling out status value. For more details.
	 */
	virtual void onCalloutStatusNotify(H323CalloutStatus status) = 0;
	
	/**
 	 * @brief The callback event is triggered when the result of using the H.323 device to pair the assigned meeting is out.
	 * @param result Paring result. For more details.
	 * @param meetingNumber The meeting number to be paired.
	 */
	virtual void onParingH323Result(H323ParingResult result, UINT64 meetingNumber) = 0;
};


/**
 * @brief Enumeration of H.323 device type.
 * Here are more detailed structural descriptions.
 */
enum H323DeviceType
{
	/** Unknown device, only for initialization. */
	H323DeviceType_Unknown,
	/** H.323 device. */
	H323DeviceType_H323,
	/** SIP device. */
	H323DeviceType_SIP,
	/** H.323 device and SIP device. */
	H323DeviceType_BOTH,
};

/**
 * @class IH323Device
 * @brief Meeting H.323 device Interface
 */
class IH323Device
{
public:
	virtual const zchar_t* GetName() = 0;
	virtual const zchar_t* GetIP() = 0;
	virtual const zchar_t* GetE164Num() = 0;
	virtual H323DeviceType GetDeviceType() = 0;
	virtual bool IsCustomizedH323Device() = 0;
	virtual ~IH323Device(){};
};

/**
 * @class CustomizedH323Device
 * @brief Customized Meeting H.323 device
 */
class CustomizedH323Device : public IH323Device
{
public:
	CustomizedH323Device()
	{
		_type = H323DeviceType_Unknown;
	}

	virtual void SetName(const zchar_t* name_)
	{
		if (name_)
			_name = name_;
	}

	virtual const zchar_t* GetName()
	{
		return _name.c_str();
	}

	virtual void SetIP(const zchar_t* ip_)
	{
		if (ip_)
			_ip = ip_;
	}

	virtual const zchar_t* GetIP()
	{
		return _ip.c_str();
	}

	virtual void SetE164Num(const zchar_t* e164num_)
	{
		if (e164num_)
			_e164num = e164num_;
	}

	virtual const zchar_t* GetE164Num()
	{
		return _e164num.c_str();
	}

	virtual void SetDeviceType(H323DeviceType type_)
	{
		_type = type_;
	}

	virtual H323DeviceType GetDeviceType()
	{
		return _type;
	}

	virtual bool IsCustomizedH323Device()
	{
		return true;
	}

private:
	/** Type of device. */
	H323DeviceType _type;
	/** Device name. */
	std::wstring _name;
	/** Device IP. */
	std::wstring _ip;
	/** E.164 number */
	std::wstring _e164num;
};

/**
 * @class IMeetingH323Helper
 * @brief Meeting H323 Helper Interface.
 */
class IMeetingH323Helper
{
public:
	/**
 	 * @brief Sets meeting H.323 helper callback event handler.
	 * @param pEvent A pointer to the IMeetingH323HelperEvent that receives H.323 helper event. 
	 */
	virtual void SetEvent(IMeetingH323HelperEvent* pEvent) = 0;
	
	/**
 	 * @brief Gets the list of H.323 call-in number supported by the current meeting.
	 * @return If the function succeeds, the return value is the pointer to the list of the call-in number. Otherwise, the return value is nullptr.
	 */
	virtual IList<const zchar_t* >* GetH323Address() = 0;
	
	/**
 	 * @brief Gets the H.323 password for the current meeting.
	 * @return If the function succeeds, the return value is the H.323 meeting connect password. Otherwise, the return value is the length of zero(0). 
	 */
	virtual const zchar_t* GetH323Password() = 0;
	
	/**
 	 * @brief Determines if it is enabled to pair the meeting.
	 * @param meetingNum true Specifies the meeting number.
	 * @return If pairing meeting is enabled, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError CanPairingMeeting(UINT64 meetingNum) = 0;
	
	/**
 	 * @brief Sends Meeting paring code.
	 * @param meetingNum Specifies which meeting to paring. 
	 * @param paringCode Code for paring.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 * @note If the function succeeds, the IMeetingH323HelperEvent::onParingH323Result() will be triggered once received the response of the H.323 device. 
	 */
	virtual SDKError SendMeetingParingCode(UINT64 meetingNum, zchar_t* paringCode) = 0;
	
	/**
	 * @brief Gets the list of the call-out devices supported by the current meeting.
	 * @return If the function succeeds, the return value is the pointer to the list of devices.  Otherwise, the return value is an empty list.
	 * @note The list will be cleared each time the function is called.
	 */
	virtual IList<IH323Device* >* GetCalloutH323DeviceList() = 0;
	
	/**
 	 * @brief Call out with the assigned H.323 device.
	 * @param deviceInfo Specify the H.323 device to use.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS.  Otherwise, this function returns an error.
	 * @note If the function succeeds, the IMeetingH323HelperEvent::onCalloutStatusNotify() will be triggered once the callout status of H.323 device changes.
	 */
	virtual SDKError CallOutH323(IH323Device* deviceInfo) = 0;
	
	/**
 	 * @brief Cancel current outgoing call.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError CancelCallOutH323() = 0;
};
END_ZOOM_SDK_NAMESPACE
#endif