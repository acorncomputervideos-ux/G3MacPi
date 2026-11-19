# CPU interpreter performance

How fast the PowerPC 750 interpreter runs, how it was measured, and what
has been done about it.

## Measuring

- Use a **Release** build (`build/release`). `build/host` is built with
  AddressSanitizer and UBSan and runs about 6× slower; its timings say
  nothing about the emulator.
- Workload: `g3_rom_probe <Mac OS ROM> 4000`, i.e. the first 4 s of guest
  time of a Mac OS 9 start-up (1.4 G guest instructions, ending at the
  same machine state every run).
- Profiles: `valgrind --tool=callgrind` on a Release build with `-g`
  (gprof's per-call overhead distorts these small functions).

## Results (x86-64 development container, one core)

| Change | 4 s of guest time | Guest MIPS |
|---|---|---|
| Before | 22.5 s | 62 |
| Direct RAM path + inline TLB hit | 15.0 s | 93 |
| + fetch-page cache | 13.4 s | 105 |

The Raspberry Pi 400 (Cortex-A72, 1.8 GHz) is expected to be several
times slower than this host; not measured yet.

## What the fast paths do

- **Direct RAM window** (`MemoryInterface::direct_window`). The bus
  exposes main RAM (the RAM region at physical 0) as one host block. The
  CPU fetches, loads and stores inside it with plain host loads and
  stores, skipping the bus's region lookup and virtual calls. Everything
  else (ROM, MMIO, unmapped) takes the bus path as before. The window is
  re-read at the start of every `run()` call.
- **Inline soft-TLB hit** in `translate()`. Misses and first stores to a
  page still go to `translate_enabled()` (R/C bits, protection).
- **Fetch-page cache.** The host pointer of the RAM page holding the
  current instruction stream, keyed by effective page and MSR[IR|PR].
  Dropped by `flush_tlb()` (every SR, BAT, SDR1 write and tlbie) and at
  the start of `run()`. Instructions are still read from guest RAM on
  every fetch, so stores to code are seen at once.

Tests: `tests/unit/cpu/test_ppc750_mmu.cpp` (all MMU tests run through
`run()`, plus `fetch_cache_follows_ibat_change`,
`fetch_cache_follows_msr_ir`, `fetch_sees_stores_to_its_own_page`).

## Where the time goes now

About 150 host instructions per guest instruction. It is spread over
dispatch (`execute` and its per-opcode functions with their prologues),
translation (about 20 per lookup) and load/store handling. Large further
gains need a different execution model: a decoded-block cache or a JIT.
