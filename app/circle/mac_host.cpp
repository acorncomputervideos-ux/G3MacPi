// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/circle/mac_host.cpp — see mac_host.h.

#include "mac_host.h"

#include <circle/bcmframebuffer.h>
#include <circle/logger.h>
#include <circle/new.h>
#include <circle/util.h>

#include "settings.h"

static const char FromMac[] = "mac";

namespace {

void SdPath (char *pOut, unsigned nSize, const char *pPath)
{
	strncpy (pOut, G3_SD_DRIVE, nSize - 1);
	pOut[nSize - 1] = '\0';
	unsigned n = strlen (pOut);
	strncpy (pOut + n, pPath, nSize - n - 1);
	pOut[nSize - 1] = '\0';
}

const unsigned MaxRomFile = 16 * 1024 * 1024;	// the Mac OS ROM file is about 2 MB

} // namespace

// --- CFatFsImage ----------------------------------------------------------------------

CFatFsImage::CFatFsImage (void)
:	m_bOpen (FALSE),
	m_bReadOnly (TRUE),
	m_nSectors (0)
{
}

CFatFsImage::~CFatFsImage (void)
{
	if (m_bOpen)
	{
		f_close (&m_File);
	}
}

boolean CFatFsImage::Open (const char *pPath, boolean bReadOnly)
{
	char Path[g3::MachineConfig::kPathMax + 4];
	SdPath (Path, sizeof Path, pPath);

	BYTE Mode = FA_READ | FA_OPEN_EXISTING;
	if (!bReadOnly)
	{
		Mode |= FA_WRITE;
	}
	if (f_open (&m_File, Path, Mode) != FR_OK)
	{
		// A file marked read-only on the card: use it read-only.
		if (bReadOnly || f_open (&m_File, Path, FA_READ | FA_OPEN_EXISTING) != FR_OK)
		{
			return FALSE;
		}
		bReadOnly = TRUE;
	}
	m_bOpen = TRUE;
	m_bReadOnly = bReadOnly;
	m_nSectors = f_size (&m_File) / g3::BlockDevice::kSectorSize;

	return TRUE;
}

bool CFatFsImage::read (u64 lba, u32 count, u8 *out)
{
	if (!m_bOpen || lba > m_nSectors || count > m_nSectors - lba)
	{
		return false;
	}
	if (f_lseek (&m_File, (FSIZE_t) (lba * g3::BlockDevice::kSectorSize)) != FR_OK)
	{
		return false;
	}
	UINT nWant = count * g3::BlockDevice::kSectorSize;
	UINT nRead = 0;

	return f_read (&m_File, out, nWant, &nRead) == FR_OK && nRead == nWant;
}

bool CFatFsImage::write (u64 lba, u32 count, const u8 *in)
{
	if (!m_bOpen || m_bReadOnly || lba > m_nSectors || count > m_nSectors - lba)
	{
		return false;
	}
	if (f_lseek (&m_File, (FSIZE_t) (lba * g3::BlockDevice::kSectorSize)) != FR_OK)
	{
		return false;
	}
	UINT nWant = count * g3::BlockDevice::kSectorSize;
	UINT nWritten = 0;

	return f_write (&m_File, in, nWant, &nWritten) == FR_OK && nWritten == nWant;
}

bool CFatFsImage::flush (void)
{
	return !m_bOpen || m_bReadOnly || f_sync (&m_File) == FR_OK;
}

// --- CCircleMacHost -------------------------------------------------------------------

CCircleMacHost::CCircleMacHost (void)
:	m_pFile (0),
	m_pEjectHandler (0),
	m_pEjectContext (0)
{
}

CCircleMacHost::~CCircleMacHost (void)
{
	delete [] m_pFile;
}

void CCircleMacHost::SetEjectHandler (EjectHandler pHandler, void *pContext)
{
	m_pEjectHandler = pHandler;
	m_pEjectContext = pContext;
}

const u8 *CCircleMacHost::load_file (const char *pPath, u32 &nSize)
{
	char Path[g3::MachineConfig::kPathMax + 4];
	SdPath (Path, sizeof Path, pPath);

	FIL File;
	if (f_open (&File, Path, FA_READ | FA_OPEN_EXISTING) != FR_OK)
	{
		return 0;
	}
	FSIZE_t nFileSize = f_size (&File);
	if (nFileSize == 0 || nFileSize > MaxRomFile)
	{
		f_close (&File);
		return 0;
	}

	delete [] m_pFile;
	m_pFile = new (HEAP_ANY) u8[nFileSize];
	UINT nRead = 0;
	FRESULT Result = f_read (&File, m_pFile, (UINT) nFileSize, &nRead);
	f_close (&File);
	if (Result != FR_OK || nRead != nFileSize)
	{
		return 0;
	}

	nSize = (u32) nFileSize;
	return m_pFile;
}

