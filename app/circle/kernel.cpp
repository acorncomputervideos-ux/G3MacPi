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
#include "ndrv_data.h"
#include "platform/common/cpu_selftest.h"
#include "platform/common/net_switch.h"

#include <circle/new.h>

static const char FromKernel[] = "g3macpi";

// Generated code for the JIT (docs/cpu/jit-a64.md). Circle maps everything
// after the end of .text (_etext) as not executable (PXN) and everything
// read/write, so this buffer is put into a section of its own, .text_g3jit (its
// linker script places all .text* before _etext): it is both. Defined in
// assembly so the section gets the right flags. It adds its size to the
// kernel image.
static const unsigned JitCodeWords = 512 * 1024;	// 2 MB
asm (".section .text_g3jit, \"awx\", %progbits\n"
     ".balign 65536\n"
     "g3_jit_code:\n"
     ".zero 2097152\n"
     ".previous\n");
extern "C" u32 g3_jit_code[];
#define s_JitCode g3_jit_code

// The CPU self-test's own code buffer (the Mac's may be in use), 512 KB.
static const unsigned SelfTestCodeWords = 128 * 1024;
asm (".section .text_g3jit, \"awx\", %progbits\n"
     ".balign 65536\n"
     "g3_selftest_code:\n"
     ".zero 524288\n"
     ".previous\n");
extern "C" u32 g3_selftest_code[];

static u64 SelfTestClock (void *)
{
	return CTimer::GetClockTicks64 ();	// 1 MHz (CLOCKHZ)
}
static const unsigned BlockCacheSize = 8 << 20;

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
	m_bMenuShown (FALSE),
	m_pRam (0),
	m_nRamSize (0),
	m_pVram (0),
	m_pPixels (0),
	m_pBlocks (0),
	m_pSelfTestWork (0),
	m_bMacRunning (FALSE),
	m_nLastDrawTicks (0),
	m_nLastStatsTicks (0),
	m_nGuestMs (0),
	m_DrawCore (CMemorySystem::Get ())
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
		// A missing/unreadable SD card is not fatal (no settings, images or
		// log file, though). Tried a few times: a card that did not answer
		// at once may after a pause and another reset of the controller.
		boolean bCard = FALSE;
		for (unsigned nTry = 1; nTry <= 3 && !bCard; nTry++)
		{
			bCard = m_EMMC.Initialize ();
			if (!bCard)
			{
				m_Logger.Write (FromKernel, LogWarning, "SD card (EMMC) init failed (try %u of 3)", nTry);
				CTimer::SimpleMsDelay (200);
			}
		}
		if (!bCard)
		{
			m_Logger.Write (FromKernel, LogError,
					"The SD card could not be read: no settings, disk images or log. "
					"Try another card (FAT32), or report the lines above.");
		}
	}

	if (bOK)
	{
#ifndef G3_QEMU_DRAWTEST	// QEMU's raspi4b has no Pi 4 USB controller
		bOK = m_USBHCI.Initialize ();
#endif
	}

	if (bOK)
	{
		StartDrawCore ();	// not fatal: without it, core 0 draws
	}

	return bOK;
}

// Core 1 draws the Mac's screen: the shadow copy of the drawn video memory
// (8 MB, as much as the Mac's VRAM) and the presenter come from the high heap.
void CKernel::StartDrawCore (void)
{
	CBcmFrameBuffer *pFrameBuffer = m_Screen.GetFrameBuffer ();
	const u32 nShadow = 8u << 20;
	u8 *pShadow = new (HEAP_HIGH) u8[nShadow];
	if (pFrameBuffer == 0 || pShadow == 0)
	{
		return;
	}
	m_pPresenter = new (HEAP_HIGH) g3::ScreenPresenter (pShadow, nShadow);
	if (m_pPresenter == 0)
	{
		return;
	}
	m_pPresenter->pause ();		// nothing to draw until the Mac runs
	g3::ScreenSurface Surface;
	Surface.base = (u8 *) (uintptr) pFrameBuffer->GetBuffer ();
	Surface.width = pFrameBuffer->GetWidth ();
	Surface.height = pFrameBuffer->GetHeight ();
	Surface.pitch = pFrameBuffer->GetPitch ();
	Surface.depth = pFrameBuffer->GetDepth ();
	m_DrawCore.Setup (m_pPresenter, Surface);
	if (!m_DrawCore.Initialize ())
	{
		m_Logger.Write (FromKernel, LogWarning, "Cannot start core 1: core 0 draws the screen");
		return;
	}
	for (unsigned i = 0; i < 100 && !m_DrawCore.m_bStarted; i++)
	{
		CTimer::SimpleMsDelay (1);
	}
	m_bDrawCore = m_DrawCore.m_bStarted;
	m_Logger.Write (FromKernel, LogNotice, "Screen drawing: %s",
			m_bDrawCore ? "core 1" : "core 0 (core 1 did not start)");
	for (unsigned i = 0; i < 100 && !m_DrawCore.m_bWorkerStarted; i++)
	{
		CTimer::SimpleMsDelay (1);
	}
	m_Logger.Write (FromKernel, LogNotice, "JIT compiling: %s",
			m_DrawCore.m_bWorkerStarted ? "core 2" : "core 0 (core 2 did not start)");
	for (unsigned i = 0; i < 100 && !m_DrawCore.m_bIoStarted; i++)
	{
		CTimer::SimpleMsDelay (1);
	}
	m_MacHost.SetDiskIoCore (&m_DrawCore);
	m_Logger.Write (FromKernel, LogNotice, "Disk image I/O: %s",
			m_DrawCore.m_bIoStarted ? "core 3" : "core 0 (core 3 did not start)");
	if (m_DrawCore.m_bIoStarted)
	{
		StartSoundAndNet ();
	}
}

