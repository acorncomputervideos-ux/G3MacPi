// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/circle/settings.cpp — see settings.h.

#include "settings.h"
#include "platform/common/frame_copy.h"
#include "platform/common/menu_painter.h"
#include "platform/common/pi_overclock.h"

#include <circle/string.h>
#include <circle/util.h>
#include <fatfs/ff.h>

namespace {

// "SD:" + path, bounded.
void SdPath (char *pOut, unsigned nSize, const char *pPath)
{
	strncpy (pOut, G3_SD_DRIVE, nSize - 1);
	pOut[nSize - 1] = '\0';
	unsigned n = strlen (pOut);
	strncpy (pOut + n, pPath, nSize - n - 1);
	pOut[nSize - 1] = '\0';
}

const unsigned MaxConfigSize = 8192;

} // namespace

s32 CFatFsLister::list (const char *pPath, g3::DirEntry *pOut, u32 nMax)
{
	char Path[g3::MachineConfig::kPathMax + 4];
	SdPath (Path, sizeof Path, pPath);

	DIR Directory;
	if (f_opendir (&Directory, Path) != FR_OK)
	{
		return -1;
	}

	u32 n = 0;
	FILINFO Info;
	while (n < nMax && f_readdir (&Directory, &Info) == FR_OK && Info.fname[0] != '\0')
	{
		if (Info.fattrib & (AM_HID | AM_SYS))
		{
			continue;	// e.g. System Volume Information
		}
		if (Info.fname[0] == '.')
		{
			continue;	// ".", ".." and dot files (macOS leaves "._x" files)
		}
		g3::DirEntry &Entry = pOut[n++];
		strncpy (Entry.name, Info.fname, sizeof Entry.name - 1);
		Entry.name[sizeof Entry.name - 1] = '\0';
		Entry.directory = (Info.fattrib & AM_DIR) != 0;
		Entry.size = Info.fsize;
	}
	f_closedir (&Directory);

	return (s32) n;
}

boolean LoadSettings (g3::MachineConfig &Config, g3::ConfigParseReport &Report)
{
	char Path[64];
	SdPath (Path, sizeof Path, G3_CONFIG_FILE);

	FIL File;
	if (f_open (&File, Path, FA_READ | FA_OPEN_EXISTING) != FR_OK)
	{
		return FALSE;
	}

	static char Buffer[MaxConfigSize];
	UINT nRead = 0;
	FRESULT Result = f_read (&File, Buffer, sizeof Buffer, &nRead);
	f_close (&File);
	if (Result != FR_OK)
	{
		return FALSE;
	}

	g3::parse_machine_config (Buffer, nRead, Config, Report);

	return TRUE;
}

