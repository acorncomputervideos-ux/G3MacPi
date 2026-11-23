/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * guest/ndrv/g3_ndrv.c — our Mac OS display driver ("ndrv") for the
 * emulator's display device (docs/hardware/framebuffer.md).
 *
 * Built -fPIC -shared, converted to PEF by g3_elf2pef. Exports
 * DoDriverIO (code) and TheDriverDescription (data). Mac OS routines are
 * reached only through cfm_call (guest/cfm/cfm.h).
 *
 * Sources (facts only; nothing copied): Apple, "Designing PCI Cards and
 * Drivers for Power Macintosh Computers" (rev. 1999-03-26):
 *   ch. 8  DriverDescription, DoDriverIO, command codes and kinds,
 *          IOCommandIsComplete;
 *   ch. 10 RegistryPropertyGet / RegistryPropertyGetSize;
 *   ch. 11 SetInterruptTimer, CancelTimer, UpTime, DurationToAbsolute;
 *   ch. 12 AAPL,address;
 *   ch. 13 graphics driver control and status calls, their records and
 *          constants; the Video Services Library's interrupt services
 *          (VSLNewInterruptService, VSLDoInterruptService,
 *          VSLDisposeInterruptService, kVBLService 'vbl ').
 * Inside Macintosh: Devices — the CntrlParam layout (csCode at 26,
 * csParam at 28) and the Device Manager result codes. Record layouts use
 * Mac OS's 68K alignment (2-byte), as Apple's Video.h declares it
 * ("#pragma options align=mac68k"), so every record is read and written
 * here through explicit byte offsets. The value 'vido' of kNdrvTypeIsVideo,
 * and clutType 0 / directType 2 / RGBDirect 16: Apple's headers (see
 * docs/guest/ndrv.md).
 *
 * Calling convention notes: DoDriverIO's IOCommandContents is a union
 * passed by value in a register by Mac OS; the SVR4 compiler would expect
 * a union argument by reference, so it is declared here as a pointer.
 * OSErr results are returned sign-extended in r3.
 */
#include "guest/cfm/cfm.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef short s16;
typedef int s32;

/* --- constants (values from the sources above) ------------------------------------ */
enum {
    noErr = 0, controlErr = -17, statusErr = -18, readErr = -19, writErr = -20,
    paramErr = -50, noHardwareErr = -200,
};
enum {  /* IOCommandCode */
    kOpenCommand, kCloseCommand, kReadCommand, kWriteCommand, kControlCommand, kStatusCommand,
    kKillIOCommand, kInitializeCommand, kFinalizeCommand, kReplaceCommand, kSupersededCommand,
};
enum { kImmediateIOCommandKind = 4 };
enum {  /* control csCodes */
    cscReset = 0, cscKillIO = 1, cscSetMode = 2, cscSetEntries = 3, cscSetGamma = 4, cscGrayPage = 5,
    cscSetGray = 6, cscSetInterrupt = 7, cscDirectSetEntries = 8, cscSetDefaultMode = 9,
    cscSwitchMode = 10, cscSetSync = 11, cscSavePreferredConfiguration = 16,
};
enum {  /* status csCodes */
    cscGetMode = 2, cscGetEntries = 3, cscGetPages = 4, cscGetBaseAddr = 5, cscGetGray = 6,
    cscGetInterrupt = 7, cscGetGamma = 8, cscGetDefaultMode = 9, cscGetCurMode = 10, cscGetSync = 11,
    cscGetConnection = 12, cscGetModeTiming = 13, cscGetModeBaseAddress = 14,
    cscGetPreferredConfiguration = 16, cscGetNextResolution = 17, cscGetVideoParameters = 18,
    cscSupportsHardwareCursor = 22,
};
enum { kDepthMode1 = 128, kDepthMode2 = 129, kDepthMode3 = 130 };
#define kDisplayModeIDCurrent 0x0u
#define kDisplayModeIDFindFirstResolution 0xFFFFFFFEu
#define kDisplayModeIDNoMoreResolutions 0xFFFFFFFDu
enum { clutType = 0, directType = 2, kPixelTypeIndexed = 0, RGBDirect = 16 };
enum { kModeValid = 0, kModeSafe = 1, kModeDefault = 2 };
enum { kMultiModeCRT3Connect = 6, kRSCSix = 6, kESCSixMSB3 = 0x23 };
#define kDeclROMtables 0x6465636Cu /* 'decl' */
enum {
    timingVESA_640x480_60hz = 150, timingVESA_800x600_60hz = 182, timingVESA_1024x768_60hz = 190,
    timingApple_1152x870_75hz = 220, timingVESA_1280x1024_60hz = 260, timingVESA_1600x1200_60hz = 280,
};
enum { kDisableHorizontalSyncBit = 0, kDisableVerticalSyncBit = 1, kNoSeparateSyncControlBit = 6 };