// Sound on HDMI (Circle drives HDMI 0 only) and the Pi's Ethernet adapter,
// both served by core 3. Either may be missing; the Mac then has no sound
// or no network, and runs anyway.
void CKernel::StartSoundAndNet (void)
{
#ifndef G3_QEMU_DRAWTEST
	m_pHDMISound = new CHDMISoundBaseDevice (&m_Interrupt, SoundSampleRate, 384 * 10);
	if (m_pHDMISound != 0
	    && m_pHDMISound->AllocateQueueFrames (SoundSampleRate / 10)	// 100 ms
	    && m_pHDMISound->Start ())
	{
		m_pHDMISound->SetWriteFormat (SoundFormatSigned16, 2);
		m_MacHost.EnableSound ();
		m_DrawCore.AttachSound (m_MacHost.GetAudioBridge (), m_pHDMISound);
		m_Logger.Write (FromKernel, LogNotice, "Sound: HDMI, %u Hz, mixed on core 3", SoundSampleRate);
	}
	else
	{
		m_Logger.Write (FromKernel, LogWarning, "Sound: HDMI sound did not start; the Mac has no sound");
	}

#endif
}

// The network, once the settings are known: Circle's TCP/IP stack (DHCP)
// on CPiNetPort, the adapter served by core 3 as a switch between the Pi's
// stack and the Mac, and the web interface (app/circle/web.h). Without
// core 3 there is no network.
void CKernel::StartNetwork (void)
{
#ifndef G3_QEMU_DRAWTEST	// QEMU's raspi4b has no Pi 4 Ethernet adapter
	const g3::MachineConfig &Config = m_pMenu->config ();
	if (!m_DrawCore.m_bIoStarted || (!Config.network && !Config.web))
	{
		m_Logger.Write (FromKernel, LogNotice, "Network: off");
		return;
	}
	m_PiPort.Register ();	// first: the stack takes it
	m_pNet = new CNetSubSystem (0, 0, 0, 0, "g3macpi");
	if (m_pNet == 0 || !m_pNet->Initialize (FALSE))	// FALSE: don't wait for a link or DHCP
	{
		m_Logger.Write (FromKernel, LogWarning, "Network: the Ethernet adapter did not start");
		return;
	}
	CNetDevice *pAdapter = 0;
	for (unsigned i = 0; CNetDevice::GetNetDevice (i) != 0; i++)
	{
		CNetDevice *pDevice = CNetDevice::GetNetDevice (i);
		if (pDevice != &m_PiPort && pDevice->GetType () == NetDeviceTypeEthernet)
		{
			pAdapter = pDevice;
			break;
		}
	}
	if (pAdapter == 0)
	{
		m_Logger.Write (FromKernel, LogWarning, "Network: no Ethernet adapter");
		return;
	}
	m_PiPort.SetAdapter (pAdapter);
	const u8 *pPiAddress = pAdapter->GetMACAddress ()->Get ();
	u8 MacAddress[6];
	g3::net_switch::mac_address_for (pPiAddress, MacAddress);
	// The adapter's filter: the Pi's address and broadcasts (Circle), and
	// as extra entries the Mac's address and AppleTalk's broadcast.
	static u8 Groups[3][MAC_ADDRESS_SIZE] = {{0}, {0x09, 0x00, 0x07, 0xFF, 0xFF, 0xFF}, {0}};
	memcpy (Groups[0], MacAddress, 6);
	pAdapter->SetMulticastFilter (Groups);
	if (Config.network)
	{
		m_MacHost.EnableNet (MacAddress);
	}
	m_DrawCore.AttachNet (pAdapter, Config.network ? m_MacHost.GetNetBridge () : 0, MacAddress,
			      Config.web ? m_PiPort.Bridge () : 0, pPiAddress);
	m_Logger.Write (FromKernel, LogNotice,
			"Network: core 3 switches the adapter between the Pi (%02X:%02X:%02X:%02X:%02X:%02X) "
			"and the Mac (%02X:%02X:%02X:%02X:%02X:%02X)",
			pPiAddress[0], pPiAddress[1], pPiAddress[2], pPiAddress[3], pPiAddress[4], pPiAddress[5],
			MacAddress[0], MacAddress[1], MacAddress[2], MacAddress[3], MacAddress[4], MacAddress[5]);
	if (Config.web)
	{
		CCircleWebHost::TKernelHooks Hooks = {this, WebStatus, WebInUse, WebPassword, WebReboot,
						      WebConfigChanged, WebInsertMedia};
		m_pWebHost = new CCircleWebHost (Hooks);
		new CWebServer (m_pNet, m_pWebHost);
	}
#endif
}

// Once DHCP has given the Pi an address: show the web interface's address
// in the menu and the log.
void CKernel::CheckWebAddress (void)
{
	if (m_bWebAddressShown || m_pNet == 0 || m_pWebHost == 0 || !m_pNet->IsRunning ())
	{
		return;
	}
	m_bWebAddressShown = TRUE;
	CString Address;
	m_pNet->GetConfig ()->GetIPAddress ()->Format (&Address);
	CString Status;
	Status.Format ("Web interface: http://%s/", (const char *) Address);
	m_Logger.Write (FromKernel, LogNotice, "%s", (const char *) Status);
	m_pMenu->set_status (Status);
	if (m_bMenuShown)
	{
		ShowMenu (TRUE);
	}
}