boolean SaveSettings (const g3::MachineConfig &Config)
{
	static char Buffer[MaxConfigSize];
	u32 nLength = g3::write_machine_config (Config, Buffer, sizeof Buffer);
	if (nLength == 0)
	{
		return FALSE;
	}

	char Path[64];
	SdPath (Path, sizeof Path, G3_CONFIG_FILE);

	FIL File;
	if (f_open (&File, Path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		return FALSE;
	}
	UINT nWritten = 0;
	FRESULT Result = f_write (&File, Buffer, nLength, &nWritten);
	FRESULT CloseResult = f_close (&File);

	return Result == FR_OK && CloseResult == FR_OK && nWritten == nLength;
}

boolean LoadMachineState (g3::MachineState &State)
{
	char Path[64];
	SdPath (Path, sizeof Path, G3_STATE_FILE);

	FIL File;
	if (f_open (&File, Path, FA_READ | FA_OPEN_EXISTING) != FR_OK)
	{
		return FALSE;
	}
	static u8 Buffer[g3::MachineState::kFileSize + 1];
	UINT nRead = 0;
	FRESULT Result = f_read (&File, Buffer, sizeof Buffer, &nRead);
	f_close (&File);

	return Result == FR_OK && State.decode (Buffer, nRead);
}

boolean SaveMachineState (const g3::MachineState &State)
{
	static u8 Buffer[g3::MachineState::kFileSize];
	State.encode (Buffer);

	char Path[64];
	SdPath (Path, sizeof Path, G3_STATE_FILE);

	FIL File;
	if (f_open (&File, Path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		return FALSE;
	}
	UINT nWritten = 0;
	FRESULT Result = f_write (&File, Buffer, sizeof Buffer, &nWritten);
	FRESULT CloseResult = f_close (&File);

	return Result == FR_OK && CloseResult == FR_OK && nWritten == sizeof Buffer;
}

void CreateSettingsFolders (void)
{
	char Path[64];
	SdPath (Path, sizeof Path, "/g3macpi");
	f_mkdir (Path);			// FR_EXIST is fine
	SdPath (Path, sizeof Path, G3_DISKS_DIR);
	f_mkdir (Path);
	SdPath (Path, sizeof Path, G3_ROM_DIR);
	f_mkdir (Path);
	SdPath (Path, sizeof Path, G3_CD_DIR);
	f_mkdir (Path);
	SdPath (Path, sizeof Path, G3_HDD_DIR);
	f_mkdir (Path);
}

boolean CreateBlankDisk (const char *pPath, u64 nBytes, const char **ppMessage)
{
	*ppMessage = "";
	char Path[g3::MachineConfig::kPathMax + 8];
	SdPath (Path, sizeof Path, pPath);
	if (nBytes >= 0x100000000ull)			// FAT32: files below 4 GB
	{
		*ppMessage = "Too big for the SD card's FAT32 file system (4 GB at most).";
		return FALSE;
	}

	FATFS *pFileSystem;
	DWORD nFreeClusters;
	if (f_getfree (G3_SD_DRIVE, &nFreeClusters, &pFileSystem) == FR_OK
	    && (u64) nFreeClusters * pFileSystem->csize * 512 < nBytes + (1u << 20))
	{
		*ppMessage = "Not enough free space on the SD card for that size.";
		return FALSE;
	}

	FIL File;
	if (f_open (&File, Path, FA_WRITE | FA_CREATE_NEW) != FR_OK)
	{
		*ppMessage = "Could not create the file (does it exist already?).";
		return FALSE;
	}
	// Seeking past the end in write mode allocates the space (its old
	// contents stay; FatFs doesn't write it). Then zero the first megabyte:
	// no partition map, so Mac OS sees an uninitialised disk.
	boolean bOK = f_lseek (&File, (FSIZE_t) nBytes) == FR_OK && f_tell (&File) == nBytes
		      && f_lseek (&File, 0) == FR_OK;
	static u8 Zero[64 * 1024];
	for (u32 nDone = 0; bOK && nDone < (1u << 20); nDone += sizeof Zero)
	{
		UINT nWritten = 0;
		bOK = f_write (&File, Zero, sizeof Zero, &nWritten) == FR_OK && nWritten == sizeof Zero;
	}
	if (f_close (&File) != FR_OK)
	{
		bOK = FALSE;
	}
	if (!bOK)
	{
		f_unlink (Path);
		*ppMessage = "Writing the new disk failed (SD card full or read-only?).";
	}
	return bOK;
}

static boolean WriteWholeFile (const char *pPath, const char *pData, unsigned nLength)
{
	FIL File;
	if (f_open (&File, pPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		return FALSE;
	}
	UINT nWritten = 0;
	const FRESULT Result = f_write (&File, pData, nLength, &nWritten);
	return f_close (&File) == FR_OK && Result == FR_OK && nWritten == nLength;
}

const char *ApplyPiSpeed (const g3::MachineConfig &Config)
{
	static char Old[8192], New[8192 + 256];
	FIL File;
	if (f_open (&File, G3_SD_DRIVE "/config.txt", FA_READ | FA_OPEN_EXISTING) != FR_OK)
	{
		return "Pi CPU speed: config.txt could not be read.";
	}
	UINT nRead = 0;
	const FRESULT Result = f_read (&File, Old, sizeof Old, &nRead);
	f_close (&File);
	if (Result != FR_OK || nRead == sizeof Old)
	{
		return "Pi CPU speed: config.txt could not be read (or is too large).";
	}
	bool bChanged = false;
	const u32 nNew = g3::pi_overclock::apply (Old, nRead, Config.pi_overclock, New, sizeof New, bChanged);
	if (nNew == 0)
	{
		return "Pi CPU speed: config.txt is too large to change.";
	}
	if (!bChanged)
	{
		return 0;
	}
	// The old file first, so a failed write can be undone on a computer.
	if (!WriteWholeFile (G3_SD_DRIVE "/config.bak", Old, nRead))
	{
		return "Pi CPU speed: could not save config.bak; config.txt left as it was.";
	}
	if (!WriteWholeFile (G3_SD_DRIVE "/config.txt", New, nNew))
	{
		WriteWholeFile (G3_SD_DRIVE "/config.txt", Old, nRead);	// put it back
		return "Pi CPU speed: writing config.txt failed (the old one is in config.bak).";
	}
	return Config.pi_overclock ? "Settings saved. Pi CPU speed 2.0 GHz from the next restart (config.txt)."
				   : "Settings saved. Pi CPU speed 1.8 GHz from the next restart (config.txt).";
}

// The frame buffer as a surface to draw on; the text cursor hidden.
static boolean ScreenSurfaceOf (CScreenDevice *pScreen, g3::ScreenSurface &Surface)
{
	static const char HideCursor[] = "\x1b[?25l";
	pScreen->Write (HideCursor, sizeof HideCursor - 1);
	CBcmFrameBuffer *pFrameBuffer = pScreen->GetFrameBuffer ();
	if (pFrameBuffer == 0)
	{
		return FALSE;
	}
	Surface.base = (u8 *) (uintptr) pFrameBuffer->GetBuffer ();
	Surface.width = pFrameBuffer->GetWidth ();
	Surface.height = pFrameBuffer->GetHeight ();
	Surface.pitch = pFrameBuffer->GetPitch ();
	Surface.depth = pFrameBuffer->GetDepth ();
	return TRUE;
}

// Painting straight into the frame buffer flickers: each repaint shows the
// bare desktop for a moment before the window is drawn over it. So paint
// into a back buffer the size of the screen, then copy the rows that
// changed (platform/common/frame_copy.h).
static g3::ScreenSurface BackBufferFor (const g3::ScreenSurface &Screen)
{
	static u8 *s_pBuffer = 0;
	static unsigned s_nSize = 0;
	unsigned nSize = Screen.pitch * Screen.height;
	if (nSize > s_nSize)
	{
		delete [] s_pBuffer;
		s_pBuffer = new u8[nSize];
		s_nSize = s_pBuffer != 0 ? nSize : 0;
	}
	g3::ScreenSurface Back = Screen;
	Back.base = s_pBuffer;
	return Back;
}

static void Present (const g3::ScreenSurface &Back, const g3::ScreenSurface &Screen)
{
	g3::copy_changed_rows (Back.base, Back.pitch, Screen.base, Screen.pitch,
			       Screen.width * (Screen.depth / 8), Screen.height);
}

void DrawSettings (CScreenDevice *pScreen, const g3::ConfigMenu::Screen &Screen)
{
	// The menu as a classic Mac desktop (platform/common/menu_painter.h).
	g3::ScreenSurface Surface;
	if (!ScreenSurfaceOf (pScreen, Surface))
	{
		return;
	}
	g3::ScreenSurface Back = BackBufferFor (Surface);
	if (Back.base == 0)
	{
		g3::MenuPainter::paint (Screen, Surface);	// no memory: draw directly
		return;
	}
	g3::MenuPainter::paint (Screen, Back);
	Present (Back, Surface);
}

void DrawSplash (CScreenDevice *pScreen, const char *pMessage, const char *pHint)
{
	g3::ScreenSurface Surface;
	if (!ScreenSurfaceOf (pScreen, Surface))
	{
		return;
	}
	g3::ScreenSurface Back = BackBufferFor (Surface);
	if (Back.base == 0)
	{
		g3::MenuPainter::paint_splash (Surface, pMessage, pHint);
		return;
	}
	g3::MenuPainter::paint_splash (Back, pMessage, pHint);
	Present (Back, Surface);
}
