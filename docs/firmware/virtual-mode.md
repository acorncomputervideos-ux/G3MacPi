# Virtual-Mode firmware

Code:
- `core/firmware/openfirm/mmu.*`: translations, page table, `mmu` and
  `memory` packages;
- `core/firmware/openfirm/guest_view.*`;
- `core/machine/bw_firmware.cpp` (`apply_mmu`).

Tests:
- `tests/unit/firmware/test_mmu.cpp`;
- `tests/unit/machine/test_bw_virtual.cpp`.

## Why

Apple's Mac OS ROM boot script needs the firmware's memory services,
reached through `/chosen` `mmu` and `memory`:
- it claims physical pages and virtual pages separately;
- it `map`s the first at the second;
- it copies data through the virtual addresses;
- it hands a virtual address to its loader, in `/rom/macos`'s
  "AAPL,toolbox-image,lzss".

That is the PowerPC binding's Virtual-Mode.

## Source

PowerPC Processor Binding to IEEE 1275, revision 2.1 (IBM):
- §4.2: Virtual-Mode when `real-mode?` is false. Firmware and client share
  one virtual address space, and addresses passed between them are
  virtual. The firmware keeps its own translations, and the client
  must preserve them if it takes over.
- §5.1.7: the package that `/chosen` `mmu` refers to has "translations":
  sets of `(virt size phys mode)`, each encode-int.
- §6:
  - The MMU methods are those of IEEE 1275 §3.6.5, implemented by CPU
    nodes.
  - `mode` is the PTE's WIMGxPP bits.
  - mode −1 means W=0 I=0 M=0 G=1 PP=10 for memory, and W=1 I=1 M=0 G=1
    PP=10 for I/O.
- §4.1: memory used for interrupt vectors isn't available for allocation.

The method stack effects are written from IEEE 1275 §3.6.5 as the
binding cites it, and are still to be checked against the standard's
text:
- `claim ( [virt] size align -- base )` (with align 0, the address is
  given);
- `release ( virt size -- )`;
- `map ( phys virt size mode -- )`;
- `unmap ( virt size -- )`;
- `modify ( virt size mode -- )`;
- `translate ( virt -- false | phys mode true )`.

Apple's boot script uses claim, release, map and unmap with exactly
these shapes.

## Design

- **`Translations`**: a page-granular list, ascending and
  non-overlapping. Mapping over an existing range replaces it; unmapping
  splits entries.
- **At power-on**, 1:1 translations cover:
  - RAM (M, PP=10);
  - mac-io's BAR;
  - the display's video memory and registers (the binding's I/O
    default).
- **Hardware**, rebuilt from the list after every change (`apply_mmu`).
  Every translation goes into the page table, BAT-covered ones included.
  Mac OS's hardware-init code reprograms DBAT0 to the first 16 MB, clears
  the other BATs, and then relies on the page table for the rest of RAM.
  When RAM was BAT-only, it faulted on its work area at the top of RAM.
  The table is sized RAM / 128, the PowerPC manual's 64 KB per 8 MB, so
  2 MB for 256 MB. It sits just below the firmware region.
  - BATs map four 1:1 windows: RAM (one BAT up to 256 MB, two up to
    512 MB), 0x80000000 (256 MB, PCI and mac-io), and 0x90000000 (32 MB,
    the display).
  - Every translated page gets a PTE in the hashed page table, with SDR1
    pointing at it.
  - Segment register n holds VSID n, with keys 0, so PP 00 to 10 are
    read/write.
  - A non-1:1 mapping inside a BAT window is refused, since the BAT
    would hide it.
- **Virtual space** for `mmu` claims runs from the end of the RAM window
  up to 0x80000000.
- **Physical claims** with an alignment are taken from the top of free
  memory. Low memory stays free for programs that load at fixed
  addresses. Apple's loader needs 0x100000 to 0x213D68, and an aligned
  claim from the bottom gave that space to the compressed ROM image.
  IEEE 1275 leaves the choice to the firmware.
- **Exception vectors**: memory below 0x4000 is never handed out
  (binding §4.1).
- **Guest memory access**: the firmware's Forth and client interface
  read and write guest memory through `GuestView`, which uses the CPU's
  own translation as a data access would (`Ppc750::probe_translate`: no
  exceptions, no R/C updates). Before `go` that follows the firmware's
  tables. After `go` it follows whatever the client has set up, as the
  binding's client interface rules require.
- **The client starts** with MSR IR|DR set.
- `/options` has "real-mode?" "false". There is a `/rom` node, so that
  the boot script can create `/rom/macos` under it.

## Limits

- At most 512 MB of RAM in Virtual-Mode (two RAM BATs).
- At most 128 translations. The page table is rebuilt in full on every
  change, which is slow for large RAM but rare.
- No `set-callback` translate callbacks (§4.2.6), and no lazy handling
  of page-table overflow.
- The firmware's user interface doesn't handle DSI/ISI itself. An
  unmapped address in Forth is an error, not a fault.
- Apple's real `/rom` node has properties ours doesn't (to verify against
  a real device tree).

## Tests

- Translations: map, replace, unmap splits, modify, page rounding, a
  full table, and ranges that run past 4 GB.
- Page table:
  - the PTEG addresses and PTE words of the CPU test's worked example;
  - primary then secondary groups, and both full;
  - 300 pages each found by the emulated 750's own translation;
  - the probe leaving R/C bits alone.
- Package methods: claim (exact and aligned), map, translate, mode −1
  defaults, unmap, release, failure as a Forth error, and memory
  claim/release.
- Machine level:
  - `/chosen` mmu/memory, the translations property, real-mode? and
    `/rom`;
  - a Forth script maps a page and writes through the virtual address,
    and the bytes land in the physical page and nowhere else;
  - unmap, after which the address fails;
  - the client interface reading an argument array and strings that
    are only virtually addressed;
  - aligned physical claims from the top, and the vectors refused;
  - the client starting with IR/DR.

Planted bugs caught:
- client interface reading physically;
- no PTEs;
- bottom-up claims;
- real-mode client start;
- BAT shadowing allowed;
- no translations property.