void CKernel::WebStatus (void *pContext, g3::WebStatus &Out)
{
	CKernel *pThis = (CKernel *) pContext;
	Out.version = G3_VERSION_STRING " (" G3_GIT_REV ")";
	Out.uptime_s = pThis->m_Timer.GetUptime ();
	Out.mac_state = !pThis->m_bMacRunning ? "stopped (settings menu)"
			: pThis->m_bMenuShown ? "paused (settings menu)" : "running";
	Out.guest_mips_x10 = pThis->m_bMacRunning ? pThis->m_nMipsX10 : 0;
	if (pThis->m_pNet != 0 && pThis->m_pNet->IsRunning ())
	{
		CString Address;
		pThis->m_pNet->GetConfig ()->GetIPAddress ()->Format (&Address);
		strncpy (Out.ip, Address, sizeof Out.ip - 1);
	}
	memcpy (Out.host_mac, pThis->m_PiPort.GetMACAddress ()->Get (), 6);
	pThis->m_MacHost.mac_address (Out.mac_mac);
	Out.sound = pThis->m_pMenu->config ().sound;
	Out.network = pThis->m_pMenu->config ().network;
	const g3::MachineConfig &Config = pThis->m_pMenu->config ();
	for (unsigned i = 0; i < g3::MachineConfig::kDrives && i < 4; i++)
	{
		Out.drives[i].label = g3::MachineConfig::drive_label (i);
		Out.drives[i].type = g3::MachineConfig::type_name (Config.drive[i].type);
		Out.drives[i].image = Config.drive[i].image;
	}
}

// A disc into a CD-ROM drive from the web page: as in the menu's drive
// page, at once in a running Mac.
u32 CKernel::WebInsertMedia (void *pContext, u32 nDrive, const char *pPath)
{
	CKernel *pThis = (CKernel *) pContext;
	boolean bOK = FALSE;
	const g3::ConfigMenu::Action Action = pThis->m_pMenu->insert_media (nDrive, pPath, bOK);
	if (!bOK)
	{
		return 1;
	}
	if (Action == g3::ConfigMenu::Action::MediaChanged && !pThis->m_Session.change_disc (nDrive, pPath))
	{
		return 2;
	}
	pThis->m_pMenu->set_status (pPath[0] ? "Disc inserted from the web page." : "Disc ejected from the web page.");
	if (pThis->m_bMenuShown)
	{
		pThis->ShowMenu (TRUE);
	}
	return 0;
}

// An image the running Mac has open (as set in the menu) can't be replaced.
boolean CKernel::WebInUse (void *pContext, const char *pPath)
{
	CKernel *pThis = (CKernel *) pContext;
	if (!pThis->m_bMacRunning)
	{
		return FALSE;
	}
	const g3::MachineConfig &Config = pThis->m_pMenu->config ();
	for (unsigned i = 0; i < g3::MachineConfig::kDrives; i++)
	{
		if (Config.drive[i].image[0] != '\0' && strcmp (Config.drive[i].image, pPath) == 0)
		{
			return TRUE;
		}
	}
	return FALSE;
}

const char *CKernel::WebPassword (void *pContext)
{
	return ((CKernel *) pContext)->m_pMenu->config ().web_password;
}

// As Ctrl+Alt+Delete: the main loop stops the Mac (saving its state and
// flushing its disks) and the Pi restarts.
void CKernel::WebReboot (void *pContext)
{
	((CKernel *) pContext)->m_ShutdownMode = ShutdownReboot;
}

// The settings file was replaced: with the Mac stopped the menu takes the
// new settings; while it runs they apply at its next start (from the file).
void CKernel::WebConfigChanged (void *pContext)
{
	CKernel *pThis = (CKernel *) pContext;
	if (pThis->m_bMacRunning)
	{
		pThis->m_pMenu->set_status ("Settings changed on the web: they apply when the Mac next starts.");
		return;
	}
	g3::MachineConfig Config;
	g3::ConfigParseReport Report;
	if (!LoadSettings (Config, Report))
	{
		return;
	}
	g3::ConfigMenu *pOld = pThis->m_pMenu;
	pThis->m_pMenu = new g3::ConfigMenu (Config, pThis->m_Lister, G3_DISKS_DIR, G3_ROM_DIR);
	pThis->m_pMenu->set_media_folders (G3_CD_DIR, G3_HDD_DIR);
	pThis->m_pMenu->set_status ("Settings changed on the web.");
	delete pOld;
	if (pThis->m_bMenuShown)
	{
		pThis->ShowMenu (TRUE);
	}
}

void CDrawCore::AttachWorker (g3::ppc::JitWorker *pWorker)
{
	__atomic_store_n (&m_pWorker, pWorker, __ATOMIC_SEQ_CST);
	asm volatile ("sev");		// core 2 may be waiting in WFE
}

void CDrawCore::DetachWorker (void)
{
	__atomic_store_n (&m_pWorker, (g3::ppc::JitWorker *) 0, __ATOMIC_SEQ_CST);
	while (__atomic_load_n (&m_nWorkerBusy, __ATOMIC_SEQ_CST))
	{
		// core 2 is finishing a poll()
	}
}

// Core 2: compile what the Mac's core queues; sleep in WFE when there is
// nothing to do (the CPU executes SEV after each request, AttachWorker too).
void CDrawCore::RunWorker (void)
{
	m_bWorkerStarted = TRUE;
	for (;;)
	{
		__atomic_store_n (&m_nWorkerBusy, 1u, __ATOMIC_SEQ_CST);
		g3::ppc::JitWorker *pWorker = __atomic_load_n (&m_pWorker, __ATOMIC_SEQ_CST);
		const u32 nDone = pWorker != 0 ? pWorker->poll () : 0;
		__atomic_store_n (&m_nWorkerBusy, 0u, __ATOMIC_SEQ_CST);
		if (nDone == 0)
		{
			asm volatile ("wfe");
		}
	}
}

void CDrawCore::AttachDisk (g3::DiskCache *pCache)
{
	for (unsigned i = 0; i < MaxDisks; i++)
	{
		g3::DiskCache *pNone = 0;
		if (__atomic_compare_exchange_n (&m_pDisk[i], &pNone, pCache, false, __ATOMIC_SEQ_CST,
						 __ATOMIC_SEQ_CST))
		{
			asm volatile ("sev");
			return;
		}
	}
}