/* Our display device's registers (docs/hardware/framebuffer.md). */
enum {
    FB_ID = 0x00, FB_WIDTH = 0x08, FB_HEIGHT = 0x0C, FB_DEPTH = 0x10, FB_PITCH = 0x14, FB_ENABLE = 0x18,
    FB_VRAMSIZE = 0x1C, FB_PALINDEX = 0x20, FB_PALDATA = 0x24, FB_STATUS = 0x28, FB_APPLY = 0x2C,
};
#define FB_MAGIC 0x47334642u /* 'G3FB' */

/* --- imports --------------------------------------------------------------------- */
extern struct cfm_tvector imp_IOCommandIsComplete;   /* DriverServicesLib */
extern struct cfm_tvector imp_RegistryPropertyGetSize; /* NameRegistryLib */
extern struct cfm_tvector imp_RegistryPropertyGet;     /* NameRegistryLib */
extern struct cfm_tvector imp_SetInterruptTimer;       /* DriverServicesLib */
extern struct cfm_tvector imp_CancelTimer;             /* DriverServicesLib */
extern struct cfm_tvector imp_UpTime;                  /* DriverServicesLib */
extern struct cfm_tvector imp_DurationToAbsolute;      /* DriverServicesLib */
extern struct cfm_tvector imp_VSLNewInterruptService;  /* VideoServicesLib */
extern struct cfm_tvector imp_VSLDoInterruptService;   /* VideoServicesLib */
extern struct cfm_tvector imp_VSLDisposeInterruptService; /* VideoServicesLib */

/* --- the driver description (chapter 8; 128 bytes, every field 4-aligned) -------- */
struct DriverDescription {
    u32 driverDescSignature;     /* 'mtej' */
    u32 driverDescVersion;       /* kInitialDriverDescriptor = 0 */
    u8 nameInfoStr[32];          /* Str31: matched against the device's name property */
    u8 version[4];               /* NumVersion */
    u32 driverRuntime;           /* RuntimeOptions */
    u8 driverName[32];           /* Str31 */
    u32 driverDescReserved[8];
    u32 nServices;
    u32 serviceCategory;         /* 'ndrv' */
    u32 serviceType;             /* 'vido' (kNdrvTypeIsVideo) */
    u8 serviceVersion[4];
};
_Static_assert(sizeof(struct DriverDescription) == 128, "DriverDescription layout");
_Static_assert(__builtin_offsetof(struct DriverDescription, driverRuntime) == 44, "layout");
_Static_assert(__builtin_offsetof(struct DriverDescription, nServices) == 112, "layout");

const struct DriverDescription TheDriverDescription = {
    0x6D74656Au, 0,
    {7, 'd', 'i', 's', 'p', 'l', 'a', 'y'},
    {0x00, 0x10, 0x40, 0x01},                    /* 0.1.0a1 (stage 0x40 = alpha, as Listing 8-1) */
    4u | 2u,                                     /* kDriverIsUnderExpertControl | kDriverIsOpenedUponLoad */
    {22, '.', 'D', 'i', 's', 'p', 'l', 'a', 'y', '_', 'V', 'i', 'd', 'e', 'o', '_',
     'G', '3', 'M', 'a', 'c', 'P', 'i'},
    {0, 0, 0, 0, 0, 0, 0, 0},
    1, 0x6E647276u /* 'ndrv' */, 0x7669646Fu /* 'vido' */, {1, 0, 0, 0},
};

