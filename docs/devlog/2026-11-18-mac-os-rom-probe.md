# 2026-11-18 — first contact with Apple's Mac OS ROM file

The user supplied the NewWorld "Mac OS ROM" data fork:
- 1,945,746 bytes;
- SHA-1 a63053efcbf8c569f82ede6ec69a551c10b7e899;
- compatible list "iMac,1 PowerMac1,1 PowerBook1,1".

The same size appears inside Apple's Mac OS ROM Update 1.0 archive, which
installs version 1.6. Nothing from it is committed.

Its bootinfo boot script was run by our firmware with
`g3_rom_probe "Mac OS ROM"`. That tool places the file at load-base, as
`load` would, and runs `init-program`.

What it needed, in order:
1. **Interpreted control structures.** The script uses `if … then` and a
   multi-line `begin … while … repeat` outside any definition, as IEEE 1275
   allows. Our Forth refused ("compile-only word"). Now implemented, see
   docs/firmware/forth.md.
2. **Interpreted `abort"`.** Now implemented.
3. **`$=`, `bounds`, `3drop`, `$find`, `.registers`.** Now defined.
   `g3_rom_probe … --words` checks every word of a script at once.
4. **The script's checks pass** on our B&W device tree: model, then
   compatible "PowerMac1,1".
5. **It now stops at `memory??`.** `/chosen` has no `memory` and `mmu`
   ihandles. The script calls `claim`, `release`, `map`, `unmap` and
   `translate` on them. It claims physical pages for the compressed
   toolbox image and maps them at a separately claimed virtual address.
   That address goes into `/rom/macos`'s "AAPL,toolbox-image,lzss" for
   the ELF loader (MacOS.elf), which `init-program` / `go` then start.

So the firmware must run in the PowerPC binding's Virtual-Mode
(binding rev. 2.1, real-mode? false):
- it keeps its own translations, published as the "translations"
  property `(virt size phys mode)` on the mmu package;
- the client program starts with those translations active.

That is the next piece of work.

## Later the same day: Virtual-Mode, and Apple's loader runs

Our firmware now runs in Virtual-Mode (docs/firmware/virtual-mode.md),
with these additions:
- `my-self` is a value (`0 to my-self`);
- init-program reports which ELF check fails;
- physical claims come from the top (with the bottom-up rule, the
  compressed ROM image took the loader's fixed addresses);
- `exit` no longer returns to the client: it is parked on an idle trap.

With these, the whole boot script runs:
1. it claims and maps memory and copies the compressed toolbox image
   (1.8 MB);
2. it creates `/rom/macos`;
3. it copies MacOS.elf to load-base;
4. `init-program` loads its two segments (0x200000 code and 0x100000
   data), and `go` starts it at 0x208780 with translation on.

**Apple's loader then runs on the emulated 750 and calls our client
interface.** `g3_rom_probe` logs every call:

    finddevice "/chosen"    getprop stdout
    finddevice "/"          getprop "AAPL,debug"  (absent)
                            getprop "AAPL,writable-ROM-aperture"  (absent)
                            getproplen "copyright"  (absent)
    write "MacOS: Boot Failure!  (0xF3C481F6)"
    write "Official Apple copyright message missing."
    exit

Before `exit` was fixed, the call returned. The loader went on to look
for `/rtas` (absent: "Unable to find RTAS. Proceed with caution"), then
claimed memory, then hung.

The loader checks the root node's "copyright" property. Whether our
firmware should carry Apple's copyright notice there is the user's
decision.

## The copyright property

The project owner agreed to add the root node's "copyright" property, for
interoperability only. MacOS.elf holds the strings "Copyright " and
" Apple Computer, Inc." next to its error message. The value is
"Copyright 1983-1999 Apple Computer, Inc. All Rights Reserved"
(`kAppleCopyrightNotice`, bw_device_tree.h).

With it, the loader passes that check. It then reads `/chosen` mmu and
memory and looks for `/rtas`. Ours is absent, so it writes
"MacOS: Boot Failure!" and "/rtas not found!" and exits.

Next: RTAS (CHRP Run-Time Abstraction Services), meaning a `/rtas` node,
`rtas-size`, `instantiate-rtas` and the RTAS calls Mac OS makes.

## RTAS, the device tree, and Mac OS's NanoKernel starts

Each stop the loader made, and the fix:
1. **`/rtas` not found.** Added RTAS (docs/firmware/rtas.md).
2. **read-pci-config token not found.** Added PCI configuration through
   RTAS.
3. **`/cpus/@0` not found.** Path components with only a unit address
   now resolve.
4. **Missing "reservation-granule-size".** Added, with the binding's TLB
   properties.
5. **The probe's time unit was wrong** (ns, where ps was needed). Fixed.
   The loader then decompressed the 1.8 MB toolbox image (about 54
   million instructions).
6. **Floating-point-unavailable exception** (vector 0x800, with MSR[IP]
   set). Clients now start with MSR FP and ME on and IP off. Every
   exception vector holds a firmware trap that reports unexpected
   exceptions.
7. **"unable to find an interrupt controller node".**
   - Turning on the loader's own debug output (root `AAPL,debug` = −1,
     probe only) showed what it wants.
   - `/chosen` "interrupt-controller" is a phandle.
   - Interrupting devices need `interrupt-parent`.
   - The controller is a node of device_type "interrupt-controller"
     with compatible "paddington". Added.
