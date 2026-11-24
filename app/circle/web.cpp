// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/circle/web.cpp — see web.h.

#include "web.h"

#include <circle/logger.h>
#include <circle/net/in.h>
#include <circle/net/ipaddress.h>
#include <circle/new.h>
#include <circle/util.h>

#include "settings.h"

static const char FromWeb[] = "web";

// --- the Pi's port --------------------------------------------------------------------------

void CPiNetPort::SetAdapter (CNetDevice *pAdapter)
{
	m_pAdapter = pAdapter;
	m_MAC.Set (pAdapter->GetMACAddress ()->Get ());
}

boolean CPiNetPort::SendFrame (const void *pBuffer, unsigned nLength)
{
	m_Bridge.net_send ((const u8 *) pBuffer, nLength);
	return TRUE;
}

boolean CPiNetPort::ReceiveFrame (void *pBuffer, unsigned *pResultLength)
{
	const u32 n = m_Bridge.net_receive ((u8 *) pBuffer);
	if (n == 0)
	{
		return FALSE;
	}
	*pResultLength = n;
	return TRUE;
}

boolean CPiNetPort::IsLinkUp (void)
{
	return m_pAdapter != 0 && m_pAdapter->IsLinkUp ();	// a flag the adapter keeps
}

TNetDeviceSpeed CPiNetPort::GetLinkSpeed (void)
{
	return m_pAdapter != 0 ? m_pAdapter->GetLinkSpeed () : NetDeviceSpeedUnknown;
}

// --- files ----------------------------------------------------------------------------------

namespace {

void SdPath (char *pOut, unsigned nSize, const char *pPath)
{
	strncpy (pOut, G3_SD_DRIVE, nSize - 1);
	pOut[nSize - 1] = '\0';
	const unsigned n = strlen (pOut);
	strncpy (pOut + n, pPath, nSize - n - 1);
	pOut[nSize - 1] = '\0';
}

} // namespace

bool CCircleWebHost::read_file (const char *pPath, u8 *pBuf, u32 nCap, u32 &nLen)
{
	char Path[400];
	SdPath (Path, sizeof Path, pPath);
	FIL File;
	if (f_open (&File, Path, FA_READ | FA_OPEN_EXISTING) != FR_OK)
	{
		return false;
	}
	UINT nRead = 0;
	const FRESULT Result = f_read (&File, pBuf, nCap, &nRead);
	f_close (&File);
	nLen = nRead;
	return Result == FR_OK;
}