/* --- helpers ------------------------------------------------------------------------ */
/* gcc may emit calls to these for loops and copies; defined here, -Bsymbolic keeps them local. */
void *memset(void *d, int c, unsigned long n)
{
    u8 *p = d;
    while (n--)
        *p++ = (u8)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned long n)
{
    u8 *p = d;
    const u8 *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

static u32 ld8(const void *p, u32 o) { return ((const u8 *)p)[o]; }
static u32 ld16(const void *p, u32 o) { return (ld8(p, o) << 8) | ld8(p, o + 1); }
static u32 ld32(const void *p, u32 o) { return (ld16(p, o) << 16) | ld16(p, o + 2); }
static void st8(void *p, u32 o, u32 v) { ((u8 *)p)[o] = (u8)v; }
static void st16(void *p, u32 o, u32 v) { st8(p, o, v >> 8); st8(p, o + 1, v); }
static void st32(void *p, u32 o, u32 v) { st16(p, o, v >> 16); st16(p, o + 2, v); }

/* --- state ------------------------------------------------------------------------- */
struct mode { u32 id, width, height, timing, refresh; /* refresh: Fixed 16.16 Hz */ };
static const struct mode kModes[] = {
    {1, 640, 480, timingVESA_640x480_60hz, 60u << 16},
    {2, 800, 600, timingVESA_800x600_60hz, 60u << 16},
    {3, 1024, 768, timingVESA_1024x768_60hz, 60u << 16},
    {4, 1152, 870, timingApple_1152x870_75hz, 75u << 16},
    {5, 1280, 1024, timingVESA_1280x1024_60hz, 60u << 16},
    {6, 1600, 1200, timingVESA_1600x1200_60hz, 60u << 16},
};
#define NMODES (sizeof kModes / sizeof kModes[0])

/* A GammaTbl (QuickDraw.h layout: six shorts, then formula data, then correction data). */
struct gamma {
    s16 gVersion, gType, gFormulaSize, gChanCnt, gDataCnt, gDataWidth;
    u8 data[3 * 256];
};

static struct {
    volatile u32 *regs;
    u8 *vram;
    u32 vram_size;
    int initialized, open;
    u32 mode_id, depth_mode;              /* current DisplayModeID and relative depth */
    u32 preferred_id, preferred_depth;
    int luminance;                        /* SetGray: 1 = map to gray */
    u32 sync;                             /* SetSync state (bits 0-1) */
    u8 clut[256][3];                      /* as loaded into the hardware (gamma applied) */
    struct gamma gamma;
    /* Vertical blanking: Mac OS moves the cursor and runs VBL tasks when the
       driver signals its 'vbl ' service. The device has no VBL interrupt,
       so a Driver Services timer stands in (chapter 1: "If the hardware
       interrupt for vertical blanking is not provided, a time management
       task may be installed"). */
    u32 vbl_service;                      /* InterruptServiceId; 0 = none */
    u32 vbl_timer[2];                     /* TimerID (room for 64 bits) */
    u32 vbl_period[2];                    /* AbsoluteTime of kVblMs */
    int vbl_running;                      /* the timer is armed */
    int vbl_disabled;                     /* SetInterrupt csMode 1 */
    struct cfm_tvector vbl_tv;            /* our handler, as Mac OS calls it */
} g;

static void hw_w(u32 reg, u32 v)
{
    g.regs[reg / 4] = v;
    __asm__ volatile("eieio" ::: "memory");
}
static u32 hw_r(u32 reg)
{
    const u32 v = g.regs[reg / 4];
    __asm__ volatile("eieio" ::: "memory");
    return v;
}

static const struct mode *find_mode(u32 id)
{
    for (u32 i = 0; i < NMODES; ++i)
        if (kModes[i].id == id)
            return &kModes[i];
    return 0;
}
static u32 hw_depth(u32 depth_mode)  /* relative depth -> device depth value */
{
    return depth_mode == kDepthMode1 ? 8 : depth_mode == kDepthMode2 ? 15 : depth_mode == kDepthMode3 ? 32 : 0;
}
static u32 bits_per_pixel(u32 depth_mode) { u32 d = hw_depth(depth_mode); return d == 15 ? 16 : d; }
static u32 row_bytes(const struct mode *m, u32 depth_mode)  /* the device's pitch rule */
{
    return (m->width * (bits_per_pixel(depth_mode) / 8) + 31) & ~31u;
}
static int fits(const struct mode *m, u32 depth_mode)
{
    return hw_depth(depth_mode) && row_bytes(m, depth_mode) * m->height <= g.vram_size;
}
static u32 max_depth(const struct mode *m)
{
    u32 d = 0;
    for (u32 k = kDepthMode1; k <= kDepthMode3; ++k)
        if (fits(m, k))
            d = k;
    return d;
}

static void load_clut_entry(u32 i)
{
    hw_w(FB_PALINDEX, i);
    hw_w(FB_PALDATA, ((u32)g.clut[i][0] << 16) | ((u32)g.clut[i][1] << 8) | g.clut[i][2]);
}

/* Gamma-corrects a 16-bit QuickDraw channel (top 8 bits index the table). */
static u8 correct(u32 channel, u32 value16)
{
    const u32 base = g.gamma.gChanCnt == 3 ? channel * 256 : 0;
    return g.gamma.data[base + (value16 >> 8)];
}

static void linear_gamma(void)
{
    g.gamma.gVersion = 0;
    g.gamma.gType = 0;
    g.gamma.gFormulaSize = 0;
    g.gamma.gChanCnt = 1;
    g.gamma.gDataCnt = 256;
    g.gamma.gDataWidth = 8;
    for (u32 i = 0; i < 256; ++i)
        g.gamma.data[i] = (u8)i;
}

/* Sets every CLUT entry to 50% gray (before mode changes, chapter 13). */
static void gray_clut(void)
{
    for (u32 i = 0; i < 256; ++i) {
        g.clut[i][0] = g.clut[i][1] = g.clut[i][2] = correct(0, 0x8000);
        load_clut_entry(i);
    }
}

static int program_mode(u32 id, u32 depth_mode)
{
    const struct mode *m = find_mode(id);
    if (!m || !fits(m, depth_mode))
        return paramErr;
    gray_clut();
    hw_w(FB_WIDTH, m->width);
    hw_w(FB_HEIGHT, m->height);
    hw_w(FB_DEPTH, hw_depth(depth_mode));
    hw_w(FB_APPLY, 1);
    if (hw_r(FB_STATUS) != 1)
        return controlErr;
    hw_w(FB_ENABLE, 1);
    g.mode_id = id;
    g.depth_mode = depth_mode;
    return noErr;
}

/* --- vertical blanking --------------------------------------------------------------- */
enum { kVblMs = 16, durationMillisecond = 1 };
#define kVBLService 0x76626C20u /* 'vbl ' */

/* AbsoluteTime is a 64-bit struct; Mac OS routines that return one take a
   pointer to the result as a hidden first argument (r3). */
static void vbl_arm(void);

static long vbl_handler(void *p1, void *p2)  /* SecondaryInterruptHandler2 */
{
    (void)p1;
    (void)p2;
    g.vbl_running = 0;
    if (!g.vbl_service)
        return noErr;
    if (!g.vbl_disabled)
        cfm_call(&imp_VSLDoInterruptService, g.vbl_service);
    vbl_arm();
    return noErr;
}

static void vbl_arm(void)
{
    u32 now[2] = {0, 0};
    cfm_call(&imp_UpTime, now);
    u32 when[2];
    when[1] = now[1] + g.vbl_period[1];
    when[0] = now[0] + g.vbl_period[0] + (when[1] < now[1]);
    g.vbl_tv.code = (unsigned long)vbl_handler;
    g.vbl_tv.toc = 0;  /* our code doesn't use r2 */
    if ((s32)cfm_call(&imp_SetInterruptTimer, when, &g.vbl_tv, 0, g.vbl_timer) == noErr)
        g.vbl_running = 1;
}

static void vbl_start(const void *entry)
{
    u32 id = 0;
    if ((s32)cfm_call(&imp_VSLNewInterruptService, entry, kVBLService, &id) != noErr || id == 0)
        return;  /* Mac OS then works without VBL tasks, as before */
    g.vbl_service = id;
    cfm_call(&imp_DurationToAbsolute, g.vbl_period, (u32)(kVblMs * durationMillisecond));
    vbl_arm();
}

static void vbl_stop(void)
{
    const u32 id = g.vbl_service;
    g.vbl_service = 0;  /* a handler already running won't re-arm */
    if (g.vbl_running) {
        u32 remaining[2];
        cfm_call(&imp_CancelTimer, g.vbl_timer[0], remaining);
        g.vbl_running = 0;
    }
    if (id)
        cfm_call(&imp_VSLDisposeInterruptService, id);
}

/* --- Name Registry ------------------------------------------------------------------- */
/* Reads up to `max` 32-bit cells of a property; returns the count, or -1 if absent. */
static s32 get_cells(const void *entry, const char *name, u32 *cells, u32 max)
{
    u32 size = 0;
    if ((s32)cfm_call(&imp_RegistryPropertyGetSize, entry, name, &size) != noErr)
        return -1;
    if (size > max * 4)
        size = max * 4;
    if ((s32)cfm_call(&imp_RegistryPropertyGet, entry, name, cells, &size) != noErr)
        return -1;
    return (s32)(size / 4);
}

static int initialize(const void *init_info)
{
    /* DriverInitInfo: DriverRefNum refNum (2 bytes), then RegEntryID deviceEntry. */
    const void *entry = (const u8 *)init_info + 2;
    u32 cells[10];
    u32 vram = 0, regs = 0;
    /* Logical addresses as Mac OS maps them (AAPL,address: one per
       assigned-addresses entry); otherwise the physical addresses in
       assigned-addresses (PCI binding: phys.hi, phys.mid, phys.lo, size.hi,
       size.lo per BAR; BAR 0 = video memory, BAR 1 = registers).
       docs/guest/ndrv.md. */
    s32 n = get_cells(entry, "AAPL,address", cells, 10);
    if (n >= 2) {
        vram = cells[0];
        regs = cells[1];
    } else {
        n = get_cells(entry, "assigned-addresses", cells, 10);
        if (n < 10)
            return noHardwareErr;
        vram = cells[2];
        regs = cells[7];
    }
    g.regs = (volatile u32 *)regs;
    g.vram = (u8 *)vram;
    if (hw_r(FB_ID) != FB_MAGIC)
        return noHardwareErr;
    g.vram_size = hw_r(FB_VRAMSIZE);
    linear_gamma();
    g.luminance = 0;
    g.sync = 0;
    for (u32 i = 0; i < 256; ++i) {  /* read back the palette the firmware left */
        hw_w(FB_PALINDEX, i);
        const u32 rgb = hw_r(FB_PALDATA);
        g.clut[i][0] = (u8)(rgb >> 16);
        g.clut[i][1] = (u8)(rgb >> 8);
        g.clut[i][2] = (u8)rgb;
    }
    /* Keep the firmware's mode if it is one of ours, else 640x480 at 8 bits. */
    const u32 w = hw_r(FB_WIDTH), h = hw_r(FB_HEIGHT), d = hw_r(FB_DEPTH);
    u32 id = 1, depth = kDepthMode1;
    for (u32 i = 0; i < NMODES; ++i)
        if (kModes[i].width == w && kModes[i].height == h)
            id = kModes[i].id;
    if (d == 15)
        depth = kDepthMode2;
    else if (d == 32)
        depth = kDepthMode3;
    if (!fits(find_mode(id), depth)) {
        id = 1;
        depth = kDepthMode1;
    }
    const int err = program_mode(id, depth);
    if (err != noErr)
        return err;
    g.preferred_id = id;
    g.preferred_depth = depth;
    g.initialized = 1;
    if (!g.vbl_service)
        vbl_start(entry);
    return noErr;
}

/* --- control calls ---------------------------------------------------------------- */
/* SetEntries / DirectSetEntries: csTable@0, csStart@4, csCount@6; ColorSpec = 8 bytes. */
static int set_entries(const u8 *rec)
{
    const u8 *table = (const u8 *)ld32(rec, 0);
    const s16 start = (s16)ld16(rec, 4);
    const s16 count = (s16)ld16(rec, 6);
    if (!table || count < 0 || start < -1 || (start >= 0 && start + count > 255))
        return paramErr;
    for (s32 i = 0; i <= count; ++i) {
        const u8 *cs = table + 8 * i;
        const u32 index = start >= 0 ? (u32)(start + i) : ld16(cs, 0);
        if (index > 255)
            return paramErr;
        u32 r = ld16(cs, 2), gr = ld16(cs, 4), b = ld16(cs, 6);
        if (g.luminance) {  /* gray of equal HSB brightness: the largest component */
            u32 v = r > gr ? r : gr;
            v = v > b ? v : b;
            r = gr = b = v;
        }
        g.clut[index][0] = correct(0, r);
        g.clut[index][1] = correct(1, gr);
        g.clut[index][2] = correct(2, b);
        load_clut_entry(index);
    }
    return noErr;
}

static int set_gamma(const u8 *rec)
{
    const u8 *t = (const u8 *)ld32(rec, 0);
    if (!t) {
        linear_gamma();
        return noErr;
    }
    const u32 version = ld16(t, 0), type = ld16(t, 2), formula = ld16(t, 4), chans = ld16(t, 6),
              count = ld16(t, 8), width = ld16(t, 10);
    if (version != 0 || type != 0 || (chans != 1 && chans != 3))
        return paramErr;
    if (count != 256 || width != 8)  /* the only correction-data shape we apply */
        return paramErr;
    g.gamma.gChanCnt = (s16)chans;
    const u8 *data = t + 12 + formula;
    for (u32 i = 0; i < chans * 256; ++i)
        g.gamma.data[i] = data[i];
    return noErr;
}

/* GrayPage: a 50% dither in the current depth (patterns from chapter 13),
   inverted on alternate rows; direct modes also get a linear gray table. */
static int gray_page(const u8 *rec)
{
    if (ld16(rec, 6) != 0)  /* csPage: we have one page */
        return paramErr;
    const struct mode *m = find_mode(g.mode_id);
    const u32 bpp = bits_per_pixel(g.depth_mode);
    const u32 pattern = bpp == 8 ? 0xFF00FF00u : bpp == 16 ? 0xFFFF0000u : 0xFFFFFFFFu;
    const u32 pitch = hw_r(FB_PITCH);
    for (u32 y = 0; y < m->height; ++y) {
        u32 *row = (u32 *)(g.vram + y * pitch);
        const u32 base = (y & 1) ? ~pattern : pattern;
        for (u32 x = 0; x < pitch / 4; ++x)
            row[x] = (bpp == 32 && (x & 1)) ? ~base : base;
    }
    if (g.depth_mode != kDepthMode1)
        for (u32 i = 0; i < 256; ++i) {
            g.clut[i][0] = correct(0, i * 0x101);
            g.clut[i][1] = correct(1, i * 0x101);
            g.clut[i][2] = correct(2, i * 0x101);
            load_clut_entry(i);
        }
    return noErr;
}

static int do_control(u32 code, u8 *rec)
{
    switch (code) {
    case cscSetMode: {  /* VDPageInfo: csMode@0, csData@2, csPage@6, csBaseAddr@8 */
        if (ld16(rec, 6) != 0)
            return paramErr;
        const int err = program_mode(g.mode_id, ld16(rec, 0));
        if (err == noErr)
            st32(rec, 8, (u32)g.vram);
        return err;
    }
    case cscSetEntries:
        return g.depth_mode == kDepthMode1 ? set_entries(rec) : controlErr;
    case cscDirectSetEntries:
        return g.depth_mode != kDepthMode1 ? set_entries(rec) : controlErr;
    case cscSetGamma:
        return set_gamma(rec);
    case cscGrayPage:
        return gray_page(rec);
    case cscSetGray:
        g.luminance = ld8(rec, 0) ? 1 : 0;
        return noErr;
    case cscSwitchMode: {  /* VDSwitchInfoRec: csMode@0, csData@2, csPage@6, csBaseAddr@8 */
        if (ld16(rec, 6) != 0)
            return paramErr;
        const int err = program_mode(ld32(rec, 2), ld16(rec, 0));
        if (err == noErr)
            st32(rec, 8, (u32)g.vram);
        return err;
    }
    case cscSetSync: {  /* VDSyncInfoRec: csMode@0, csFlags@1 (0 means 0x3) */
        u32 flags = ld8(rec, 1);
        if (flags == 0)
            flags = 0x3;
        flags &= 0x3;
        g.sync = (g.sync & ~flags) | (ld8(rec, 0) & flags);
        hw_w(FB_ENABLE, g.sync ? 0 : 1);  /* any sync disabled: blank */
        return noErr;
    }
    case cscSavePreferredConfiguration: {
        const struct mode *m = find_mode(ld32(rec, 2));
        if (!m || !fits(m, ld16(rec, 0)))
            return paramErr;
        g.preferred_id = m->id;
        g.preferred_depth = ld16(rec, 0);
        return noErr;
    }
    case cscSetInterrupt:  /* VDFlagRecord: csMode 0 = VBL interrupts on, 1 = off */
        g.vbl_disabled = ld8(rec, 0) != 0;
        return noErr;
    default:  /* Reset, KillIO, SetDefaultMode, cursor, power: not supported */
        return controlErr;
    }
}

/* --- status calls ---------------------------------------------------------------- */
static int do_status(u32 code, u8 *rec)
{
    switch (code) {
    case cscGetMode:      /* VDPageInfo */
        st16(rec, 0, g.depth_mode);
        st16(rec, 6, 0);
        st32(rec, 8, (u32)g.vram);
        return noErr;
    case cscGetEntries: {
        u8 *table = (u8 *)ld32(rec, 0);
        const s16 start = (s16)ld16(rec, 4), count = (s16)ld16(rec, 6);
        if (!table || count < 0 || start < -1 || (start >= 0 && start + count > 255))
            return paramErr;
        for (s32 i = 0; i <= count; ++i) {
            u8 *cs = table + 8 * i;
            const u32 index = start >= 0 ? (u32)(start + i) : ld16(cs, 0);
            if (index > 255)
                return paramErr;
            if (start >= 0)
                st16(cs, 0, index);
            for (u32 c = 0; c < 3; ++c)
                st16(cs, 2 + 2 * c, g.clut[index][c] * 0x101u);
        }
        return noErr;
    }
    case cscGetPages:
        st16(rec, 6, 1);
        return noErr;
    case cscGetBaseAddr:
        if (ld16(rec, 6) != 0)
            return paramErr;
        st32(rec, 8, (u32)g.vram);
        return noErr;
    case cscGetGray:
        st8(rec, 0, g.luminance);
        return noErr;
    case cscGetGamma:
        st32(rec, 0, (u32)&g.gamma);
        return noErr;
    case cscGetCurMode:   /* VDSwitchInfoRec */
        st16(rec, 0, g.depth_mode);
        st32(rec, 2, g.mode_id);
        st16(rec, 6, 0);
        st32(rec, 8, (u32)g.vram);
        return noErr;
    case cscGetSync:      /* 0xFF asks for capabilities */
        if (ld8(rec, 0) == 0xFF)
            st8(rec, 0, (1u << kDisableHorizontalSyncBit) | (1u << kDisableVerticalSyncBit) |
                            (1u << kNoSeparateSyncControlBit));
        else
            st8(rec, 0, g.sync);
        return noErr;
    case cscGetConnection:  /* VDDisplayConnectInfoRec */
        st16(rec, 0, kMultiModeCRT3Connect);
        st8(rec, 2, kRSCSix);
        st8(rec, 3, kESCSixMSB3);
        st32(rec, 4, 0);    /* not all valid/safe: the Display Manager asks GetModeTiming */
        st32(rec, 8, 0);
        st32(rec, 12, 0);
        return noErr;
    case cscGetModeTiming: {  /* VDTimingInfoRec: mode@0, format@8, data@12, flags@16 */
        const struct mode *m = find_mode(ld32(rec, 0));
        if (!m)
            return paramErr;
        st32(rec, 8, kDeclROMtables);
        st32(rec, 12, m->timing);
        st32(rec, 16, (1u << kModeValid) | (1u << kModeSafe) | (m->id == 1 ? 1u << kModeDefault : 0));
        return noErr;
    }
    case cscGetPreferredConfiguration:
        st16(rec, 0, g.preferred_depth);
        st32(rec, 2, g.preferred_id);
        return noErr;
    case cscGetNextResolution: {  /* VDResolutionInfoRec */
        const u32 prev = ld32(rec, 0);
        const struct mode *m = 0;
        if (prev == kDisplayModeIDFindFirstResolution) {
            m = &kModes[0];
        } else if (prev == kDisplayModeIDCurrent) {
            m = find_mode(g.mode_id);
        } else {
            const struct mode *p = find_mode(prev);
            if (!p || !max_depth(p))
                return paramErr;
            m = p + 1 < kModes + NMODES ? p + 1 : 0;
        }
        if (m && !max_depth(m))
            m = 0;  /* modes are in VRAM order: none after this fits */
        if (!m) {
            st32(rec, 4, kDisplayModeIDNoMoreResolutions);
            return noErr;
        }
        st32(rec, 4, m->id);
        st32(rec, 8, m->width);
        st32(rec, 12, m->height);
        st32(rec, 16, m->refresh);
        st16(rec, 20, max_depth(m));
        st32(rec, 22, 0);
        st32(rec, 26, 0);
        return noErr;
    }
    case cscGetVideoParameters: {  /* VDVideoParametersInfoRec */
        const struct mode *m = find_mode(ld32(rec, 0));
        const u32 depth = ld16(rec, 4);
        u8 *vp = (u8 *)ld32(rec, 6);
        if (!m || !fits(m, depth) || !vp)
            return paramErr;
        const u32 bpp = bits_per_pixel(depth);
        for (u32 i = 0; i < 42; ++i)
            vp[i] = 0;
        st32(vp, 0, 0);                         /* vpBaseOffset */
        st16(vp, 4, row_bytes(m, depth));       /* vpRowBytes */
        st16(vp, 6, 0);                         /* vpBounds: top, left, bottom, right */
        st16(vp, 8, 0);
        st16(vp, 10, m->height);
        st16(vp, 12, m->width);
        st32(vp, 22, 72u << 16);                /* vpHRes, vpVRes: 72 dpi (Fixed) */
        st32(vp, 26, 72u << 16);
        st16(vp, 30, bpp == 8 ? kPixelTypeIndexed : RGBDirect);
        st16(vp, 32, bpp);
        st16(vp, 34, bpp == 8 ? 1 : 3);         /* vpCmpCount */
        st16(vp, 36, bpp == 8 ? 8 : bpp == 16 ? 5 : 8);  /* vpCmpSize */
        st32(rec, 10, 1);                       /* csPageCount */
        st32(rec, 14, bpp == 8 ? clutType : directType);
        return noErr;
    }
    case cscSupportsHardwareCursor:
        st8(rec, 0, 0);
        return noErr;
    case cscGetInterrupt:
        st8(rec, 0, g.vbl_disabled ? 1 : 0);
        return noErr;
    default:  /* GetDefaultMode, GetModeBaseAddress, gamma lists, power... */
        return statusErr;
    }
}

/* --- the entry point --------------------------------------------------------------- */
long DoDriverIO(u32 space_id, u32 command_id, void *contents, u32 code, u32 kind)
{
    (void)space_id;
    int result;
    switch (code) {
    case kInitializeCommand:
    case kReplaceCommand:
        result = initialize(contents);
        break;
    case kFinalizeCommand:
    case kSupersededCommand:
        vbl_stop();
        if (g.initialized)
            hw_w(FB_ENABLE, 0);
        g.initialized = 0;
        g.open = 0;
        result = noErr;
        break;
    case kOpenCommand:
        result = g.initialized ? noErr : noHardwareErr;
        if (result == noErr)
            g.open = 1;
        break;
    case kCloseCommand:
        g.open = 0;
        result = noErr;
        break;
    case kReadCommand:
        result = readErr;
        break;
    case kWriteCommand:
        result = writErr;
        break;
    case kKillIOCommand:
        result = noErr;  /* every request completes before DoDriverIO returns */
        break;
    case kControlCommand:
    case kStatusCommand: {
        /* CntrlParam: csCode at 26, csParam at 28 holds the pointer to the record. */
        u8 *pb = contents;
        u8 *rec = (u8 *)ld32(pb, 28);
        const u32 cs = ld16(pb, 26);
        if (!g.initialized)
            result = code == kControlCommand ? controlErr : statusErr;
        else if (!rec)
            result = paramErr;
        else
            result = code == kControlCommand ? do_control(cs, rec) : do_status(cs, rec);
        break;
    }
    default:
        result = paramErr;
        break;
    }
    if (kind & kImmediateIOCommandKind)
        return result;
    /* Synchronous and asynchronous requests are completed through the Device Manager. */
    return (s32)cfm_call(&imp_IOCommandIsComplete, command_id, (long)result);
}
