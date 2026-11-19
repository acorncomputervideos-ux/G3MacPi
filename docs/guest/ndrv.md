# Our Mac OS display driver (ndrv)

Code: `guest/ndrv/g3_ndrv.c`. Test: `tests/guest/run_ndrv.cpp`
(`g3_guest_ndrv`, in CI's guest job).

Mac OS 9 draws through a display driver: a PEF code fragment with a
`DoDriverIO` entry point. Its type is 'ndrv', service type 'vido'. Apple's
drivers are for Apple's hardware. Our display device is our own design
(docs/hardware/framebuffer.md), so we write its driver ourselves. It's
compiled C, converted by `g3_elf2pef` (docs/tools/pef.md), calling Mac OS
only through `cfm_call` (guest/cfm).

## Sources

- **Apple, "Designing PCI Cards and Drivers for Power Macintosh
  Computers"** (revised 1999-03-26):
  - chapter 8: DriverDescription, DoDriverIO, the command codes and
    kinds, IOCommandIsComplete;
  - chapter 10: the Name Registry calls;
  - chapter 12: AAPL,address;
  - chapter 13: every graphics control and status call, their records,
    and the timing, connection and depth constants.
- **Inside Macintosh: Devices:** the CntrlParam layout (csCode at 26,
  csParam at 28) and the Device Manager result codes.
- **Apple's IONDRVSupport headers** (open source, APSL), read for these
  facts only:
  - the value of kNdrvTypeIsVideo ('vido', also on Apple's developer
    site);
  - clutType 0, directType 2, RGBDirect 16;
  - Video.h's 68K record alignment (`#pragma options align=mac68k`).
  Its framebuffer host also confirms that a control call's record
  pointer is in csParam.

Mac OS records use 68K (2-byte) alignment, so VDSwitchInfoRec's csData
is at offset 2, not 4. The driver reads and writes every record through
explicit byte offsets, never C structs.

## What it does

- **Initialize** (and Replace):
  - Finds the device through the Name Registry: `AAPL,address` (logical
    addresses, which Mac OS makes from `assigned-addresses`) if present,
    else the physical addresses in `assigned-addresses`. Our firmware
    gives two entries: video memory, then the register block.
  - Checks the ID register ('G3FB'); otherwise `noHardwareErr`.
  - Reads back the firmware's palette.
  - Keeps the firmware's mode if it's in the table; else uses 640x480 at
    8 bits.
- **Modes**: 640x480, 800x600, 1024x768 and 1280x1024 (VESA 60 Hz
  timings), 1152x870 (Apple 75 Hz) and 1600x1200 (VESA 60 Hz). Each comes
  at 8 (CLUT), 16 (x1R5G5B5) and 32 (x8R8G8B8) bits, as far as video
  memory allows.
  - GetNextResolution lists every mode that fits, with its deepest depth.
  - GetVideoParameters fills the VPBlock.
  - GetModeTiming reports Apple's timing constants, valid and safe.
  - SwitchMode and SetMode program the device. They gray the CLUT first,
    as chapter 13 asks.
- **Colour**:
  - SetEntries and GetEntries, by position or by value field;
    DirectSetEntries on direct modes.
  - SetGamma and GetGamma: one or three channels of 256 eight-bit
    entries, with formula data skipped.
  - SetGray maps colours to the gray of equal HSB brightness.
  - GrayPage fills the screen with the chapter's 50% dither patterns,
    inverted on alternate rows.
- **Connection**: a multiscan display (raw sense 6, extended 0x23,
  Apple's 20" Multiscan example). It reports no "all valid/safe" flags,
  so the Display Manager asks GetModeTiming for each mode.
- **SetSync / GetSync**:
  - Any disabled sync blanks the display.
  - csFlags 0 acts as 0x3, Apple's Energy Saver special case.
  - Capabilities: H, V, no separate control (0x43).
- **Preferences**: SavePreferredConfiguration and
  GetPreferredConfiguration, kept in memory only. NVRAM is a later step.
- **Unsupported**:
  - Reset, KillIO, SetDefaultMode, SetInterrupt, the hardware cursor
    and power states: controlErr or statusErr.
  - SupportsHardwareCursor answers false.
  - Read and write return readErr and writErr.
- Immediate requests return their result. Synchronous and asynchronous
  ones are completed through IOCommandIsComplete, as in Listing 8-2.

## How it's tested

`g3_guest_ndrv` loads the driver on the emulated 750 and connects it to
the real display device model. The test then plays Mac OS:
- It prepares the PEF like the Code Fragment Manager would.
- It provides stand-ins for IOCommandIsComplete and the two Name
  Registry calls. They are emulator trap words behind transition
  vectors.
- It makes the calls that the Device Manager and Display Manager make.

The expected values are written from Apple's documentation, not from
the driver's source:
- the description's bytes;
- every record field at its 68K offset;
- device mode and pitch, palette contents, gamma, luminance mapping and
  dither pixels;
- the resolution list with 4 MB and 1 MB of video memory;
- refusals and completion behaviour.

Planted bugs it catches:
- csParam offset;
- service type;
- VPBlock offset;
- natural instead of 68K alignment;
- luminance rule;
- immediate-request completion;
- gamma formula skip;
- SetSync's csFlags-0 rule;
- the resolution list's video-memory limit;
- GetEntries count;
- graying before a mode change;
- connection type.

## Not yet verified

All of this needs a real Mac OS 9 boot:
- **Mac OS loading it.** The Mac OS ROM file's display expert finding
  the driver, matching `nameInfoStr` "display" to our node's name, and
  preparing the fragment. The firmware doesn't yet put the PEF into the
  node's `driver,AAPL,MacOS,PowerPC` property; that is the next step.
- **Addresses.** Mac OS 9.2.1 does not map a root-level device 1:1; our
  driver's first register read faulted there. The node now sits under
  `/pci` with `assigned-addresses`, and Mac OS maps it (the CD's start-up
  desktop pattern is drawn through the driver).
- **Interrupts.** No vertical-blanking interrupt is offered: there is no
  Video Services Library call yet. The Display Manager may need one for
  the cursor.
