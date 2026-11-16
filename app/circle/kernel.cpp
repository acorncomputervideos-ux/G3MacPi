// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/circle/kernel.cpp — Milestone 0/1 kernel: startup screen, host
// information, and a self-test proving that the portable emulator core
// (core/) compiles and runs inside Circle.

#include "kernel.h"

#include <circle/machineinfo.h>
#include <circle/memory.h>
#include <circle/string.h>
#include <circle/version.h>

#include "core/common/bitops.h"
#include "core/common/endian.h"
#include "core/common/version.h"

static const char FromKernel[] = "g3macpi";

CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	m_Timer (&m_Interrupt),
	// Circle otherwise leaves the ARM cores at their minimum clock (600 MHz on
	// the Pi 400, observed on hardware 2026-11-16). Run at full speed; the
	// emulator loop must later call m_CPUThrottle.Update() every few seconds so
	// Circle can reduce the clock if the SoC gets hot.
	m_CPUThrottle (CPUSpeedMaximum),
	m_LogTee (&m_Screen, &m_Serial),
	m_Logger (m_Options.GetLogLevel (), &m_Timer)
{
	m_ActLED.Blink (3);	// visible sign of life before the screen is up
}

CKernel::~CKernel (void)
{
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

	boolean bOK =    g3::bswap32 (0x12345678u) == 0x78563412u
		      && Buf[0] == 0x12 && Buf[3] == 0x78
		      && g3::load_be32 (Buf) == 0x12345678u
		      && g3::ppc_mask (16, 31) == 0x0000FFFFu
		      && g3::ppc_mask (31, 0) == 0x80000001u
		      && g3::rotl32 (0x12345678u, 8) == 0x34567812u
		      && g3::sign_extend16 (0x8000u) == -32768;

	m_Logger.Write (FromKernel, bOK ? LogNotice : LogError,
			"Core self-test: %s", bOK ? "PASSED" : "FAILED");

	return bOK;
}

TShutdownMode CKernel::Run (void)
{
	LogHostInfo ();
	RunCoreSelfTest ();

	m_Logger.Write (FromKernel, LogNotice,
			"Milestone 0: no emulation yet. System halted.");

	return ShutdownHalt;
}
