// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/circle/kernel.cpp — Milestone 1 kernel.
//
//  * Startup screen and host information (Milestone 0).
//  * Full ARM clock with temperature-based throttling.
//  * Log file on the SD card: SD:/g3macpi/logs/boot.log
//  * USB keyboard and mouse test: every key press/release and mouse
//    movement is shown using the same portable code (platform/common) that
//    will later feed the emulated Macintosh ADB keyboard and mouse.
//  * Ctrl+Alt+Delete reboots (the log file is rewritten first).

#include "kernel.h"

#include <circle/machineinfo.h>
#include <circle/memory.h>
#include <circle/string.h>
#include <circle/util.h>
#include <circle/version.h>

#include "core/common/bitops.h"
#include "core/common/endian.h"
#include "core/common/version.h"

static const char FromKernel[] = "g3macpi";

#define SD_DRIVE	"SD:"
#define LOG_DIR		SD_DRIVE "/g3macpi/logs"
#define LOG_FILE	LOG_DIR "/boot.log"
#define LOG_FILE_PREV	LOG_DIR "/boot-prev.log"

CKernel *CKernel::s_pThis = 0;

CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	m_Timer (&m_Interrupt),
	// Circle otherwise leaves the ARM cores at their minimum clock (600 MHz on
	// the Pi 400, observed on hardware 2026-11-16). Run at full speed; Run()
	// calls m_CPUThrottle.Update() regularly so Circle can reduce the clock
	// if the SoC gets hot.
	m_CPUThrottle (CPUSpeedMaximum),
	m_LogTee (&m_Screen, &m_Serial),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_USBHCI (&m_Interrupt, &m_Timer, TRUE),	// TRUE: USB plug-and-play
	m_EMMC (&m_Interrupt, &m_Timer, &m_ActLED),
	m_bSDMounted (FALSE),
	m_pKeyboard (0),
	m_pMouse (0),
	m_nDroppedEvents (0),
	m_nHeldModifiers (0),
	m_nMouseDX (0), m_nMouseDY (0), m_nMouseWheel (0),
	m_nMouseButtons (0),
	m_nMouseEvents (0),
	m_nLastMouseLogTicks (0),
	m_ShutdownMode (ShutdownNone),
	m_pMenu (0),
	m_bMenuShown (FALSE)
{
	s_pThis = this;
	m_ActLED.Blink (3);	// visible sign of life before the screen is up
}

CKernel::~CKernel (void)
{
	delete m_pMenu;
	s_pThis = 0;
}

boolean CKernel::Initialize (void)
{
	boolean bOK = TRUE;

	if (bOK)
	{
		bOK = m_Screen.Initialize ();
	}

	if (bOK)
	{
		bOK = m_Serial.Initialize (115200);
	}

	if (bOK)
	{
		// Screen first so the startup screen is the first thing on HDMI.
		ShowStartupScreen ();

		bOK = m_Logger.Initialize (&m_LogTee);
	}

	if (bOK)
	{
		bOK = m_Interrupt.Initialize ();
	}

	if (bOK)
	{
		bOK = m_Timer.Initialize ();
	}

	if (bOK)
	{
		// A missing/unreadable SD card is not fatal: we only lose the log file.
		if (!m_EMMC.Initialize ())
		{
			m_Logger.Write (FromKernel, LogWarning, "SD card (EMMC) init failed");
		}
	}

	if (bOK)
	{
		bOK = m_USBHCI.Initialize ();
	}

	return bOK;
}

void CKernel::ShowStartupScreen (void)
{
	// ANSI sequences supported by Circle's terminal: see doc/screen.txt.
	// Note: Circle renders "bold" (ESC[1m) in red, so it is not used.
	// All banner lines have the same length so the blue bar is a clean rectangle.
	static const char Banner[] =
		"\x1b[H\x1b[J"
		"\x1b[44m\x1b[97m"
		"                                                                \n"
		"   G3MacPi  -  PowerPC Macintosh G3 emulator                    \n"
		"   bare metal on Raspberry Pi 400                               \n"
		"                                                                \n"
		"\x1b[0m\n";

	m_Screen.Write (Banner, sizeof Banner - 1);

	// Plain-text banner for the UART (no colour codes).
	static const char SerialBanner[] =
		"\r\n==== " G3_PROJECT_NAME " - PowerPC Macintosh G3 emulator ====\r\n";
	m_Serial.Write (SerialBanner, sizeof SerialBanner - 1);

	CString Line;
	Line.Format ("Version %s (git %s), built " __DATE__ " " __TIME__ "\n\n",
		     G3_VERSION_STRING, G3_GIT_REV);
	m_Screen.Write ((const char *) Line, Line.GetLength ());
}

