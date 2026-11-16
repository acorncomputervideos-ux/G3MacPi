// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/circle/main.cpp — Circle entry point (pattern from Circle's samples).

#include "kernel.h"
#include <circle/startup.h>

int main (void)
{
	// Cannot return from here: some Circle destructors are not implemented.
	CKernel Kernel;
	if (!Kernel.Initialize ())
	{
		halt ();
		return EXIT_HALT;
	}

	TShutdownMode ShutdownMode = Kernel.Run ();

	switch (ShutdownMode)
	{
	case ShutdownReboot:
		reboot ();
		return EXIT_REBOOT;

	case ShutdownHalt:
	default:
		halt ();
		return EXIT_HALT;
	}
}
