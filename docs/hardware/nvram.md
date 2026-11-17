# mac-io NVRAM — implementation notes

Code: `core/devices/macio/nvram.*`. Tests: `tests/unit/devices/test_nvram.cpp`,
`tests/unit/machine/test_yosemite.cpp`.

The NVRAM holds Open Firmware's settings and the Mac OS parameter data.
Mac OS 9 and Open Firmware expect formatted partitions in it. Our firmware
will create them (a later milestone).

## Facts

Sources: Linux `arch/powerpc/platforms/powermac/nvram.c` and QEMU
`hw/nvram/mac_nvram.c` plus `hw/misc/macio/macio.c` (facts only, no code).

| Fact | Value | Confidence |
|---|---|---|
| Size | 8 KB (0x2000) | Linux `NVRAM_SIZE`, QEMU `MACIO_NVRAM_SIZE` |
| Access styles in Linux | direct (one `reg` entry): byte n at `n * nvram_mult`, where `nvram_mult = region size / 8 KB`. Indirect (two `reg` entries): address port gets `n >> 5`, data at `(n & 0x1F) << 4`. Flash (`nvram,flash`, Core99 and later). | Linux — confirmed |
| Style used by this model | direct, stride 16 (0x20000 window) | QEMU Old World (`it_shift` 4) — **to verify for the B&W** with a device-tree dump |
| Placement | mac-io BAR + 0x60000 | QEMU Old World — **to verify** |
| Wide accesses | split into byte accesses with big-endian lanes | QEMU (`impl.max_access_size = 1`); Linux only uses `in_8`/`out_8` |
| Contents of a blank part | zeros | assumption; our firmware formats it anyway |

If the B&W turns out to use the indirect style, only the address decoding
in `MacioNvram` changes.

## Persistence

`data()` and `load()` expose the 8 KB image. `dirty()` reports whether the
guest changed any byte since the last `load()` or `clear_dirty()`, so the
platform layer can save it (for example to `SD:/g3macpi/nvram.bin`; not
wired up yet). Writing a byte's existing value doesn't set the dirty flag.

## Tests

- All 8192 bytes, using the Linux direct-access formula, including the
  derivation of `nvram_mult`.
- Aliasing between stride points; wide reads and writes, including ones
  that straddle two cells.
- load, dirty and clear semantics.
- Inside Paddington: the last byte through the BAR, unmapping together
  with the BAR, and contents kept while unmapped.