g3::BlockDevice *CCircleMacHost::open_image (const char *pPath, bool bReadOnly)
{
	CFatFsImage *pImage = new CFatFsImage;
	if (!pImage->Open (pPath, bReadOnly))
	{
		CLogger::Get ()->Write (FromMac, LogWarning, "Cannot open %s", pPath);
		delete pImage;
		return 0;
	}
	CLogger::Get ()->Write (FromMac, LogNotice, "Image %s: %u MB%s", pPath,
				(unsigned) (pImage->sector_count () / 2048),
				pImage->read_only () ? ", read-only" : "");
	if (m_pIoCore == 0 || !m_pIoCore->DiskIoRunning ())
	{
		return pImage;		// I/O on the Mac's core
	}
	for (unsigned i = 0; i < MaxCached; i++)
	{
		TCached &C = m_Cached[i];
		if (C.pCache != 0)
		{
			continue;
		}
		const u32 nLines = 64;			// 4 MB
		const u32 nWriteSectors = 256;		// 128 KB
		C.pLines = new (HEAP_HIGH) u8[nLines * g3::DiskCache::kLineBytes];
		C.pWriteQueue = new (HEAP_HIGH) u8[nWriteSectors * g3::BlockDevice::kSectorSize];
		if (C.pLines == 0 || C.pWriteQueue == 0)
		{
			delete [] C.pLines;
			delete [] C.pWriteQueue;
			C = TCached ();
			break;
		}
		C.pImage = pImage;
		C.pCache = new g3::DiskCache (*pImage, C.pLines, nLines, C.pWriteQueue, nWriteSectors);
		C.pCache->set_wait ([] (void *) { asm volatile ("yield"); }, 0);
		m_pIoCore->AttachDisk (C.pCache);
		return C.pCache;
	}
	return pImage;		// no slot or memory: I/O on the Mac's core
}

void CCircleMacHost::close_image (g3::BlockDevice *pImage)
{
	if (pImage == 0)
	{
		return;
	}
	for (unsigned i = 0; i < MaxCached; i++)
	{
		TCached &C = m_Cached[i];
		if (C.pCache != pImage)
		{
			continue;
		}
		C.pCache->flush ();			// everything written reaches the card
		m_pIoCore->DetachDisk (C.pCache);	// the I/O core lets go of it
		delete C.pCache;
		delete [] C.pLines;
		delete [] C.pWriteQueue;
		pImage = C.pImage;
		C = TCached ();
		break;
	}
	pImage->flush ();
	delete static_cast<CFatFsImage *> (pImage);	// a CFatFsImage, behind a cache or not
}

void CCircleMacHost::console (const char *pText, u32 nLength)
{
	// Firmware console text (rare once Mac OS runs): one log line per call.
	char Line[128];
	unsigned n = nLength < sizeof Line - 1 ? nLength : sizeof Line - 1;
	memcpy (Line, pText, n);
	while (n > 0 && (Line[n - 1] == '\n' || Line[n - 1] == '\r'))
	{
		n--;
	}
	Line[n] = '\0';
	if (n > 0)
	{
		CLogger::Get ()->Write (FromMac, LogNotice, "OF: %s", Line);
	}
}

void CCircleMacHost::EnableNet (const u8 *pAddress)
{
	for (unsigned i = 0; i < 6; i++)
	{
		m_MAC[i] = pAddress[i];
	}
	m_bNet = TRUE;
}

bool CCircleMacHost::mac_address (u8 Out[6])
{
	if (!m_bNet)
	{
		return false;
	}
	for (unsigned i = 0; i < 6; i++)
	{
		Out[i] = m_MAC[i];
	}
	return true;
}

void CCircleMacHost::disc_ejected (u32 nDrive)
{
	if (m_pEjectHandler != 0)
	{
		(*m_pEjectHandler) (m_pEjectContext, nDrive);
	}
}

// --- the Mac's screen on HDMI ---------------------------------------------------------

void DrawMacScreen (CScreenDevice *pScreen, g3::MacSession &Session, u32 *pPixels)
{
	CBcmFrameBuffer *pFrameBuffer = pScreen->GetFrameBuffer ();
	if (pFrameBuffer == 0 || !Session.screen_on ())
	{
		return;
	}

	const u32 nWidth = Session.screen_width ();
	const u32 nHeight = Session.screen_height ();
	const u32 nScreenWidth = pFrameBuffer->GetWidth ();
	const u32 nScreenHeight = pFrameBuffer->GetHeight ();
	if (nWidth == 0 || nHeight == 0 || nWidth > nScreenWidth || nHeight > nScreenHeight)
	{
		return;		// too big for this HDMI mode (cmdline.txt sets width/height)
	}
	Session.screen (pPixels, nWidth);

	u32 nScale = nScreenWidth / nWidth;
	if (nScreenHeight / nHeight < nScale)
	{
		nScale = nScreenHeight / nHeight;
	}
	const u32 x0 = (nScreenWidth - nWidth * nScale) / 2;
	const u32 y0 = (nScreenHeight - nHeight * nScale) / 2;

	const u32 nPitch = pFrameBuffer->GetPitch ();
	const u32 nDepth = pFrameBuffer->GetDepth ();
	u8 *pBase = (u8 *) (uintptr) pFrameBuffer->GetBuffer ();

	for (u32 y = 0; y < nHeight; y++)
	{
		const u32 *pSrc = pPixels + y * nWidth;
		for (u32 sy = 0; sy < nScale; sy++)
		{
			u8 *pRow = pBase + (y0 + y * nScale + sy) * nPitch;
			if (nDepth == 32)
			{
				u32 *pDst = (u32 *) pRow + x0;
				for (u32 x = 0; x < nWidth; x++)
				{
					for (u32 sx = 0; sx < nScale; sx++)
					{
						*pDst++ = pSrc[x] | 0xFF000000;	// Circle: 0xAARRGGBB
					}
				}
			}
			else	// 16 bits: RGB565
			{
				u16 *pDst = (u16 *) pRow + x0;
				for (u32 x = 0; x < nWidth; x++)
				{
					const u32 p = pSrc[x];
					const u16 c = (u16) (((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
					for (u32 sx = 0; sx < nScale; sx++)
					{
						*pDst++ = c;
					}
				}
			}
		}
	}
}
