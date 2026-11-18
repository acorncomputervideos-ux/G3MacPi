# Our high-level Open Firmware — implementation notes

Code: `core/firmware/openfirm/`. Tests: `tests/unit/firmware/test_openfirm.cpp`.
Decision: ADR-0002 (our own HLE firmware; no Apple boot ROM needed).

On a real Mac, Open Firmware (IEEE 1275) describes the hardware to the
operating system as a **device tree**. Mac OS's boot code reads it, and
calls the firmware through the **client interface**. Our firmware
provides both from C++, so no Apple boot ROM is needed. Mac OS itself
(the `Mac OS ROM` file from the System Folder) still comes from the user.

## Status

| Part | State |
|---|---|
| Device tree storage, walking, properties, paths, aliases | ✅ step 1 |
| Client interface: tree services, test, milliseconds, exit/enter | ✅ step 1 |
| Reaching the client interface from guest code (CPU hook) | ✅ step 2 |
| Instances: open/close/read/write/seek, instance-to-package/-path; disk and console drivers | ✅ step 3 |
| Memory: claim/release | ✅ step 3 |
| Disk partitions (Apple Partition Map): `hd:N` opens partition N | ✅ step 4 |
| Files on HFS Plus partitions: `hd:N,\\:tbxi` opens the boot file | ✅ step 5 |
| Files on classic HFS partitions | ✅ step 6 |
| CHRP bootinfo parser (the boot file's text header; see bootinfo.md) | ✅ step 7 |
| call-method and disk methods (block-size, read-blocks, ...) | to do |
| Virtual-memory services (translations, map) | to do, if Mac OS needs them |
| B&W device tree contents (needs a real device-tree dump) | to do |
| Forth interpreter: language core (forth.md) | ✅ step 8 |
| Firmware words: device tree, encoding, packages, `load` (forth-words.md) | ✅ step 9 |
| ELF init-program and `go` (elf-loader.md) | ✅ step 10 |
| `boot`, `init-program`, `go`: bootinfo scripts and ELF programs | ✅ step 11 |
| `interpret` and `call-method` client services (through the Forth interpreter) | ✅ step 12 |
| The B&W device tree; wiring into the machine | to do |

## Facts used

Sources: IEEE 1275-1994 (core) and the PowerPC processor binding. Where
the standard leaves something open, the behaviour Linux's `prom_init.c`
expects as a client is noted (facts only, no code).

| Fact | Source |
|---|---|
| Tree of packages; each has a `name` property; property values are byte arrays, integers as big-endian 32-bit cells, strings with a trailing NUL | IEEE 1275 |
| Path syntax `/name@unit:args/...`; no unit address → first child with that name; leading component without `/` is an alias in `/aliases` | IEEE 1275 |
| Client call: argument array of cells = service-name address, N-args, N-returns, args, returns; call result 0 = done, −1 = unknown service | IEEE 1275 §6.3.1 |
| `peer(0)` = root; `peer`/`child`/`parent` return 0 when there is none | IEEE 1275 |
| `getproplen`/`getprop` return −1 for a missing property; `getprop` copies at most buflen bytes and returns the full size | IEEE 1275 |
| `nextprop`: previous = null or "" → first; returns 1 (name written), 0 (no more), −1 (previous not a property); buffer holds 32 bytes | IEEE 1275 |
| `finddevice` returns −1 if not found | IEEE 1275 |
| `package-to-path`/`canon` return the length of the full path, without the NUL | IEEE 1275; Linux appends the NUL itself and handles a length ≥ buflen as truncation |

## How guest code reaches the firmware

The PowerPC binding passes the client-interface entry address to the
client in r5. The client calls it with the argument array in r3 and gets
the result back in r3. Our entry point is one emulator-defined trap
word, `0x00004F01`, written into guest memory by `of::Firmware::install`.

Primary opcode 0 is illegal on the 750, so no real code executes such a
word. The CPU asks its HLE handler before raising the program exception.
The handler claims only its own trap words, runs the client interface in
C++, puts the result in r3 and continues at LR, as a real entry's `blr`
would. Every other opcode-0 word, `0x00000000` included, still faults as
on real hardware (tested).

Open question, to be answered by logging Mac OS's calls: whether the
boot code calls with address translation on. For now the argument
addresses are physical.

## Memory and instances (step 3)

| Fact | Source |
|---|---|
| `claim ( virt size align -- base )`: align 0 claims exactly at virt; otherwise the firmware picks an aligned free area; −1 on failure. `release ( virt size -- )` | IEEE 1275 + PowerPC binding |
| `open ( device-specifier -- ihandle )` returns 0 on failure; the arguments of the last path component go to the package's open | IEEE 1275 |
| `read`/`write ( ihandle addr len -- actual )`, −1 on error; `seek ( ihandle pos.hi pos.lo -- status )`, 0 ok / −1 error | IEEE 1275 |
| `instance-to-package ( ihandle -- phandle )`; `instance-to-path ( ihandle buf buflen -- length )` | IEEE 1275 |

Choices:
- claim maps 1:1 (real mode), doesn't round sizes, and picks the
  lowest aligned free area. Real Apple firmware may allocate top-down;
  that will show up in logs if it matters.
- The memory map refuses a double free. Ranges are kept merged.
- ihandles are `0x20000000 + slot` (32 instances).
- Arguments of the last component start at its first `:`. For example,
  `hd:9,\\:tbxi` passes `9,\\:tbxi`. `instance-to-path` appends `:args`
  (exact Apple formatting to verify).
- `DiskDriver`: with a leading number in the arguments (`9` or
  `9,file`), the instance sees only that Apple Partition Map partition
  (offset 0 = its first byte); otherwise it sees the whole disk. The file
  part isn't interpreted yet. Access is at byte granularity, with
  read-modify-write for partial sectors. Reads at the end return fewer bytes, then 0.
- `ConsoleDriver`: writes go to a sink (the boot log); reads return 0
  bytes. The value Apple's firmware returns when no key is waiting is
  to verify.
- A service whose part isn't attached (no memory map or instance table)
  is reported missing by `test` and fails as an unknown service.

## File paths (step 5)

After the partition number and a comma, the arguments name a file on the
partition's HFS Plus or HFS volume (`core/storage/hfs_volume`):

| Syntax | Meaning |
|---|---|
| `\\` (two backslashes) at the start | the blessed System Folder (volume header finderInfo[0]) |
| `\` at the start | the root folder |
| `name\name` | path components, ASCII case-insensitive |
| `:TYPE` (last) | the first file of Finder type TYPE in the folder reached so far |

So `hd:9,\\:tbxi` opens the file of type `tbxi` (the `Mac OS ROM`
file) in partition 9's blessed folder. This is how Mac OS 9 boot paths
look, but the exact grammar Apple's firmware accepts is to verify. File
instances are read-only; seek is limited to the file size. A file part
on a partition without HFS Plus, a missing file, a folder, or a file in
the middle of a path makes `open` fail.

## interpret and call-method (step 12)

`interpret ( cmd arg1..argP -- catch-result ret1..retQ )` and
`call-method ( method ihandle arg1..argP -- catch-result ret1..retQ )` run
through the Forth interpreter. Stack order: **arg1 ends up on top of the
Forth stack** (argP is pushed first), and **ret1 is the top of the stack**
afterwards. Evidence: Linux's `prom_init.c` calls the `color!` method
( r g b index -- ) with the arguments `index, b, g, r`, and the memory
node's `claim` method ( virt size align -- ) with `align, size, virt`.
Missing results read as 0, leftover stack items are discarded, and an
error's throw code is the catch-result.

`call-method` runs `$call-method`, so it reaches the same C++ driver
methods (read, write, seek) as Forth code does.

## Modelling choices

- phandles are `0x10000000 + node index` (any non-zero value is allowed).
  Mac OS may treat phandles as pointers into firmware memory; if logging
  shows that, the base can move.
- Each node stores its unit-address text. A real firmware derives it from
  `reg` with the parent's `encode-unit`; our tree builder must write it to
  match `reg`. Unit addresses compare as comma-separated hex numbers.
- Node and property names are case-sensitive.
- `package-to-path`/`canon` also write a NUL when it fits inside buflen,
  never beyond it.
- Fixed storage: 256 nodes, 2048 properties, a 96 KB pool (about 140 KB
  in all). Replacing a property doesn't reclaim the old value's bytes.
  Running out clears `ok()`.
- Unknown services are counted, and the last service name is kept, so
  boot logs show exactly what Mac OS asks for next.

## Tests

- Building, walking, property order, replacing a property, name-length
  limit, capacity limits.
- Path resolution: units with and without leading zeros, case, arguments,
  repeated slashes, aliases with and without a remainder; `path_of`
  truncation.
- Client interface with argument arrays in guest RAM, called the way
  Linux's `prom_init` calls: a full tree walk with `peer`/`child` and
  `package-to-path`; `getprop` truncation; a `nextprop` walk; `setprop`;
  `finddevice` with an alias; `canon`; `test`; unknown services;
  `milliseconds`; `exit`; invalid phandles; too few return cells.
