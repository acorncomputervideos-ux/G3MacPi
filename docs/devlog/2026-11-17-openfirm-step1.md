# 2026-11-17 — Open Firmware, step 1: device tree and client interface

- `of::DeviceTree`: fixed-size storage (no allocation in core), nodes,
  properties, walking, IEEE 1275 path resolution with unit addresses and
  aliases, and `path_of`.
- `of::ClientInterface`: reads IEEE 1275 argument arrays from guest
  memory. Services: test, peer, child, parent, getproplen, getprop,
  nextprop, setprop, finddevice, canon, package-to-path, milliseconds,
  exit, enter. Unknown services are counted for boot logs.
- `core/common/cstring.h`: the few string helpers the freestanding core
  needs.
- Sabotage: 13 mutations. One test was too weak (package-to-path writing
  past buflen) and was tightened; one mutation needed redoing because it
  never triggered. All are caught now.
- 176/176 tests pass.
- Next: let guest code reach the client interface (a CPU hook at the
  client-interface entry point), then instances and memory services.

## Step 2 (same day): guest code reaches the client interface

- CPU: an HLE hook on primary opcode 0 (illegal on the 750). The handler
  claims only its own trap words; everything else still faults.
- `of::Firmware` writes the trap word `0x00004F01` at the entry address
  and services calls: r3 = argument array in, result out, continue at LR.
- Test: a guest program calls `finddevice` and then an unknown service
  via `bctrl`, and stores both results; other opcode-0 words (including
  0) still raise a program exception.
- Sabotage: not returning to LR, and claiming every opcode-0 word, are
  both caught. 178/178 tests pass.

## Step 3 (same day): memory and instances

- `of::MemoryMap` (claim/release with exact and aligned claims, merging,
  double-free refusal) and `of::InstanceTable` (ihandles, per-package C++
  drivers, device-specifier arguments).
- Drivers: `DiskDriver` (byte-addressed seek/read/write over a
  BlockDevice, per-instance position, read-modify-write) and
  `ConsoleDriver` (output to a sink).
- Client services: claim, release, open, close, read, write, seek,
  instance-to-package, instance-to-path; `test` reports only the parts
  that are attached.
- Two of my test expectations were wrong (a miscounted path length, and
  0x9000 being 4 KB-aligned itself); the code was right.
- Sabotage: 11 mutations. Two first went unnoticed: read-modify-write,
  because the test checked the wrong neighbour; and a shared disk
  position, because the test image's byte pattern repeated so the wrong
  offset read the same bytes. Both tests are tightened and all mutations
  are caught now.
- 181/181 tests pass.
