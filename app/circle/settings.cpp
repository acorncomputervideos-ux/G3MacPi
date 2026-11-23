// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/circle/settings.cpp — see settings.h.

#include "settings.h"

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
}

void DrawSettings (CDevice *pScreen, const g3::ConfigMenu::Screen &Screen)
{
	using Attr = g3::ConfigMenu::Attr;

	// Hide the cursor, then each row at a fixed place: redraws do not scroll.
	CString Out ("\x1b[?25l");
	for (unsigned nRow = 0; nRow < g3::ConfigMenu::kRows; nRow++)
	{
		CString Line;
		Line.Format ("\x1b[%u;1H", nRow + 1);
		Out.Append (Line);

		switch (Screen.attr[nRow])
		{
		case Attr::Title:	Out.Append ("\x1b[44m\x1b[97m");	break;	// white on blue
		case Attr::Selected:	Out.Append ("\x1b[7m");			break;	// reverse
		case Attr::Dim:		Out.Append ("\x1b[90m");		break;	// grey
		case Attr::Status:	Out.Append ("\x1b[93m");		break;	// yellow
		case Attr::Changed:	Out.Append ("\x1b[96m");		break;	// cyan
		case Attr::Normal:	break;
		}

		// Pad to the full width so a highlight bar has a fixed length.
		char Text[g3::ConfigMenu::kCols + 1];
		unsigned n = 0;
		for (; Screen.text[nRow][n] != '\0' && n < g3::ConfigMenu::kCols; n++)
		{
			Text[n] = Screen.text[nRow][n];
		}
		for (; n < g3::ConfigMenu::kCols; n++)
		{
			Text[n] = ' ';
		}
		Text[n] = '\0';
		Out.Append (Text);
		Out.Append ("\x1b[0m");
	}

	pScreen->Write ((const char *) Out, Out.GetLength ());
}