void CKernel::LogHostInfo (void)
{
	CMachineInfo *pInfo = CMachineInfo::Get ();

	m_Logger.Write (FromKernel, LogNotice, "%s %s (git %s)",
			G3_PROJECT_NAME, G3_VERSION_STRING, G3_GIT_REV);
	m_Logger.Write (FromKernel, LogNotice, "Framework: %s %s, AArch%u",
			CIRCLE_NAME, CIRCLE_VERSION_STRING, (unsigned) AARCH);
	m_Logger.Write (FromKernel, LogNotice, "Host: %s, SoC %s",
			pInfo->GetMachineName (), pInfo->GetSoCName ());
	m_Logger.Write (FromKernel, LogNotice, "Host RAM: %u MB, Circle heap: %u MB",
			pInfo->GetRAMSize (),
			(unsigned) (CMemorySystem::Get ()->GetMemSize () / (1024 * 1024)));
	m_Logger.Write (FromKernel, LogNotice, "ARM clock: %u MHz (max %u MHz), SoC %u C",
			m_CPUThrottle.GetClockRate () / 1000000,
			m_CPUThrottle.GetMaxClockRate () / 1000000,
			m_CPUThrottle.GetTemperature ());
	m_Logger.Write (FromKernel, LogNotice, "Screen: %ux%u",
			m_Screen.GetWidth (), m_Screen.GetHeight ());
}

// Runs a few core/ functions on the target CPU. The same functions are unit
// tested on Linux; this proves that the portable core builds under Circle's
// freestanding flags and gives identical answers on the Pi.
boolean CKernel::RunCoreSelfTest (void)
{
	unsigned char Buf[4];
	g3::store_be32 (Buf, 0x12345678u);

	boolean bOK =    g3::byteswap32 (0x12345678u) == 0x78563412u
		      && Buf[0] == 0x12 && Buf[3] == 0x78
		      && g3::load_be32 (Buf) == 0x12345678u
		      && g3::ppc_mask (16, 31) == 0x0000FFFFu
		      && g3::ppc_mask (31, 0) == 0x80000001u
		      && g3::rotl32 (0x12345678u, 8) == 0x34567812u
		      && g3::sign_extend16 (0x8000u) == -32768;

	// The HID tracker used for keyboard input.
	g3::HidKeyboardTracker Tracker;
	g3::KeyEvent Events[g3::HidKeyboardTracker::kMaxEventsPerReport];
	const u8 KeysA[6] = {0x04, 0, 0, 0, 0, 0};
	const u8 KeysB[6] = {0x05, 0, 0, 0, 0, 0};
	bOK = bOK && Tracker.update (0x02, KeysA, Events) == 2
		  && Events[0].usage == 0xE1 && Events[0].pressed
		  && Events[1].usage == 0x04 && Events[1].pressed
		  && Tracker.update (0x00, KeysB, Events) == 3
		  && Events[0].usage == 0xE1 && !Events[0].pressed
		  && Events[1].usage == 0x04 && !Events[1].pressed
		  && Events[2].usage == 0x05 && Events[2].pressed;

	m_Logger.Write (FromKernel, bOK ? LogNotice : LogError,
			"Core self-test: %s", bOK ? "PASSED" : "FAILED");

	return bOK;
}

boolean CKernel::MountSDCard (void)
{
	if (f_mount (&m_FileSystem, SD_DRIVE, 1) != FR_OK)
	{
		m_Logger.Write (FromKernel, LogWarning, "Cannot mount SD card; no log file");
		return FALSE;
	}

	// Create directories; FR_EXIST is fine.
	FRESULT Result = f_mkdir (SD_DRIVE "/g3macpi");
	if (Result == FR_OK || Result == FR_EXIST)
	{
		Result = f_mkdir (LOG_DIR);
	}
	if (Result != FR_OK && Result != FR_EXIST)
	{
		m_Logger.Write (FromKernel, LogWarning, "Cannot create %s (error %d)",
				LOG_DIR, (int) Result);
		return FALSE;
	}

	// Keep the previous run's log, which is useful after a crash.
	f_unlink (LOG_FILE_PREV);
	f_rename (LOG_FILE, LOG_FILE_PREV);

	m_bSDMounted = TRUE;
	return TRUE;
}

