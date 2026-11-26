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
| + inline data-access fast path (`mem_fast`) | 13.0 s | 108 |

Tried and rejected: link-time optimisation (`-flto`) made it slower
(14.6 s).

Against real time: the emulated G3 runs at 350 MHz with one instruction
per cycle assumed, so a guest second is 350 M instructions. At ~108 MIPS
that is about 3.3x slower than a real Blue & White on this host (g3_host:
25 s of guest time took 100 s). The Raspberry Pi 400 (Cortex-A72, 1.8 GHz)
is expected to be several times slower again; not measured yet.

## What the fast paths do

- **Direct RAM window** (`MemoryInterface::direct_window`). The bus
  exposes main RAM (the RAM region at physical 0) as one host block. The
  CPU fetches, loads and stores inside it with plain host loads and
  stores, skipping the bus's region lookup and virtual calls. Everything
  else (ROM, MMIO, unmapped) takes the bus path as before. The window is
  re-read at the start of every `run()` call.
- **Inline data accesses** (`mem_fast`): an aligned-in-page load or
  store that hits the soft TLB (or runs in real mode) and lands in the
  RAM window is done at the call site.
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

The decoded-block cache (stage 1 of the JIT) is described in
[block-cache.md](block-cache.md).

## Idle: the CPU dozes (2026-11-24)

Sampling the PC while Mac OS 9.2.1 boots from its CD (`G3_SAMPLE_MS` in
`g3_rom_probe`) put 70% of the samples in one seven-instruction loop at
0x0032BC00: Mac OS's idle loop (r25 holds "idle"). It sets HID0[DOZE] and
then, over and over, `sync; mtmsr r10; isync` with MSR[POW] and EE in r10.
On a real 750 that stops the processor until an interrupt (750 UM 10.2,
"Power management": MSR[POW] with HID0 DOZE, NAP or SLEEP selected); we
used to run the loop flat out instead.

Now `Ppc750::dozing()` is true while MSR[POW] is set, HID0 selects a
power-saving mode and no interrupt can be taken; `run()` executes nothing
then (interpreter, block cache, and chained generated code, which returns
to `run_blocks` when MSR[POW] is set), and `ExecutionLoop::run_for` lets
guest time pass to the next event or decrementer expiry, which wakes it.
Taking the interrupt clears MSR[POW] (SRR1 does not keep it).

Effect on the CD boot, 20 guest seconds: 2.08 × 10⁹ instructions instead
of 7.0 × 10⁹, the same screen at the end (55 pixels differ: the clock
and the cursor), and 24 s instead of 67 s on the development machine for
25 guest seconds. On the Pi the guest pacer then waits instead of
spinning, so an idle Mac leaves the core free (cooler), and the speed
measurement no longer counts the idle loop as work.

Checks: `machine/loop/dozing_cpu_lets_time_pass` (time passes without
instructions until the decrementer; without HID0[DOZE] POW does nothing),
`cpu/*/dozing_stops_until_an_interrupt` (interpreter, block cache, JIT and
background JIT identical; nothing runs while asleep; an external
interrupt wakes it). Sabotage: ignoring HID0, `run_blocks` not stopping,
chained code not returning at MSR[POW] are caught by those tests; the
execution loop not letting time pass makes the test hang (stopped by
the timeout).
