# Booting the machine with our firmware — implementation notes

Code: `core/machine/bw_firmware.*` (class `BwFirmware`). Tests:
`tests/unit/machine/test_bw_boot.cpp`.

`BwFirmware` is installed in a `YosemiteMachine` and does what Apple's
boot ROM firmware would do at power-on. Then `command()` runs Open
Firmware user-interface text such as `boot hd:2,\\:tbxi`.

## Power-on

1. **PCI resources**: the DEC 21154 bridge gets bus numbers (primary 0,
   secondary 1, subordinate 1). The mac-io chip's BAR 0 is set to
   0x80800000, with memory space and bus mastering enabled, so its cells
   appear on the bus. The BAR address is our choice (to compare with a
   real B&W).
2. **Memory layout**: the top 1 MB of RAM belongs to the firmware.

   | Offset from the region start | Use |
   |---|---|
   | 0x0 | client-interface trap word (`r5` points here) |
   | 0x1000 | Forth data space (64 KB) |
   | 0x11000 | property scratch space (16 KB) |
   | below 0x40000 | client program stack (r1 starts near 0x40000) |

   Everything below the region is free for `claim`. `load-base` is
   0x00400000 (4 MB). That's our choice: low enough for small machines,
   and above where ELF programs usually put their segments, which keeps
   the CHRP rule that the loaded image and the segments don't overlap.
   Programs may also use the region from load-base up to the firmware.
3. **Device tree**: `build_bw_device_tree` with the machine's RAM size,
   clocks, PVR and the BAR (see `bw-device-tree.md`).
4. **Devices**: disk images become `DiskDriver`s on the IDE disk nodes.
   The console (`/packages/terminal-emulator`) is opened, and its
   ihandle is put in `/chosen` as `stdout` and `stdin`.
5. **Client interface**: the trap word is installed and the CPU's HLE
   hook attached. The client interface gets the memory map, the
   instances and the Forth interpreter. `milliseconds` and `get-msecs`
   count emulated time.

`go` sets up the CPU (`Firmware::start_client`: pc, r1, r3 = r4 = 0,
r5, real mode). The caller then runs the machine.

At least 8 MB of RAM is required.

## The end-to-end test

A disk image with an Apple Partition Map and an HFS Plus partition. The
partition's blessed folder holds a boot file of type `tbxi`, whose text
is a CHRP bootinfo object. `boot hd:2,\\:tbxi` runs its script:
- the script prints "booting";
- it loads `\System Folder\Fragmented`, an ELF program stored in 10
  fragments, two of them through the extents overflow file;
- `init-program go` then starts it.

The client program runs on the emulated 750 and uses the client
interface as a real one does:
- `finddevice("/chosen")`;
- `getprop(chosen, "stdout")`;
- `write(stdout, "Hello from a client program\n")`.

The text arrives at the console.

Sabotage runs remove one step each (free memory, `stdout`, the CPU
start, the BAR enable, the CPU hook, the console binding). Every one
breaks the test.