// Writes the logger's text buffer (the last 16 KB of log output) to the SD card.
void CKernel::WriteLogFile (void)
{
	if (!m_bSDMounted)
	{
		return;
	}

	static char Buffer[LOGGER_BUFSIZE];
	int nLength = m_Logger.Read (Buffer, sizeof Buffer, FALSE);	// FALSE: keep text
	if (nLength < 0)
	{
		nLength = 0;
	}

	FIL File;
	FRESULT Result = f_open (&File, LOG_FILE, FA_WRITE | FA_CREATE_ALWAYS);
	if (Result != FR_OK)
	{
		m_Logger.Write (FromKernel, LogWarning, "Cannot open %s (error %d)",
				LOG_FILE, (int) Result);
		return;
	}

	UINT nWritten = 0;
	Result = f_write (&File, Buffer, (UINT) nLength, &nWritten);
	f_close (&File);

	if (Result != FR_OK || nWritten != (UINT) nLength)
	{
		m_Logger.Write (FromKernel, LogWarning, "Writing %s failed (error %d)",
				LOG_FILE, (int) Result);
		return;
	}

	m_Logger.Write (FromKernel, LogNotice, "Log written to %s (%u bytes)",
			LOG_FILE, (unsigned) nWritten);
}

void CKernel::AttachInputDevices (void)
{
	if (m_pKeyboard == 0)
	{
		m_pKeyboard = (CUSBKeyboardDevice *) m_DeviceNameService.GetDevice ("ukbd1", FALSE);
		if (m_pKeyboard != 0)
		{
			m_pKeyboard->RegisterRemovedHandler (KeyboardRemovedHandler);
			// Raw mode only: we want every press/release, not characters.
			m_pKeyboard->RegisterKeyStatusHandlerRaw (KeyStatusHandlerRaw, FALSE);
			m_Logger.Write (FromKernel, LogNotice, "USB keyboard attached");
		}
	}

	if (m_pMouse == 0)
	{
		m_pMouse = (CMouseDevice *) m_DeviceNameService.GetDevice ("mouse1", FALSE);
		if (m_pMouse != 0)
		{
			m_pMouse->RegisterRemovedHandler (MouseRemovedHandler);
			m_pMouse->RegisterStatusHandler (MouseStatusHandler);
			m_Logger.Write (FromKernel, LogNotice, "USB mouse attached (%u buttons%s)",
					m_pMouse->GetButtonCount (),
					m_pMouse->HasWheel () ? ", wheel" : "");
		}
	}
}

void CKernel::QueueEvent (const g3::InputEvent &Event)
{
	if (!m_InputQueue.push (Event))
	{
		m_nDroppedEvents = m_nDroppedEvents + 1;
	}
}

void CKernel::KeyStatusHandlerRaw (unsigned char ucModifiers, const unsigned char RawKeys[6])
{
	CKernel *pThis = s_pThis;
	g3::KeyEvent Events[g3::HidKeyboardTracker::kMaxEventsPerReport];
	u32 nEvents = pThis->m_KeyTracker.update (ucModifiers, RawKeys, Events);
	for (u32 i = 0; i < nEvents; i++)
	{
		pThis->QueueEvent (g3::InputEvent::make_key (Events[i]));
	}
}

void CKernel::MouseStatusHandler (unsigned nButtons, int nDisplacementX, int nDisplacementY,
				  int nWheelMove)
{
	g3::MouseEvent Event;
	Event.dx = (s16) nDisplacementX;
	Event.dy = (s16) nDisplacementY;
	Event.wheel = (s8) nWheelMove;
	Event.buttons = (u8) nButtons;
	s_pThis->QueueEvent (g3::InputEvent::make_mouse (Event));
}

