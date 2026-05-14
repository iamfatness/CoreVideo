/**
 * @file ui_hook_interface.h
 * @brief UI Hook Interface 
 */
#ifndef _UI_HOOK_INTERFACE_H_
#define _UI_HOOK_INTERFACE_H_

#include "zoom_sdk_def.h"

BEGIN_ZOOM_SDK_NAMESPACE
/**
 * @brief Enumeration of SDK hook type.
 * Here are more detailed structural descriptions.
 */
enum UIHOOKHWNDTYPE
{
	/** The type of hooking messages and windows defined by users. */
	UIHOOKWNDTYPE_USERDEFIEND,
	/** The type of hooking main window. */
	UIHOOKWNDTYPE_MAINWND,
	/** The type of hooking toolbar at the bottom. */
	UIHOOKWNDTYPE_BOTTOMTOOLBAR,
};

/**
 * @class IUIHookerEvent
 * @brief UI hooker callback event.
 */
class IUIHookerEvent
{
public:
	/**
	 * @brief UI relative action hooked callback. The callback works only in the working thread and does not feedback to the main thread.
	 * @param type Hook type.
	 * @param msg Hooked message. For more details, see windows MSG structure.
	 */
	virtual void onUIActionNotify(UIHOOKHWNDTYPE type, MSG msg) = 0;
};

/**
 * @class IUIHooker
 * @brief Embedded browser interface.
 */
class IUIHooker
{
public:
	/**
	 * @brief Set UI Hooker callback event handler.
	 * @param pEvent A pointer to the IUIHookerEvent that receives UI hooker event. 
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError SetEvent(IUIHookerEvent* pEvent) = 0;
	
	/**
	 * @brief Add or remove the windows message to be hooked.
	 * @param wndmsgid Specify the windows message ID, such as WM_CREATE.
	 * @param bAdd true indicates to add.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError MonitorWndMessage(unsigned int wndmsgid, bool bAdd = true) = 0;
	
	/**
	 * @brief Add or remove the windows to be hooked.
	 * @param classname Get the value of the windows class name to be hooked or unhooked via windows API GetClassName().
	 * @param bAdd true indicates to add.
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError MonitorWnd(const zchar_t* classname, bool bAdd = true) = 0;
	
	/**
	 * @brief Starts hooking. 
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 */
	virtual SDKError Start() = 0;
	
	/**
	 * @brief Stops hooking. 
	 * @return If the function succeeds, the return value is SDKERR_SUCCESS. Otherwise, this function returns an error.
	 * @note Calling the function does not clean up the hook list in the SDK. Remove the message or windows that you do not want to hook next time via MonitorWndMessage() or MonitorWnd().
	 */
	virtual SDKError Stop() = 0;
};
END_ZOOM_SDK_NAMESPACE
#endif