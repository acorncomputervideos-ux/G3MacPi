// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/circle/kernel.h — bare-metal entry object for the Raspberry Pi 400.
// Milestone 0/1: shows the startup screen and logs to HDMI + UART.

#ifndef G3_APP_CIRCLE_KERNEL_H
#define G3_APP_CIRCLE_KERNEL_H

#include <circle/actled.h>
#include <circle/devicenameservice.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/screen.h>
#include <circle/serial.h>
#include <circle/timer.h>
#include <circle/types.h>

#include "teedevice.h"

enum TShutdownMode
{
	ShutdownNone,
	ShutdownHalt,
	ShutdownReboot
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
	CTeeDevice		m_LogTee;
	CLogger			m_Logger;
};

#endif
