// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/circle/settings.h — the Pi side of the settings menu: the
// configuration file on the SD card, directory listings for the file
// pickers (FatFs), and drawing the menu on the screen.
//
// Paths in the configuration and the menu are SD-card paths without the
// drive prefix ("/g3macpi/disks/x.iso"); FatFs gets them as "SD:/...".

#ifndef G3_APP_CIRCLE_SETTINGS_H
#define G3_APP_CIRCLE_SETTINGS_H

#include <circle/device.h>
#include <circle/screen.h>
#include <circle/types.h>

#include "core/machine/machine_config.h"
#include "platform/common/config_menu.h"
#include "platform/common/machine_state.h"

#define G3_SD_DRIVE		"SD:"
#define G3_CONFIG_FILE		"/g3macpi/g3macpi.ini"
#define G3_DISKS_DIR		"/g3macpi/disks"	// any images (kept for older setups)
#define G3_ROM_DIR		"/g3macpi/rom"		// the Mac OS ROM file
#define G3_CD_DIR		"/g3macpi/cd"		// CD images
#define G3_HDD_DIR		"/g3macpi/hdd"		// hard-disk images (new blank disks go here)
#define G3_STATE_FILE		"/g3macpi/machine.state"	// clock, NVRAM, PRAM

class CFatFsLister : public g3::DirectoryLister
{
public:
	s32 list (const char *pPath, g3::DirEntry *pOut, u32 nMax) override;
};

// Reads the configuration file into Config (left at its defaults where the
// file is missing or invalid). Returns FALSE if the file could not be read;
// Report describes ignored lines.
boolean LoadSettings (g3::MachineConfig &Config, g3::ConfigParseReport &Report);

// Writes the configuration file. FALSE on failure.
boolean SaveSettings (const g3::MachineConfig &Config);

// The Mac's clock, NVRAM and PRAM from its last run (G3_STATE_FILE).
// FALSE if there is none or it is damaged.
boolean LoadMachineState (g3::MachineState &State);
boolean SaveMachineState (const g3::MachineState &State);

// Creates the folders (/g3macpi/rom, cd, hdd, disks) if missing, so the
// user knows where to put files.
void CreateSettingsFolders (void);

// A new blank hard-disk image of nBytes at pPath (SD path): the space is
// allocated and the first megabyte zeroed, so Mac OS sees an uninitialised
// disk (Drive Setup then initialises it). FALSE with a message on failure;
// a partly made file is removed.
boolean CreateBlankDisk (const char *pPath, u64 nBytes, const char **ppMessage);

// The "Pi CPU speed" setting into the firmware's config.txt (a marked
// block: platform/common/pi_overclock.h), from the next power-on. The file
// is rewritten only if the block must change; the old one is kept as
// config.bak first. Returns a line for the menu's status, or 0 when
// nothing changed.
const char *ApplyPiSpeed (const g3::MachineConfig &Config);

// Draws the menu's screen: a classic Mac desktop and window (menu_painter.h).
void DrawSettings (CScreenDevice *pScreen, const g3::ConfigMenu::Screen &Screen);
// The start-up screen (no log on HDMI): see MenuPainter::paint_splash.
void DrawSplash (CScreenDevice *pScreen, const char *pMessage, const char *pHint);

#endif
