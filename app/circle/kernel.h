// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/circle/kernel.h — bare-metal entry object for the Raspberry Pi 400.
// Milestone 1: startup screen, HDMI+UART log, full CPU clock, SD-card log
// file, and USB keyboard/mouse input test.

#ifndef G3_APP_CIRCLE_KERNEL_H
#define G3_APP_CIRCLE_KERNEL_H

#include <circle/actled.h>
#include <circle/cputhrottle.h>
#include <circle/devicenameservice.h>
#include <circle/exceptionhandler.h>
#include <circle/input/mouse.h>
#include <circle/interrupt.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/screen.h>
#include <circle/serial.h>
#include <circle/timer.h>
#include <circle/types.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbkeyboard.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>

#include "core/common/spsc_queue.h"
#include "platform/common/hid_keyboard.h"
#include "platform/common/config_menu.h"
#include "platform/common/input_events.h"
#include "mac_host.h"
#include "platform/common/mac_session.h"
#include "settings.h"
#include "teedevice.h"

enum TShutdownMode
{
	ShutdownNone,
	ShutdownHalt,
	ShutdownReboot
};

class CKernel
{
public:
	CKernel (void);
	~CKernel (void);

	boolean Initialize (void);

	TShutdownMode Run (void);

private:
	void ShowStartupScreen (void);
	void LogHostInfo (void);
	boolean RunCoreSelfTest (void);

	// SD card log file (SD:/g3macpi/logs/boot.log, previous run kept as boot-prev.log)
	boolean MountSDCard (void);
	void WriteLogFile (void);

	// Settings menu (F12 switches between it and the log view)
	void SetUpSettings (void);
	void ShowMenu (boolean bShow);
	void HandleMenuKey (u8 ucUsage);

	// The emulated Mac
	void StartMac (void);
	void StopMac (void);
	void RunMac (void);
	static void DiscEjected (void *pContext, u32 nDrive);

	// USB input test
	void AttachInputDevices (void);
	void ProcessInputEvents (void);
	void FlushMouseLog (void);

	// Called by Circle's USB drivers in interrupt context: they only
	// convert and queue events; all printing happens in ProcessInputEvents().
	static void KeyStatusHandlerRaw (unsigned char ucModifiers, const unsigned char RawKeys[6]);
	static void MouseStatusHandler (unsigned nButtons, int nDisplacementX, int nDisplacementY,
					int nWheelMove);
	static void KeyboardRemovedHandler (CDevice *pDevice, void *pContext);
	static void MouseRemovedHandler (CDevice *pDevice, void *pContext);
	void QueueEvent (const g3::InputEvent &Event);

private:
	// Do not change this order: members are constructed top to bottom.
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CScreenDevice		m_Screen;
	CSerialDevice		m_Serial;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CCPUThrottle		m_CPUThrottle;
	CTeeDevice		m_LogTee;
	CLogger			m_Logger;
	CUSBHCIDevice		m_USBHCI;
	CEMMCDevice		m_EMMC;
	FATFS			m_FileSystem;

	boolean			m_bSDMounted;
	CUSBKeyboardDevice	*m_pKeyboard;
	CMouseDevice		*m_pMouse;

	// Input path: USB IRQ -> tracker -> queue -> main loop.
	g3::HidKeyboardTracker	m_KeyTracker;			// used in IRQ context only
	g3::SpscQueue<g3::InputEvent, 256> m_InputQueue;
	volatile unsigned	m_nDroppedEvents;

	// Main-loop state
	unsigned		m_nHeldModifiers;		// bit n = usage 0xE0+n held
	int			m_nMouseDX, m_nMouseDY, m_nMouseWheel;
	unsigned		m_nMouseButtons;
	unsigned		m_nMouseEvents;
	unsigned		m_nLastMouseLogTicks;

	TShutdownMode		m_ShutdownMode;

	// Settings
	CFatFsLister		m_Lister;
	g3::ConfigMenu		*m_pMenu;
	boolean			m_bMenuShown;

	// The emulated Mac (memory comes from the high heap: above 1 GB)
	g3::MacSession		m_Session;
	CCircleMacHost		m_MacHost;
	u8			*m_pRam;
	u32			m_nRamSize;
	u8			*m_pVram;
	u32			*m_pPixels;		// screen conversion buffer
	boolean			m_bMacRunning;
	unsigned		m_nLastDrawTicks;
	unsigned		m_nLastStatsTicks;
	u64			m_nGuestMs;		// guest time run so far

	static CKernel		*s_pThis;
};

#endif
