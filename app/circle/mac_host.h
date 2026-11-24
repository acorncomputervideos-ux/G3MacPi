// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/circle/mac_host.h — what the Pi provides to the emulated Mac
// (g3::MacHost): the Mac OS ROM file and disk/CD images on the SD card
// (FatFs), and drawing the Mac's screen on HDMI.

#ifndef G3_APP_CIRCLE_MAC_HOST_H
#define G3_APP_CIRCLE_MAC_HOST_H

#include <circle/screen.h>
#include <circle/types.h>
#include <fatfs/ff.h>

#include "core/storage/block_device.h"
#include "platform/common/mac_session.h"
#include "platform/common/disk_cache.h"

// A disk or CD image file on the SD card, read and written in place.
// final: deleted through its own type only (BlockDevice has no virtual
// destructor by design).
class CFatFsImage final : public g3::BlockDevice
{
public:
	CFatFsImage (void);
	~CFatFsImage (void);

	boolean Open (const char *pPath, boolean bReadOnly);	// SD-card path without "SD:"

	u64 sector_count (void) const override	{ return m_nSectors; }
	bool read_only (void) const override	{ return m_bReadOnly; }
	bool read (u64 lba, u32 count, u8 *out) override;
	bool write (u64 lba, u32 count, const u8 *in) override;
	bool flush (void) override;

private:
	FIL	m_File;
	boolean	m_bOpen;
	boolean	m_bReadOnly;
	u64	m_nSectors;
};

// The core that does disk-image I/O (platform/common/disk_cache.h).
class CDiskIoCore
{
public:
	virtual boolean DiskIoRunning (void) const = 0;
	virtual void AttachDisk (g3::DiskCache *pCache) = 0;
	// Returns once the I/O core no longer uses pCache.
	virtual void DetachDisk (g3::DiskCache *pCache) = 0;

protected:
	~CDiskIoCore (void) = default;
};

class CCircleMacHost : public g3::MacHost
{
public:
	using EjectHandler = void (*) (void *pContext, u32 nDrive);

	CCircleMacHost (void);
	~CCircleMacHost (void);

	void SetEjectHandler (EjectHandler pHandler, void *pContext);
	// With a running I/O core, images are read and written there, through
	// a DiskCache each (4 MB of cache, 128 KB write queue).
	void SetDiskIoCore (CDiskIoCore *pCore) { m_pIoCore = pCore; }

	// g3::MacHost
	const u8 *load_file (const char *pPath, u32 &nSize) override;
	g3::BlockDevice *open_image (const char *pPath, bool bReadOnly) override;
	void close_image (g3::BlockDevice *pImage) override;
	void console (const char *pText, u32 nLength) override;
	void disc_ejected (u32 nDrive) override;

private:
	u8		*m_pFile;		// the last file loaded (the ROM file)
	EjectHandler	m_pEjectHandler;
	void		*m_pEjectContext;
	CDiskIoCore	*m_pIoCore = 0;
	struct TCached
	{
		g3::DiskCache	*pCache;
		CFatFsImage	*pImage;
		u8		*pLines;
		u8		*pWriteQueue;
	};
	static const unsigned MaxCached = 8;
	TCached		m_Cached[MaxCached] = {};
};

// Draws the Mac's screen centred on the HDMI screen, scaled by the largest
// whole factor that fits. pPixels: a buffer for width x height pixels.
void DrawMacScreen (CScreenDevice *pScreen, g3::MacSession &Session, u32 *pPixels);

#endif
