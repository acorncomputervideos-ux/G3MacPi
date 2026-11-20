# Physical bus — implementation notes

Code: `core/mem/physical_bus.*`. Tests: `tests/unit/mem/test_physical_bus.cpp`.

- Up to 32 regions: RAM (read/write, guest byte order), ROM (writes ignored
  and counted), MMIO (`MmioDevice::mmio_read/mmio_write(offset, size)`).
- Overlapping, empty or 4 GB-wrapping mappings are rejected.
- Accesses straddling a region edge are split into bytes, each going to its
  own region (or the unmapped handling).
- Unmapped addresses: `BusError` (default — the CPU takes a machine check) or
  `AllOnes` (reads 0xFF.., writes dropped). **Which one the Blue & White
  G3's Grackle produces for which ranges is not yet researched**; the
  machine model will choose per range once documented.
- ROM writes are ignored without a bus error — to verify against hardware.
- `host_pointer()` gives DMA engines direct access to RAM.
- Lookup: last-hit cache + linear scan. Fast enough for now; a page-indexed
  table can replace it if profiling on the Pi shows the need.