void CDrawCore::DetachDisk (g3::DiskCache *pCache)
{
	for (unsigned i = 0; i < MaxDisks; i++)
	{
		g3::DiskCache *pThis = pCache;
		__atomic_compare_exchange_n (&m_pDisk[i], &pThis, (g3::DiskCache *) 0, false, __ATOMIC_SEQ_CST,
					     __ATOMIC_SEQ_CST);
	}
	while (__atomic_load_n (&m_nIoBusy, __ATOMIC_SEQ_CST))
	{
		// core 3 is finishing a pass over the disks
	}
}

void CDrawCore::AttachSound (g3::AudioBridge *pBridge, CSoundBaseDevice *pDevice)
{
	m_pSound = pDevice;
	__atomic_store_n (&m_pAudio, pBridge, __ATOMIC_SEQ_CST);
	asm volatile ("sev");
}

void CDrawCore::AttachNet (CNetDevice *pAdapter, g3::NetBridge *pMac, const u8 *pMacAddress,
			   g3::NetBridge *pPi, const u8 *pPiAddress)
{
	m_pMacNet = pMac;
	m_pPiNet = pPi;
	memcpy (m_MacAddress, pMacAddress, 6);
	memcpy (m_PiAddress, pPiAddress, 6);
	__atomic_store_n (&m_pNetDevice, pAdapter, __ATOMIC_SEQ_CST);
	asm volatile ("sev");
}

// The HDMI sound queue: kept filled with the Mac's sound, resampled to the
// device's 48 kHz (silence when the Mac plays nothing). Returns the chunks
// written.
u32 CDrawCore::ServiceSound (void)
{
	g3::AudioBridge *pBridge = __atomic_load_n (&m_pAudio, __ATOMIC_SEQ_CST);
	if (pBridge == 0)
	{
		return 0;
	}
	static const unsigned Chunk = 256;
	static s16 Buffer[2 * Chunk];
	u32 nDone = 0;
	while (nDone < 8
	       && m_pSound->GetQueueSizeFrames () - m_pSound->GetQueueFramesAvail () >= Chunk)
	{
		pBridge->render (Buffer, Chunk, SoundSampleRate);
		m_pSound->Write (Buffer, sizeof Buffer);
		nDone++;
	}
	return nDone;
}

// Ethernet, a switch with two stations (platform/common/net_switch.h):
// frames in from the adapter (polled; Circle's driver uses no receive
// interrupt) go to the Mac, to the Pi's stack or to both, by destination;
// frames from either go out. The PHY is checked every 2 seconds (as
// Circle's own network task does), so the link follows the cable.
u32 CDrawCore::ServiceNet (void)
{
	CNetDevice *pAdapter = __atomic_load_n (&m_pNetDevice, __ATOMIC_SEQ_CST);
	if (pAdapter == 0)
	{
		return 0;
	}
	static u8 Frame[FRAME_BUFFER_SIZE];
	u32 nDone = 0;
	unsigned nLength;
	for (unsigned i = 0; i < 8 && pAdapter->ReceiveFrame (Frame, &nLength); i++)
	{
		const u32 nTo = g3::net_switch::route (Frame, nLength, m_MacAddress, m_PiAddress);
		if ((nTo & g3::net_switch::kToMac) && m_pMacNet != 0)
		{
			m_pMacNet->put_incoming (Frame, nLength);
		}
		if ((nTo & g3::net_switch::kToHost) && m_pPiNet != 0)
		{
			m_pPiNet->put_incoming (Frame, nLength);
		}
		nDone++;
	}
	g3::NetBridge *const Stations[2] = {m_pMacNet, m_pPiNet};
	for (g3::NetBridge *pStation : Stations)
	{
		for (unsigned i = 0; pStation != 0 && i < 8 && pAdapter->IsSendFrameAdvisable (); i++)
		{
			const u32 n = pStation->take_outgoing (Frame);
			if (n == 0)
			{
				break;
			}
			pAdapter->SendFrame (Frame, n);
			nDone++;
		}
	}
	const unsigned nNow = CTimer::GetClockTicks ();
	if (nNow - m_nLastPHYTicks >= 2 * CLOCKHZ)
	{
		m_nLastPHYTicks = nNow;
		pAdapter->UpdatePHY ();
	}
	return nDone;
}

// Core 3: serve the attached disk caches, the sound and the network; sleep
// in WFE when idle. The caches execute SEV after each request; for sound and
// the network, which nobody signals, the generic timer's event stream wakes
// WFE regularly: an event each time bit 12 of the counter changes from 0 to
// 1 (CNTKCTL_EL1 EVNTEN, bit 2, and EVNTI, bits 7:4; Arm ARM, "Event
// streams"), every 2^13 counts: 152 us at the Pi 4's 54 MHz counter.
void CDrawCore::RunDiskIo (void)
{
	u64 nCtl;
	asm volatile ("mrs %0, cntkctl_el1" : "=r" (nCtl));
	nCtl = (nCtl & ~0xF8ull) | (12u << 4) | (1u << 2);
	asm volatile ("msr cntkctl_el1, %0" :: "r" (nCtl));
	asm volatile ("isb");
	m_bIoStarted = TRUE;
	for (;;)
	{
		__atomic_store_n (&m_nIoBusy, 1u, __ATOMIC_SEQ_CST);
		u32 nDone = 0;
		for (unsigned i = 0; i < MaxDisks; i++)
		{
			g3::DiskCache *pCache = __atomic_load_n (&m_pDisk[i], __ATOMIC_SEQ_CST);
			if (pCache != 0)
			{
				nDone += pCache->service ();
			}
		}
		__atomic_store_n (&m_nIoBusy, 0u, __ATOMIC_SEQ_CST);
		nDone += ServiceSound ();
		nDone += ServiceNet ();
		if (nDone == 0)
		{
			asm volatile ("wfe");
		}
	}
}

