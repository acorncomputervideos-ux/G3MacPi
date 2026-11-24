// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/circle/kernel.h — bare-metal entry object for the Raspberry Pi 400.
// Milestone 1: startup screen, HDMI+UART log, full CPU clock, SD-card log
// file, and USB keyboard/mouse input test.

#ifndef G3_APP_CIRCLE_KERNEL_H
#define G3_APP_CIRCLE_KERNEL_H

#include <circle/actled.h>
#include <circle/bcm54213.h>
#include <circle/cputhrottle.h>
#include <circle/devicenameservice.h>
#include <circle/exceptionhandler.h>
#include <circle/input/mouse.h>
#include <circle/interrupt.h>
#include <circle/koptions.h>
#include <circle/multicore.h>
#include <circle/logger.h>
#include <circle/sched/scheduler.h>
#include <circle/screen.h>
#include <circle/serial.h>
#include <circle/sound/hdmisoundbasedevice.h>
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
#include "platform/common/screen_presenter.h"
#include "core/cpu/ppc750/jit_worker.h"
#include "settings.h"
#include "teedevice.h"
#include "web.h"

enum TShutdownMode
{
	ShutdownNone,
	ShutdownHalt,
	ShutdownReboot
};

static const unsigned SoundSampleRate = 48000;	// HDMI sound

// The other cores: core 1 draws the Mac's screen (ScreenPresenter), core 2
// compiles the JIT's code (JitWorker), core 3 does disk-image I/O (DiskCache)
// and moves the Mac's sound to HDMI and its Ethernet frames to the adapter.
class CDrawCore : public CMultiCoreSupport, public CDiskIoCore
{
public:
	CDrawCore (CMemorySystem *pMemorySystem) : CMultiCoreSupport (pMemorySystem) {}
	void Setup (g3::ScreenPresenter *pPresenter, const g3::ScreenSurface &Surface)
	{
		m_pPresenter = pPresenter;
		m_Surface = Surface;
	}
	void Run (unsigned nCore) override;
	volatile boolean m_bStarted = FALSE;		// set by core 1 when it runs
	volatile boolean m_bWorkerStarted = FALSE;	// set by core 2 when it runs

	// Core 0: give core 2 the running Mac's JIT worker, or take it away
	// (DetachWorker returns once core 2 no longer uses it).
	void AttachWorker (g3::ppc::JitWorker *pWorker);
	void DetachWorker (void);

	// CDiskIoCore (core 0)
	volatile boolean m_bIoStarted = FALSE;	// set by core 3 when it runs
	boolean DiskIoRunning (void) const override { return m_bIoStarted; }
	void AttachDisk (g3::DiskCache *pCache) override;
	void DetachDisk (g3::DiskCache *pCache) override;

	// Core 0, once: the sound and network devices core 3 serves. The
	// network: the adapter, and the two stations sharing it (either
	// bridge may be 0): the Mac and the Pi's own stack (the web interface).
	void AttachSound (g3::AudioBridge *pBridge, CSoundBaseDevice *pDevice);
	void AttachNet (CNetDevice *pAdapter, g3::NetBridge *pMac, const u8 *pMacAddress,
			g3::NetBridge *pPi, const u8 *pPiAddress);

private:
	void RunWorker (void);
	void RunDiskIo (void);
	u32 ServiceSound (void);
	u32 ServiceNet (void);
	g3::AudioBridge *m_pAudio = 0;		// atomic accesses only
	CSoundBaseDevice *m_pSound = 0;
	CNetDevice *m_pNetDevice = 0;		// atomic accesses only
	g3::NetBridge *m_pMacNet = 0;
	g3::NetBridge *m_pPiNet = 0;
	u8 m_MacAddress[6] = {};
	u8 m_PiAddress[6] = {};
	unsigned m_nLastPHYTicks = 0;
	static const unsigned MaxDisks = 8;
	g3::DiskCache *m_pDisk[MaxDisks] = {};	// atomic accesses only
	unsigned m_nIoBusy = 0;			// core 3 is serving the disks
	g3::ScreenPresenter *m_pPresenter = 0;
	g3::ScreenSurface m_Surface;
	g3::ppc::JitWorker *m_pWorker = 0;	// atomic accesses only
	unsigned m_nWorkerBusy = 0;		// core 2 is inside poll()
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
	void StartDrawCore (void);
	void StartSoundAndNet (void);	// on core 3, once it runs
	CHDMISoundBaseDevice	*m_pHDMISound = 0;

	// The network (after the settings are read): Circle's TCP/IP stack on
	// our stand-in port, the switch on core 3, the web interface.
	void StartNetwork (void);
	void CheckWebAddress (void);
	static void WebStatus (void *pContext, g3::WebStatus &Out);
	static boolean WebInUse (void *pContext, const char *pPath);
	static const char *WebPassword (void *pContext);
	static void WebReboot (void *pContext);
	static void WebConfigChanged (void *pContext);
	static u32 WebInsertMedia (void *pContext, u32 nDrive, const char *pPath);
	CNetSubSystem		*m_pNet = 0;
	CPiNetPort		m_PiPort;
	CCircleWebHost		*m_pWebHost = 0;
	boolean			m_bWebAddressShown = FALSE;
	// Start-up: quiet (a splash, the log on the UART and SD card only)
	// unless cmdline.txt has bootlog=1; then the Mac starts at once unless
	// F12 or Esc comes during the first moments (autostart in the settings).
	boolean			m_bQuietBoot = TRUE;
	boolean			m_bBootWait = FALSE;
	boolean			m_bUSBReady = FALSE;	// the USB host controller started
	void			BootToMac (void);
	unsigned		m_nMipsX10 = 0;	// the last measured emulation speed
	u64			m_nLastInstructions = 0;
	static void SelfTestAttach (void *pContext, void *pWorker);	// CpuSelfTest::Setup::worker_attach
#ifdef G3_QEMU_DRAWTEST
	void DrawTest (void);
#endif
	void HandleMenuKey (u8 ucUsage);

	// The emulated Mac
	void StartMac (void);
	void StopMac (void);
	void RunSelfTest (void);
	void RunMac (void);
	void SaveMacState (boolean bForce);
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
	CScheduler		m_Scheduler;	// core 0 tasks: the network, the web server
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
	u8			*m_pBlocks;		// the CPU's block cache (with jit = yes)
	u8			*m_pSelfTestWork;	// memory for the CPU self-test
	boolean			m_bMacRunning;
	unsigned		m_nLastDrawTicks;
	unsigned		m_nLastStatsTicks;
	u64			m_nGuestMs;		// guest time run so far
	g3::MachineState	m_SavedState;		// as last written to the SD card
	g3::MachineState	m_CurrentState;		// scratch for comparing
	boolean			m_bStateValid = FALSE;	// m_SavedState holds a state
	unsigned		m_nLastStateTicks = 0;	// last check for changes

	// Drawing on core 1 (falls back to drawing here if it can't start).
	CDrawCore		m_DrawCore;
	g3::ScreenPresenter	*m_pPresenter = 0;
	g3::ScreenFrame		m_Frame;
	boolean			m_bDrawCore = FALSE;	// core 1 draws
	void			ResumeDrawing (void);

	static CKernel		*s_pThis;
};

#endif