8. The loader built Mac OS's tables (ProductInfo, DecoderInfo, interrupt
   masks and vectors), mapped the interrupt controller and the ROM, and
   called **`quiesce`** (Apple's client-interface service). Added, as a
   no-op.
9. **DSI on its work area at 0x0FE4000C.** The hardware-init code had set
   DBAT0 to 0-16 MB and relied on the page table. Every translation now
   goes into the page table, sized for the RAM.

**Result.** Apple's NanoKernel runs and sets up its own page table and
BATs (ROM at 0xFFC00000, the 68K emulator at 0x68000000). Over a billion
instructions in, the CPU is inside the 68K emulator.

The program exceptions on the way are the NanoKernel probing CPU models:
`mfspr` of SPR 0 (the 601's MQ) and SPR 959 (604), both illegal on the
750, as the CPU must report. The data faults are its own page-fault
handling. `G3_STEP=1 g3_rom_probe …` lists the exceptions, with the
instruction behind each program exception.

Speed is now the limit. The interpreter runs about 7-8 million
instructions per second on this host, so a full Mac OS boot will need
many seconds to minutes of host time.

## Display driver, NVRAM, and the first 68K stall

- **Display driver.** `BwFirmware::Config::display_driver` puts our PEF
  driver in the display node's "driver,AAPL,MacOS,PowerPC"
  (`G3_NDRV=…pef` in the probe). The loader copies the property into
  Mac OS's Name Registry.
- **/chosen "nvram"**: an instance of mac-io's nvram node (an
  NvramDriver: read, write, seek, size).
  - The loader walks the CHRP partition chain through it.
  - Blank NVRAM (all lengths 0) made it read offset 0 forever. Firmware
    now formats a blank NVRAM: free space, "APL,MacOS75" at 0x12F0
    (XPRAM 0x1300, Name Registry 0x1400), "common" at 0x1800
    (core/machine/bw_nvram.*).
  - The loader showed that "APL,MacOS75" carries signature 0xA0. With
    0x5F it made its own partition; with 0xA0 it uses ours.
- **The stall.** After the hand-over the 68K code loops at ROM
  0xFFC156B6:

      FFC156B8  move.w ([$5FFFEFD0],$7A),d0
      FFC156C0  cmpi.w #$0800,d0
      FFC156C4  beq.s  $FFC156B6

  The pointer at 0x5FFFEFD0 is 0. The ROM polls a register (offsets
  0x7A and 0x1C nearby) of a device whose base it never got. That is
  likely one the loader reported missing (SCC, sound, SCSI, …) and that a
  B&W has. Next: identify it.
- **Probe additions:** G3_SAMPLE_MS (where the CPU is, every N ms),
  G3_DUMP (physical memory), G3_VDUMP (through the CPU's translation),
  NVRAM partition listing.

## The 68K wait is the SCSI interrupt vector; then MESH

- **G3_VDUMP printed zeros.** That was a probe bug: an argument was
  evaluated before the read that set it. The pointer at 0x5FFFEFD0 is in
  fact 0x5FFFEB80, a NanoKernel "Hnfo" structure (written at
  0xF1030C, as a watchpoint showed). The polled word at +0x7A is one of
  the interrupt vectors the loader computes. 0x0800 means "none". By the
  loader's print order (SIO, SCSI, SCC A, SCC B, VIA, ADB, NMI) it is
  SCSI.
- **A real B&W has MESH SCSI.** A published Linux boot log from a
  PowerMac1,1 (GitHub gist by tomari) shows "mesh: configured…", with
  interrupts 12 and 0. It also gives real facts for ESCC (0x13020 /
  0x13000, interrupts 15 and 16) and NVRAM (OF 0x140, XP 0x11b0, NR
  0x12b0). Our NVRAM layout now uses those offsets.
- **The SCSI node.** The loader recognises it only when named "scsi" (it
  ignored "mesh"). With it, SCSIIntVect = 11 and the 68K wait ends.
- **Next stop.** In timed runs the NanoKernel stops in its fatal-error
  loop (0xF10FE8: it increments the word at 0 forever). Ppc750 now has an
  exception hook (`G3_EXC`, `G3_EXC_VEC` in the probe), which shows the
  cause: a machine check reading 0x80810040, MESH's bus-status register.
  We don't emulate MESH yet. Next: a MESH model, starting with no
  devices on the bus.