// Core 1 draws about 60 times a second; core 2 compiles; core 3 does disk I/O.
void CDrawCore::Run (unsigned nCore)
{
	if (nCore == 2)
	{
		RunWorker ();
		return;
	}
	if (nCore == 3)
	{
		RunDiskIo ();
		return;
	}
	if (nCore != 1 || m_pPresenter == 0)
	{
		return;
	}
	m_bStarted = TRUE;
	const u64 nPeriod = CLOCKHZ / 60;		// in system timer ticks (1 MHz)
	u64 nNext = CTimer::GetClockTicks64 ();
	for (;;)
	{
		m_pPresenter->render (m_Surface);
		nNext += nPeriod;
		u64 nNow = CTimer::GetClockTicks64 ();
		if (nNow > nNext + nPeriod)	// fell behind: don't try to catch up
		{
			nNext = nNow;
		}
		while ((nNow = CTimer::GetClockTicks64 ()) < nNext)
		{
			asm volatile ("yield");
		}
	}
}

// Core 1 may draw again (the menu is gone): it redraws everything once.
void CKernel::ResumeDrawing (void)
{
	if (m_bDrawCore && m_bMacRunning && !m_bMenuShown)
	{
		m_Session.screen_frame (m_Frame);
		m_pPresenter->publish (m_Frame);
		m_pPresenter->resume ();
	}
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
	// Core 0's start-up stack (KERNEL_STACK_SIZE below MEM_KERNEL_STACK): how
	// much is in use here, deep in nothing yet. A kernel object on it once
	// overflowed it (see main.cpp).
	{
		u64 nSP;
		asm volatile ("mov %0, sp" : "=r" (nSP));
		m_Logger.Write (FromKernel, LogNotice, "Core 0 stack: %u of %u KB in use",
				(unsigned) ((MEM_KERNEL_STACK - nSP) / 1024), (unsigned) (KERNEL_STACK_SIZE / 1024));
	}

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

			if (m_bMacRunning && !m_bMenuShown)
			{
				m_Session.key (Usage, Event.key.pressed);	// to the Mac's ADB keyboard
			}
			else if (!m_bMenuShown)
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
		else if (m_bMacRunning && !m_bMenuShown)
		{
			m_Session.mouse (Event.mouse.dx, Event.mouse.dy, Event.mouse.buttons);
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
	m_pMenu->set_media_folders (G3_CD_DIR, G3_HDD_DIR);
	if (!m_bSDMounted)
	{
		m_pMenu->set_status ("No SD card file system: settings cannot be saved.");
	}

	ShowMenu (TRUE);
}

void CKernel::ShowMenu (boolean bShow)
{
	if (m_pPresenter != 0)
	{
		m_pPresenter->pause ();	// core 1 keeps off the screen while we write to it
	}
	m_bMenuShown = bShow;
	// While the menu or the Mac shows, the log goes to the UART only.
	m_LogTee.EnableFirst (!bShow && !m_bMacRunning);

	static const char Clear[] = "\x1b[0m\x1b[H\x1b[J";
	m_Screen.Write (Clear, sizeof Clear - 1);

	if (bShow)
	{
		g3::ConfigMenu::Screen Screen;
		m_pMenu->render (Screen);
		DrawSettings (&m_Screen, Screen);
	}
	else if (m_bMacRunning)
	{
		static const char Hide[] = "\x1b[?25l";
		m_Screen.Write (Hide, sizeof Hide - 1);
		m_nLastDrawTicks = 0;	// redraw the Mac's screen at once
		ResumeDrawing ();
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
		if (!m_bMacRunning || m_pMenu->needs_restart ())
		{
			StartMac ();		// shows the Mac on success, else a status line
		}
		else
		{
			ShowMenu (FALSE);	// resume
		}
		if (!m_bMenuShown)
		{
			return;
		}
		break;

	case g3::ConfigMenu::Action::MediaChanged: {
		const u32 nDrive = m_pMenu->media_changed_drive ();
		const char *pImage = m_pMenu->config ().drive[nDrive].image;
		if (!m_Session.change_disc (nDrive, pImage))
		{
			m_pMenu->set_status ("Could not open that disc image.");
		}
		else
		{
			m_pMenu->set_status (pImage[0] ? "Disc inserted." : "Disc ejected.");
		}
		break; }

	case g3::ConfigMenu::Action::SelfTest:
		RunSelfTest ();
		break;

	case g3::ConfigMenu::Action::CreateDisk: {
		// Show "Creating the disk..." first: allocating a few GB takes a while.
		g3::ConfigMenu::Screen Screen;
		m_pMenu->render (Screen);
		DrawSettings (&m_Screen, Screen);
		const char *pMessage = "";
		const boolean bOK = m_bSDMounted
				    && CreateBlankDisk (m_pMenu->new_disk_path (), m_pMenu->new_disk_bytes (), &pMessage);
		if (!m_bSDMounted)
		{
			pMessage = "No SD card file system.";
		}
		m_Logger.Write (FromKernel, bOK ? LogNotice : LogWarning, "New disk %s (%u MB): %s",
				m_pMenu->new_disk_path (), (unsigned) (m_pMenu->new_disk_bytes () >> 20),
				bOK ? "created" : pMessage);
		m_pMenu->disk_created (bOK, bOK ? 0 : pMessage);
		break; }

	case g3::ConfigMenu::Action::None:
		break;
	}

	g3::ConfigMenu::Screen Screen;
	m_pMenu->render (Screen);
	DrawSettings (&m_Screen, Screen);
}

void CKernel::RunSelfTest (void)
{
	// docs/cpu/selftest.md. The Mac (if running) waits while the menu shows.
	m_pMenu->set_status ("CPU self-test running (about 10 seconds)...");
	{
		g3::ConfigMenu::Screen Screen;
		m_pMenu->render (Screen);
		DrawSettings (&m_Screen, Screen);
	}
	if (m_pSelfTestWork == 0)
	{
		m_pSelfTestWork = new (HEAP_HIGH) u8[g3::CpuSelfTest::kWorkBytes];
	}
	if (m_pSelfTestWork == 0)
	{
		m_pMenu->set_status ("CPU self-test: not enough memory.");
		return;
	}
	g3::CpuSelfTest::Setup Setup;
	Setup.work = m_pSelfTestWork;
	Setup.code = g3_selftest_code;
	Setup.code_words = SelfTestCodeWords;
	Setup.clock_us = SelfTestClock;
	if (m_DrawCore.m_bWorkerStarted)
	{
		// Compile on core 2, as the Mac does; the running Mac's own worker
		// is handed back afterwards (the Mac is paused meanwhile).
		m_DrawCore.DetachWorker ();
		Setup.worker_attach = SelfTestAttach;
		Setup.worker_context = this;
	}
	m_Logger.Write (FromKernel, LogNotice, "CPU self-test: %u random programs, then the speed benchmark",
			Setup.programs);
	const u64 nStart = CTimer::GetClockTicks64 ();
	const g3::CpuSelfTest::Result Result = g3::CpuSelfTest::run (Setup);
	const unsigned nMs = (unsigned) ((CTimer::GetClockTicks64 () - nStart) / 1000);
	if (m_bMacRunning)
	{
		m_DrawCore.AttachWorker (m_Session.jit_worker ());
	}

	char Line1[160], Line2[160];
	g3::CpuSelfTest::describe (Result, Line1, sizeof Line1, Line2, sizeof Line2);
	m_Logger.Write (FromKernel, Result.ok () ? LogNotice : LogError, "%s", Line1);
	m_Logger.Write (FromKernel, LogNotice, "%s", Line2);
	m_Logger.Write (FromKernel, LogNotice, "CPU self-test: %u instructions compared, %u block runs in generated code, %u ms",
			(unsigned) Result.instructions, (unsigned) Result.jit_block_runs, nMs);
	WriteLogFile ();

	CString Status;
	Status.Format ("%s %s", Result.ok () ? "Self-test PASS." : "Self-test FAIL (see log).", Line2);
	m_pMenu->set_status (Status);
}

void CKernel::SelfTestAttach (void *pContext, void *pWorker)
{
	CKernel *pThis = static_cast<CKernel *> (pContext);
	if (pWorker != 0)
	{
		pThis->m_DrawCore.AttachWorker (static_cast<g3::ppc::JitWorker *> (pWorker));
	}
	else
	{
		pThis->m_DrawCore.DetachWorker ();
	}
}

void CKernel::StartMac (void)
{
	StopMac ();
	const g3::MachineConfig &Config = m_pMenu->config ();

	// Memory: the Mac's RAM, video memory and a screen conversion buffer,
	// above 1 GB where there is room (HEAP_HIGH returns 0 when full).
	const u32 nRamSize = Config.ram_mb << 20;
	if (m_pRam == 0 || m_nRamSize != nRamSize)
	{
		delete [] m_pRam;
		m_pRam = new (HEAP_HIGH) u8[nRamSize];
		m_nRamSize = m_pRam != 0 ? nRamSize : 0;
	}
	const u32 nVramSize = 8u << 20;
	if (m_pVram == 0)
	{
		m_pVram = new (HEAP_HIGH) u8[nVramSize];
	}
	if (m_pPixels == 0)
	{
		m_pPixels = new (HEAP_HIGH) u32[1600 * 1200];
	}
	if (m_pRam == 0 || m_pVram == 0 || m_pPixels == 0)
	{
		m_pMenu->set_status ("Not enough memory for that much Mac RAM.");
		return;
	}
	memset (m_pRam, 0, nRamSize);
	memset (m_pVram, 0, nVramSize);

	// What the Mac kept from its last run (clock, NVRAM, PRAM): a real Mac
	// keeps them powered by its battery.
	m_bStateValid = m_bSDMounted && LoadMachineState (m_SavedState);
	m_Logger.Write (FromKernel, LogNotice, "Saved clock and NVRAM: %s",
			m_bStateValid ? "restored" : "none (first start)");

	// The Pi 400 has no battery clock: the time is unknown (before 2020)
	// unless something set it. Then the Mac's clock carries on from where it
	// was when it last stopped (it lags by the time the Pi was off), or
	// starts in 2001 the first time.
	const u64 nTime = g3::choose_start_time (m_Timer.GetUniversalTime (), 1577836800u,
						 m_bStateValid ? &m_SavedState : 0, 978307200u);

	g3::MacSession::Memory Memory;
	Memory.ram = m_pRam;
	Memory.ram_size = nRamSize;
	Memory.vram = m_pVram;
	Memory.vram_size = nVramSize;
	if (Config.jit)
	{
		if (m_pBlocks == 0)
		{
			m_pBlocks = new (HEAP_HIGH) u8[BlockCacheSize];
		}
		Memory.blocks = m_pBlocks;
		Memory.blocks_size = m_pBlocks != 0 ? BlockCacheSize : 0;
		Memory.code = s_JitCode;
		Memory.code_words = JitCodeWords;
		Memory.jit_background = m_DrawCore.m_bWorkerStarted;	// compile on core 2
	}
	m_MacHost.SetEjectHandler (DiscEjected, this);

	m_Logger.Write (FromKernel, LogNotice, "Starting the Mac: %u MB, ROM %s", Config.ram_mb, Config.rom);
	const g3::MacSession::Error Error = m_Session.start (Config, m_MacHost, Memory, g_NdrvData, g_NdrvSize, nTime,
							     m_bStateValid ? &m_SavedState : 0);
	if (Error != g3::MacSession::Error::None)
	{
		CString Status;
		Status.Format ("Cannot start: %s%s%s", g3::MacSession::error_text (Error),
			       m_Session.detail ()[0] ? ": " : "", m_Session.detail ());
		m_Logger.Write (FromKernel, LogError, "%s", (const char *) Status);
		m_pMenu->set_status (Status);
		return;
	}
	m_DrawCore.AttachWorker (m_Session.jit_worker ());	// nullptr: compiling here
	m_Logger.Write (FromKernel, LogNotice, "CPU: %s%s", m_Session.jit_on () ? "block cache and generated code"
			: m_Session.block_cache_on () ? "block cache" : "interpreter",
			m_Session.jit_worker () != 0 ? " (compiled on core 2)" : "");
	if (g_NdrvSize == 0)
	{
		m_Logger.Write (FromKernel, LogWarning, "No display driver built in: Mac OS will not draw");
	}

	m_bMacRunning = TRUE;
	m_nGuestMs = 0;
	m_nLastInstructions = 0;
	m_nLastStatsTicks = m_Timer.GetTicks ();
	m_nLastStateTicks = m_nLastStatsTicks;
	m_pMenu->set_running (TRUE);
	m_pMenu->set_status ("");
	ShowMenu (FALSE);
}

// Writes the Mac's clock, NVRAM and PRAM to the SD card: always when
// bForce, otherwise only if NVRAM or PRAM changed or the clock moved on by
// five minutes or more since the last write (to spare the card).
void CKernel::SaveMacState (boolean bForce)
{
	if (!m_bSDMounted || !m_Session.capture_state (m_CurrentState))
	{
		return;
	}
	if (!bForce && m_bStateValid
	    && memcmp (m_CurrentState.nvram, m_SavedState.nvram, sizeof m_SavedState.nvram) == 0
	    && memcmp (m_CurrentState.pram, m_SavedState.pram, sizeof m_SavedState.pram) == 0
	    && m_CurrentState.rtc_1904 - m_SavedState.rtc_1904 < 5 * 60)
	{
		return;
	}
	if (SaveMachineState (m_CurrentState))
	{
		m_SavedState = m_CurrentState;
		m_bStateValid = TRUE;
	}
	else
	{
		m_Logger.Write (FromKernel, LogWarning, "Cannot write " G3_STATE_FILE);
	}
}

void CKernel::StopMac (void)
{
	if (m_bMacRunning)
	{
		if (m_pPresenter != 0)
		{
			m_pPresenter->pause ();	// before the Mac's video memory changes hands
		}
		SaveMacState (TRUE);
		m_DrawCore.DetachWorker ();	// core 2 lets go of the Mac's JIT first
		m_Session.stop ();	// closes (and flushes) the images
		m_bMacRunning = FALSE;
		m_pMenu->set_running (FALSE);
	}
}

void CKernel::DiscEjected (void *pContext, u32 nDrive)
{
	CKernel *pThis = static_cast<CKernel *> (pContext);
	pThis->m_pMenu->disc_ejected (nDrive);
	pThis->m_Logger.Write (FromKernel, LogNotice, "The Mac ejected the disc in drive %u", nDrive);
}

void CKernel::RunMac (void)
{
	// 2 ms of guest time per pass: on the Pi that takes several times longer
	// in real time, and input, USB plug-and-play and the screen are served
	// between passes.
	m_Session.run (2ull * 1000000000ull);
	m_nGuestMs += 2;

	if (m_Session.powered_off () || m_Session.halted ())
	{
		const boolean bHalted = m_Session.halted ();
		StopMac ();
		m_pMenu->set_status (bHalted ? "The emulated CPU stopped (checkstop); see the log."
					     : "The Mac was shut down.");
		ShowMenu (TRUE);
		return;
	}
	if (m_Session.reboot_requested ())
	{
		StartMac ();
		return;
	}

	unsigned nNow = m_Timer.GetTicks ();
	if (m_bDrawCore)
	{
		// Core 1 draws; hand it the display's current state (cheap).
		m_Session.screen_frame (m_Frame);
		m_pPresenter->publish (m_Frame);
	}
	else if (nNow - m_nLastDrawTicks >= HZ / 25)	// up to 25 frames per second
	{
		DrawMacScreen (&m_Screen, m_Session, m_pPixels);
		m_nLastDrawTicks = nNow;
	}
	if (nNow - m_nLastStateTicks >= 60 * HZ)	// NVRAM, PRAM, clock to the SD card
	{
		SaveMacState (FALSE);
		m_nLastStateTicks = nNow;
	}
	if (nNow - m_nLastStatsTicks >= 10 * HZ)	// speed, to the log (UART)
	{
		const unsigned nClock = m_CPUThrottle.GetClockRate () / 1000000;
		const unsigned nMaxClock = m_CPUThrottle.GetMaxClockRate () / 1000000;
		m_Logger.Write (FromKernel, LogNotice, "Mac: %u ms guest time, %u ms real, %u M instructions; "
				"ARM %u MHz, SoC %u C",
				(unsigned) m_nGuestMs, (unsigned) ((nNow - m_nLastStatsTicks) * (1000 / HZ)),
				(unsigned) (m_Session.instructions () / 1000000), nClock,
				m_CPUThrottle.GetTemperature ());
		if (nClock < nMaxClock)
		{
			m_Logger.Write (FromKernel, LogWarning, "The ARM clock is reduced to %u MHz (max %u): "
					"the SoC is too hot (socmaxtemp in cmdline.txt)", nClock, nMaxClock);
		}
		const u64 nInstructions = m_Session.instructions ();
		const unsigned nMs = (nNow - m_nLastStatsTicks) * (1000 / HZ);
		if (nMs != 0)
		{
			m_nMipsX10 = (unsigned) ((nInstructions - m_nLastInstructions) / (100 * (u64) nMs));
		}
		m_nLastInstructions = nInstructions;
		m_nGuestMs = 0;
		m_nLastStatsTicks = nNow;
	}
}

#ifdef G3_QEMU_DRAWTEST
// A test build for QEMU's raspi4b (tools/build-pi.sh is not involved: see
// docs/devlog): core 0 plays the Mac, publishing a 320x240 picture at 8 bits
// that changes every frame, with pause/resume cycles; core 1 draws it.
// Results go to the log (UART); QEMU's screendump shows the picture.
static u8 s_TestVram[800 * 600 * 4];

void CKernel::DrawTest (void)
{
	// First the CPU self-test (generated AArch64 code runs natively here),
	// compiling on core 0, then on core 2.
	m_pSelfTestWork = new (HEAP_HIGH) u8[g3::CpuSelfTest::kWorkBytes];
	for (unsigned nMode = 0; nMode < 2 && m_pSelfTestWork != 0; nMode++)
	{
		g3::CpuSelfTest::Setup Setup;
		Setup.work = m_pSelfTestWork;
		Setup.code = g3_selftest_code;
		Setup.code_words = SelfTestCodeWords;
		Setup.clock_us = SelfTestClock;
		Setup.programs = 20;
		Setup.bench_instructions = 2000000;
		if (nMode == 1)
		{
			if (!m_DrawCore.m_bWorkerStarted)
			{
				m_Logger.Write (FromKernel, LogError, "drawtest: core 2 is not compiling");
				break;
			}
			Setup.worker_attach = SelfTestAttach;
			Setup.worker_context = this;
		}
		const g3::CpuSelfTest::Result Result = g3::CpuSelfTest::run (Setup);
		char Line1[160], Line2[160];
		g3::CpuSelfTest::describe (Result, Line1, sizeof Line1, Line2, sizeof Line2);
		m_Logger.Write (FromKernel, Result.ok () ? LogNotice : LogError, "drawtest: %s", Line1);
		m_Logger.Write (FromKernel, LogNotice, "drawtest: %s; %u block runs in generated code, %u compiled on core 2",
				Line2, (unsigned) Result.jit_block_runs, (unsigned) Result.installed);
	}

	if (!m_bDrawCore)
	{
		m_Logger.Write (FromKernel, LogError, "drawtest: core 1 is not drawing");
		return;
	}
	g3::ScreenFrame &F = m_Frame;
	F.on = TRUE;
	F.width = 320;
	F.height = 240;
	F.depth = 8;
	F.pitch = 320;
	F.vram = s_TestVram;
	F.vram_size = sizeof s_TestVram;
	for (unsigned i = 0; i < 256; i++)
	{
		F.palette[i] = (i << 16) | ((255 - i) << 8) | 0x40;	// red rises, green falls
	}
	for (unsigned y = 0; y < 240; y++)
	{
		for (unsigned x = 0; x < 320; x++)
		{
			s_TestVram[y * 320 + x] = (u8) x;	// a horizontal gradient
		}
	}
	m_pPresenter->publish (F);
	m_pPresenter->resume ();
	boolean bPauseOk = TRUE;
	unsigned nPauses = 0;
	for (unsigned nFrame = 0; nFrame < 300; nFrame++)
	{
		// A white square moving down the picture: about 20 rows change per frame.
		const unsigned y0 = (nFrame * 2) % 220;
		for (unsigned y = 0; y < 20; y++)
		{
			for (unsigned x = 100; x < 120; x++)
			{
				s_TestVram[(y0 + y) * 320 + x] = 255;
			}
		}
		m_pPresenter->publish (F);
		CTimer::SimpleMsDelay (20);
		if (nFrame % 50 == 49)
		{
			m_pPresenter->pause ();
			const u64 nRows = m_pPresenter->rows_redrawn ();
			s_TestVram[0] ^= 0xFF;		// a change core 1 must not draw while paused
			CTimer::SimpleMsDelay (100);
			if (m_pPresenter->rows_redrawn () != nRows)
			{
				bPauseOk = FALSE;
			}
			m_pPresenter->resume ();
			nPauses++;
		}
	}
	CTimer::SimpleMsDelay (200);
	m_Logger.Write (FromKernel, LogNotice, "drawtest: %u frames drawn, %u rows redrawn, %u rows skipped, "
			"%u pauses %s", m_pPresenter->frames_rendered (), (unsigned) m_pPresenter->rows_redrawn (),
			(unsigned) m_pPresenter->rows_skipped (), nPauses, bPauseOk ? "held" : "FAILED");
	// Then the settings menu, painted on the real frame buffer (the log to
	// the UART only from here, as while the menu shows).
	m_pPresenter->pause ();
	m_LogTee.EnableFirst (FALSE);
	{
		g3::MachineConfig Config;
		g3::ConfigMenu Menu (Config, m_Lister, G3_DISKS_DIR, G3_ROM_DIR);
		Menu.set_status ("QEMU test build: the menu as the Pi draws it");
		g3::ConfigMenu::Screen Screen;
		Menu.render (Screen);
		DrawSettings (&m_Screen, Screen);
	}
	m_Logger.Write (FromKernel, LogNotice, "drawtest: done; holding the picture");
	for (;;)
	{
		CTimer::SimpleMsDelay (1000);
	}
}
#endif

TShutdownMode CKernel::Run (void)
{
#ifdef G3_QEMU_DRAWTEST
	LogHostInfo ();
	DrawTest ();
#endif
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
	StartNetwork ();

	unsigned nLastThrottleTicks = m_Timer.GetTicks ();

	while (m_ShutdownMode == ShutdownNone)
	{
		// Core 0's tasks: the network stack and the web server.
		m_Scheduler.Yield ();
		CheckWebAddress ();

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

		if (m_bMacRunning && !m_bMenuShown)
		{
			RunMac ();
		}
		else
		{
			// Sleep until the next interrupt (timer tick at 100 Hz or
			// USB): keeps core 0 cool while idle.
			asm volatile ("wfi");
		}
	}

	StopMac ();	// flushes writable disk images
	WriteLogFile ();

	return m_ShutdownMode;
}
