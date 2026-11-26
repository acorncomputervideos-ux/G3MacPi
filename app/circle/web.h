// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/circle/web.h — the web interface on the Pi, and the Ethernet port
// shared between it and the Mac.
//
// Circle's TCP/IP stack (CNetSubSystem, on core 0's scheduler) does not
// drive the adapter itself here: it gets CPiNetPort, a stand-in device whose
// frames pass through a queue (g3::NetBridge). Core 3 owns the adapter and
// switches frames between the Pi's stack and the Mac by destination address
// (platform/common/net_switch.h). The web server is a task on core 0 that
// serves g3::WebUi (platform/common/web_ui.h) with files on the SD card.

#ifndef G3_APP_CIRCLE_WEB_H
#define G3_APP_CIRCLE_WEB_H

#include <circle/macaddress.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/netdevice.h>
#include <circle/sched/task.h>
#include <circle/types.h>
#include <fatfs/ff.h>

#include "platform/common/net_bridge.h"
#include "platform/common/web_ui.h"
#include "platform/common/wifi_bridge.h"

// The Pi's network stack's view of the Ethernet port (core 0).
class CPiNetPort : public CNetDevice
{
public:
	// Before CNetSubSystem::Initialize: registered first, it is the
	// device the stack takes.
	void Register (void)				{ AddNetDevice (); }
	// After it: the adapter (for the address and the link state).
	void SetAdapter (CNetDevice *pAdapter);
	g3::NetBridge *Bridge (void)			{ return &m_Bridge; }

	const CMACAddress *GetMACAddress (void) const override	{ return &m_MAC; }
	boolean SendFrame (const void *pBuffer, unsigned nLength) override;
	boolean ReceiveFrame (void *pBuffer, unsigned *pResultLength) override;
	boolean IsLinkUp (void) override;
	TNetDeviceSpeed GetLinkSpeed (void) override;
	boolean UpdatePHY (void) override		{ return FALSE; }	// core 3 does it

private:
	g3::NetBridge	m_Bridge;
	CMACAddress	m_MAC;
	CNetDevice	*m_pAdapter = 0;
};

// WiFi: the frame switch between the radio, the Mac and the Pi's stack
// (platform/common/wifi_bridge.h). A task on core 0, not on core 3 as for
// Ethernet: Circle's WLAN driver (a port of Plan 9's) does its SDIO work in
// core 0's scheduler tasks and may block, so it is only called from there.
// Runs once each time core 0 yields (between the Mac's time slices).
class CWifiSwitch : public CTask
{
public:
	CWifiSwitch (CNetDevice *pRadio, g3::NetBridge *pMac, g3::NetBridge *pPi,
		     const u8 *pPiAddress, const u8 *pMacAddress);
	void Run (void) override;
	u32 MacIP (void) const	{ return m_Bridge.mac_ip (); }	// learnt from its frames; 0: none yet
	boolean Joined (void) const	{ return m_pRadio->IsLinkUp (); }

private:
	void Service (void);

	CNetDevice	*m_pRadio;
	g3::NetBridge	*m_pMac;	// 0: the Mac has no network
	g3::NetBridge	*m_pPi;		// 0: no web interface
	g3::WifiBridge	m_Bridge;
	u8		m_Frame[FRAME_BUFFER_SIZE];
};

// Files on the SD card, the machine's state and actions, for WebUi.
class CCircleWebHost : public g3::WebHost
{
public:
	// The kernel's part: filled in by it.
	struct TKernelHooks
	{
		void	*pContext;
		void	(*Status) (void *pContext, g3::WebStatus &Out);
		boolean	(*InUse) (void *pContext, const char *pPath);
		const char *(*Password) (void *pContext);
		void	(*Reboot) (void *pContext);
		void	(*ConfigChanged) (void *pContext);
		u32	(*InsertMedia) (void *pContext, u32 nDrive, const char *pPath);
	};
	CCircleWebHost (const TKernelHooks &Hooks) : m_Hooks (Hooks) {}

	void status (g3::WebStatus &Out) override	{ (*m_Hooks.Status) (m_Hooks.pContext, Out); }
	bool read_file (const char *pPath, u8 *pBuf, u32 nCap, u32 &nLen) override;
	bool write_file (const char *pPath, const u8 *pData, u32 nLen) override;
	u32 list (const char *pDir, g3::WebFileEntry *pOut, u32 nMax) override;
	bool in_use (const char *pPath) override	{ return (*m_Hooks.InUse) (m_Hooks.pContext, pPath); }
	void *open_write (const char *pPath) override;
	bool write (void *pFile, const u8 *pData, u32 nLen) override;
	bool close (void *pFile) override;
	bool remove (const char *pPath) override;
	bool rename (const char *pFrom, const char *pTo) override;
	const char *password (void) override		{ return (*m_Hooks.Password) (m_Hooks.pContext); }
	void reboot_after_response (void) override	{ m_bReboot = TRUE; }
	void config_changed (void) override		{ (*m_Hooks.ConfigChanged) (m_Hooks.pContext); }
	u32 insert_media (u32 nDrive, const char *pPath) override
	{
		return (*m_Hooks.InsertMedia) (m_Hooks.pContext, nDrive, pPath);
	}

	boolean TakeReboot (void)	{ boolean b = m_bReboot; m_bReboot = FALSE; return b; }
	void Reboot (void)		{ (*m_Hooks.Reboot) (m_Hooks.pContext); }

private:
	TKernelHooks	m_Hooks;
	boolean		m_bReboot = FALSE;
	// One upload at a time, written in 64 KB pieces (the SD card is much
	// faster with large writes).
	static const unsigned BufferSize = 64 * 1024;
	FIL		m_File;
	boolean		m_bOpen = FALSE;
	boolean		m_bOK = TRUE;
	u8		*m_pBuffer = 0;
	unsigned	m_nBuffered = 0;
	boolean		Flush (void);
};

// The web server: a task on core 0, one connection at a time, port 80.
class CWebServer : public CTask
{
public:
	CWebServer (CNetSubSystem *pNet, CCircleWebHost *pHost);
	void Run (void) override;

private:
	static bool SendStub (void *pContext, const u8 *pData, u32 nLength);

	CNetSubSystem	*m_pNet;
	CCircleWebHost	*m_pHost;
	g3::WebUi	m_UI;
};

#endif