void CKernel::KeyboardRemovedHandler (CDevice *pDevice, void *pContext)
{
	// Release any held keys so nothing stays "stuck down".
	CKernel *pThis = s_pThis;
	g3::KeyEvent Events[g3::HidKeyboardTracker::kMaxEventsPerReport];
	u32 nEvents = pThis->m_KeyTracker.release_all (Events);
	for (u32 i = 0; i < nEvents; i++)
	{
		pThis->QueueEvent (g3::InputEvent::make_key (Events[i]));
	}
	pThis->m_pKeyboard = 0;
	CLogger::Get ()->Write (FromKernel, LogNotice, "USB keyboard removed");
}

void CKernel::MouseRemovedHandler (CDevice *pDevice, void *pContext)
{
	s_pThis->m_pMouse = 0;
	CLogger::Get ()->Write (FromKernel, LogNotice, "USB mouse removed");
}

void CKernel::FlushMouseLog (void)
{
	if (m_nMouseEvents == 0)
	{
		return;
	}

	m_Logger.Write (FromKernel, LogNotice,
			"Mouse: dx %d dy %d wheel %d  buttons L%c R%c M%c  (%u reports)",
			m_nMouseDX, m_nMouseDY, m_nMouseWheel,
			(m_nMouseButtons & 1) ? '*' : '-',
			(m_nMouseButtons & 2) ? '*' : '-',
			(m_nMouseButtons & 4) ? '*' : '-',
			m_nMouseEvents);

	m_nMouseDX = m_nMouseDY = m_nMouseWheel = 0;
	m_nMouseEvents = 0;
}

void CKernel::ProcessInputEvents (void)
{
	g3::InputEvent Event;
	while (m_InputQueue.pop (Event))
	{
		if (Event.type == g3::InputEvent::Type::Key)
		{
			const u8 Usage = Event.key.usage;
			if (Usage >= g3::hid::kUsageLeftCtrl && Usage <= g3::hid::kUsageLeftCtrl + 7)
			{
				unsigned nBit = 1u << (Usage - g3::hid::kUsageLeftCtrl);
				if (Event.key.pressed)
					m_nHeldModifiers |= nBit;
				else
					m_nHeldModifiers &= ~nBit;
			}

			// The settings menu takes key presses (F12 toggles it).
			if (Event.key.pressed && m_pMenu != 0)
			{
				if (Usage == g3::menu_key::kF12)
				{
					ShowMenu (!m_bMenuShown);
					continue;
				}
				if (m_bMenuShown && Usage != g3::hid::kUsageDeleteForward)
				{
					HandleMenuKey (Usage);
					continue;
				}
			}

			if (!m_bMenuShown)
			{
				m_Logger.Write (FromKernel, LogNotice, "Key %-4s usage 0x%02X  %s",
						Event.key.pressed ? "down" : "up", (unsigned) Usage,
						g3::hid::usage_name (Usage));
			}

			// Ctrl+Alt+Delete: reboot.
			const unsigned CtrlMask = (1u << 0) | (1u << 4);	// LCtrl, RCtrl
			const unsigned AltMask = (1u << 2) | (1u << 6);		// LAlt, RAlt
			if (   Event.key.pressed
			    && Usage == g3::hid::kUsageDeleteForward
			    && (m_nHeldModifiers & CtrlMask)
			    && (m_nHeldModifiers & AltMask))
			{
				m_Logger.Write (FromKernel, LogNotice, "Ctrl+Alt+Delete: rebooting");
				m_ShutdownMode = ShutdownReboot;
			}
		}
		else if (!m_bMenuShown)
		{
			// Mouse reports arrive at up to ~125-1000 per second: accumulate
			// and log a summary at most 5 times per second, or immediately
			// when a button changes.
			const g3::MouseEvent &M = Event.mouse;
			boolean bButtonsChanged = M.buttons != m_nMouseButtons;
			if (bButtonsChanged)
			{
				FlushMouseLog ();
			}
			m_nMouseDX += M.dx;
			m_nMouseDY += M.dy;
			m_nMouseWheel += M.wheel;
			m_nMouseButtons = M.buttons;
			m_nMouseEvents++;
			if (bButtonsChanged)
			{
				FlushMouseLog ();
			}
		}
	}

	unsigned nNow = m_Timer.GetTicks ();
	if (nNow - m_nLastMouseLogTicks >= HZ / 5)
	{
		FlushMouseLog ();
		m_nLastMouseLogTicks = nNow;
	}

	unsigned nDropped = m_nDroppedEvents;
	if (nDropped != 0)
	{
		m_nDroppedEvents = 0;	// a lost increment here only affects this warning
		m_Logger.Write (FromKernel, LogWarning, "%u input events dropped (queue full)",
				nDropped);
	}
}

