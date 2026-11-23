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
#include <circle/types.h>

#include "core/machine/machine_config.h"
#include "platform/common/config_menu.h"
#include "platform/common/machine_state.h"

#define G3_SD_DRIVE		"SD:"
#define G3_CONFIG_FILE		"/g3macpi/g3macpi.ini"
#define G3_DISKS_DIR		"/g3macpi/disks"
#define G3_ROM_DIR		"/g3macpi/rom"
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

// Creates /g3macpi/disks and /g3macpi/rom if missing, so the user knows
// where to put images.
void CreateSettingsFolders (void);

// Draws the menu's screen (ANSI sequences understood by CScreenDevice).
void DrawSettings (CDevice *pScreen, const g3::ConfigMenu::Screen &Screen);

#endif
