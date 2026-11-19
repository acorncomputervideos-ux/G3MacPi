// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/circle/teedevice.h — a Circle character device that duplicates every
// write to two other devices (used to send log output to both the HDMI
// screen and the UART, so the log can be read with or without a serial cable).

#ifndef G3_APP_CIRCLE_TEEDEVICE_H
#define G3_APP_CIRCLE_TEEDEVICE_H

#include <circle/device.h>

class CTeeDevice : public CDevice
{
public:
	CTeeDevice (CDevice *pFirst, CDevice *pSecond);

	int Write (const void *pBuffer, size_t nCount) override;

	// While disabled, output goes only to the second device (the settings
	// menu owns the screen; the log still reaches the serial port).
	void EnableFirst (boolean bEnable)	{ m_bFirstEnabled = bEnable; }

private:
	CDevice *m_pFirst;
	CDevice *m_pSecond;
	volatile boolean m_bFirstEnabled;
};

#endif