void CKernel::SetUpSettings (void)
{
	g3::MachineConfig Config;
	if (m_bSDMounted)
	{
		CreateSettingsFolders ();
		g3::ConfigParseReport Report;
		if (LoadSettings (Config, Report))
		{
			m_Logger.Write (FromKernel, LogNotice, "Settings: %s, %u MB RAM",
					G3_CONFIG_FILE, Config.ram_mb);
			if (Report.ignored != 0)
			{
				m_Logger.Write (FromKernel, LogWarning,
						"Settings: %u line(s) ignored, first: line %u (%s)",
						Report.ignored, Report.first_line, Report.first_reason);
			}
		}
		else
		{
			m_Logger.Write (FromKernel, LogNotice, "Settings: no %s yet, using defaults",
					G3_CONFIG_FILE);
		}
	}

	m_pMenu = new g3::ConfigMenu (Config, m_Lister, G3_DISKS_DIR, G3_ROM_DIR);
	if (!m_bSDMounted)
	{
		m_pMenu->set_status ("No SD card file system: settings cannot be saved.");
	}

	ShowMenu (TRUE);
}

void CKernel::ShowMenu (boolean bShow)
{
	m_bMenuShown = bShow;
	m_LogTee.EnableFirst (!bShow);	// while the menu shows, the log goes to the UART only

	static const char Clear[] = "\x1b[0m\x1b[H\x1b[J";
	m_Screen.Write (Clear, sizeof Clear - 1);

	if (bShow)
	{
		g3::ConfigMenu::Screen Screen;
		m_pMenu->render (Screen);
		DrawSettings (&m_Screen, Screen);
	}
	else
	{
		static const char Back[] = "\x1b[?25hLog view. F12: settings menu.\n";
		m_Screen.Write (Back, sizeof Back - 1);
	}
}

void CKernel::HandleMenuKey (u8 ucUsage)
{
	switch (m_pMenu->key (ucUsage))
	{
	case g3::ConfigMenu::Action::Save:
		if (m_bSDMounted && SaveSettings (m_pMenu->config ()))
		{
			m_pMenu->mark_saved ();
			m_pMenu->set_status ("Settings saved to " G3_CONFIG_FILE);
			m_Logger.Write (FromKernel, LogNotice, "Settings saved");
		}
		else
		{
			m_pMenu->set_status ("Could not write " G3_CONFIG_FILE " on the SD card.");
		}
		break;

	case g3::ConfigMenu::Action::Start:
		// Not yet: the emulator core runs on the Linux host only so far.
		m_pMenu->set_status ("Starting the Mac comes with the next milestone (not in this build).");
		break;

	case g3::ConfigMenu::Action::MediaChanged:	// only while a Mac is running
	case g3::ConfigMenu::Action::None:
		break;
	}

	g3::ConfigMenu::Screen Screen;
	m_pMenu->render (Screen);
	DrawSettings (&m_Screen, Screen);
}

TShutdownMode CKernel::Run (void)
{
	LogHostInfo ();
	RunCoreSelfTest ();

	if (MountSDCard ())
	{
		WriteLogFile ();
	}

	m_Logger.Write (FromKernel, LogNotice,
			"Input test: press keys, move the mouse. F12: settings menu.");
	m_Logger.Write (FromKernel, LogNotice,
			"Ctrl+Alt+Delete saves the log and reboots.");

	SetUpSettings ();

	unsigned nLastThrottleTicks = m_Timer.GetTicks ();

	while (m_ShutdownMode == ShutdownNone)
	{
		// Must be called from TASK_LEVEL to update the tree of USB devices.
		if (m_USBHCI.UpdatePlugAndPlay ())
		{
			AttachInputDevices ();
		}

		ProcessInputEvents ();

		unsigned nNow = m_Timer.GetTicks ();
		if (nNow - nLastThrottleTicks >= 2 * HZ)
		{
			m_CPUThrottle.Update ();	// throttles if the SoC gets too hot
			nLastThrottleTicks = nNow;
		}

		// Sleep until the next interrupt (timer tick at 100 Hz or USB):
		// keeps core 0 cool while idle.
		asm volatile ("wfi");
	}

	WriteLogFile ();

	return m_ShutdownMode;
}
