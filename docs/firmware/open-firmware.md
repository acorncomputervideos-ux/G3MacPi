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
| Instances: open/close/read/write/seek, call-method | to do |
| Memory: claim/release, translations | to do |
| B&W device tree contents (needs a real device-tree dump) | to do |
| Forth subset for the boot script (`interpret`) | to do |

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