bool CCircleWebHost::write_file (const char *pPath, const u8 *pData, u32 nLen)
{
	char Path[400];
	SdPath (Path, sizeof Path, pPath);
	FIL File;
	if (f_open (&File, Path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		return false;
	}
	UINT nWritten = 0;
	const FRESULT Result = f_write (&File, pData, nLen, &nWritten);
	return f_close (&File) == FR_OK && Result == FR_OK && nWritten == nLen;
}

u32 CCircleWebHost::list (const char *pDir, g3::WebFileEntry *pOut, u32 nMax)
{
	char Path[400];
	SdPath (Path, sizeof Path, pDir);
	DIR Directory;
	if (f_opendir (&Directory, Path) != FR_OK)
	{
		return 0;
	}
	u32 n = 0;
	FILINFO Info;
	while (n < nMax && f_readdir (&Directory, &Info) == FR_OK && Info.fname[0] != '\0')
	{
		if (Info.fattrib & (AM_DIR | AM_HID | AM_SYS))
		{
			continue;
		}
		strncpy (pOut[n].name, Info.fname, sizeof pOut[n].name - 1);
		pOut[n].name[sizeof pOut[n].name - 1] = '\0';
		pOut[n].size = Info.fsize;
		n++;
	}
	f_closedir (&Directory);
	return n;
}

void *CCircleWebHost::open_write (const char *pPath)
{
	if (m_bOpen)
	{
		return 0;
	}
	if (m_pBuffer == 0)
	{
		m_pBuffer = new u8[BufferSize];
		if (m_pBuffer == 0)
		{
			return 0;
		}
	}
	char Path[400];
	SdPath (Path, sizeof Path, pPath);
	if (f_open (&m_File, Path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		return 0;
	}
	m_bOpen = TRUE;
	m_bOK = TRUE;
	m_nBuffered = 0;
	return &m_File;
}

boolean CCircleWebHost::Flush (void)
{
	if (m_nBuffered == 0 || !m_bOK)
	{
		return m_bOK;
	}
	UINT nWritten = 0;
	m_bOK = f_write (&m_File, m_pBuffer, m_nBuffered, &nWritten) == FR_OK && nWritten == m_nBuffered;
	m_nBuffered = 0;
	return m_bOK;
}

bool CCircleWebHost::write (void *pFile, const u8 *pData, u32 nLen)
{
	while (nLen > 0 && m_bOK)
	{
		unsigned n = BufferSize - m_nBuffered;
		if (n > nLen)
		{
			n = nLen;
		}
		memcpy (m_pBuffer + m_nBuffered, pData, n);
		m_nBuffered += n;
		pData += n;
		nLen -= n;
		if (m_nBuffered == BufferSize)
		{
			Flush ();
		}
	}
	return m_bOK;
}

bool CCircleWebHost::close (void *pFile)
{
	if (!m_bOpen)
	{
		return false;
	}
	Flush ();
	const boolean bClosed = f_close (&m_File) == FR_OK;
	m_bOpen = FALSE;
	return m_bOK && bClosed;
}

bool CCircleWebHost::remove (const char *pPath)
{
	char Path[400];
	SdPath (Path, sizeof Path, pPath);
	return f_unlink (Path) == FR_OK;
}

bool CCircleWebHost::rename (const char *pFrom, const char *pTo)
{
	char From[400], To[400];
	SdPath (From, sizeof From, pFrom);
	SdPath (To, sizeof To, pTo);	// FatFs takes the new name's drive off
	return f_rename (From, To) == FR_OK;
}

// --- the server -----------------------------------------------------------------------------

CWebServer::CWebServer (CNetSubSystem *pNet, CCircleWebHost *pHost)
:	CTask (32 * 1024),
	m_pNet (pNet),
	m_pHost (pHost),
	m_UI (*pHost)
{
}

bool CWebServer::SendStub (void *pContext, const u8 *pData, u32 nLength)
{
	CSocket *pSocket = (CSocket *) pContext;
	while (nLength > 0)
	{
		const unsigned n = nLength < 8192 ? nLength : 8192;
		if (pSocket->Send (pData, n, 0) < 0)
		{
			return false;
		}
		pData += n;
		nLength -= n;
	}
	return true;
}

void CWebServer::Run (void)
{
	CSocket Listener (m_pNet, IPPROTO_TCP);
	if (Listener.Bind (80) < 0 || Listener.Listen () < 0)
	{
		CLogger::Get ()->Write (FromWeb, LogError, "Cannot listen on port 80");
		return;
	}
	CLogger::Get ()->Write (FromWeb, LogNotice, "Web interface on port 80");
	static u8 Buffer[FRAME_BUFFER_SIZE];
	for (;;)
	{
		CIPAddress Client;
		u16 nPort;
		CSocket *pConnection = Listener.Accept (&Client, &nPort);
		if (pConnection == 0)
		{
			continue;
		}
		{
			g3::HttpConnection Connection (m_UI, SendStub, pConnection);
			for (;;)
			{
				const int n = pConnection->Receive (Buffer, sizeof Buffer, 0);
				if (n <= 0)
				{
					Connection.closed ();
					break;
				}
				if (!Connection.feed (Buffer, (u32) n))
				{
					break;
				}
			}
		}
		delete pConnection;
		if (m_pHost->TakeReboot ())
		{
			m_pHost->Reboot ();
		}
	}
}
