// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/circle/main.cpp — Circle entry point (pattern from Circle's samples).

#include "kernel.h"
#include <circle/startup.h>

int main (void)
{
	// Cannot return from here: some Circle destructors are not implemented.
	// The kernel object is large (about 300 KB: the Mac's sound and network
	// queues among its members) and lives on the heap: core 0's start-up
	// stack is 128 KB (KERNEL_STACK_SIZE). As a local here it overflowed
	// that stack into the memory below MEM_KERNEL_END, which Circle's USB
	// driver caught ("assertion failed: (uintptr) pBuffer > MEM_KERNEL_END",
	// on a Pi 400).
	CKernel *pKernel = new CKernel;
	if (pKernel == 0 || !pKernel->Initialize ())
	{
		halt ();
		return EXIT_HALT;
	}

	TShutdownMode ShutdownMode = pKernel->Run ();

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
