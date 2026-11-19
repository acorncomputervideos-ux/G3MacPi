# Decoded-block cache (JIT stage 1)

`core/cpu/ppc750/block_cache.{h,cpp}`, `core/cpu/ppc750/ppc750_blocks.cpp`.

## What it does

A *block* is the straight-line code that starts at one physical address
and runs up to the first block-ending instruction, the end of its 4 KB
page, or 32 instructions. Each instruction is decoded once into an
operation: a handler and the instruction word. Later visits replay the
operations, with no fetch, translation or decode.

- **Block-ending instructions:** anything after which the next PC, the
  MSR or the address translation can change. That is branches, `sc`,
  traps, `rfi`, `isync`, `mtmsr`, `mtsr(in)`, `mtspr`, `tlbie(a)`,
  `tlbsync`, `sync` and `icbi`, plus HLE trap words and illegal opcodes.
- **Specialised handlers:** the most common instructions (`addi(s)`,
  `ori(s)`, `andi.`, `rlwinm`, `cmp(l)(i)`, `add`, `subf`, `and`, `or`,
  `mflr`, and the D-form integer loads and stores) skip `execute()`'s
  decoding switches. Everything else goes through `execute()`.
- **Packing:** blocks are packed at their exact length in a pool inside
  caller-supplied memory, with no allocation. A direct-mapped table
  (32K entries) finds them by physical address. When the pool is full,
  everything is flushed.

Enable it with `YosemiteConfig::jit_memory` / `jit_size` (a few MB). The
probe takes `G3_JIT=MB`. It is off by default.

## Same results as the interpreter, exactly

Time is counted in instructions, so the cache must match `step()`
instruction for instruction:

- **Interrupts:** pending interrupts are taken by `step()`, at the same
  boundaries.
- **Fetch translation and code watch:** done under the same conditions
  as the interpreter's fetch-page cache.
- **Exit flag:** `exit_` makes a block stop after the current operation.
  It is set by every exception, a checkstop, `break_run()`, an interrupt
  input becoming pending (`set_external_interrupt(true)`, decrementer),
  and blocks being dropped.
- **Budget:** the `run()` instruction count cuts blocks short.
- **Code changes:** every RAM page has a generation number; a block is
  valid only under the generation it was built with. Writing to a page
  that holds code bumps that generation. Every write path reports:
  - CPU stores on all their paths, including page-crossing stores,
    `stwcx.` and page-table R/C updates;
  - writes through `PhysicalBus`, via a write observer;
  - DMA, via `notify_write`: DBDMA input and CMD646 transfers to memory.

  Because of this, `icbi` is not needed for correctness. Real hardware
  needs it; this model, like the interpreter, is always coherent.

## Tests

`tests/unit/cpu/test_block_cache.cpp`:

- **Random programs:** 600 programs over the low 32 KB, exception vectors
  included. Many of their stores land in the code. They run in real mode
  and BAT-translated, with a normal cache and with a cache that flushes
  constantly. Interrupts and time-base advances arrive between slices of
  random length, so they land mid-block. After every slice, registers,
  instruction counts and all of memory are compared with the interpreter.
- **Directed tests:**
  - self-modifying loop;
  - page-crossing and `stwcx.` stores into code;
  - blocks at the page end;
  - code watch;
  - decrementer and external interrupts raised inside a block;
  - `break_run()` and checkstop inside a block;
  - bus and DMA write reports (`test_physical_bus`, `test_dbdma`, `test_cmd646`).

Each safety mechanism and each specialised handler was sabotaged in turn,
and the tests caught every one.

The Mac OS ROM boot (3 s of guest time, 1.05 × 10⁹ instructions) ends
with identical registers and RAM (FNV-1a of all 256 MB) with and without
the cache.

## Speed (x86 host, release build)

| ROM boot, 3 s guest | wall time |
| --- | --- |
| interpreter | 12.2–12.6 s |
| block cache, 8 MB | 11.9–12.2 s |

About 0.8 s of each run is start-up; the rest is ~90 MIPS. Under
callgrind the cache executes 20% fewer host instructions (28.8 × 10⁹
against 36.2 × 10⁹ for a 0.6 s window). Wall time barely moves because:

- Blocks are short: about 2.7 instructions per entry in Mac OS code.
- Operations are still indirect calls.
- Much of the remaining time is shared with the interpreter: firmware
  HLE memory access, bus lookups and translation misses.

A larger arena does not help; it scatters blocks over more memory.

The Pi's Cortex-A72 has not been measured; its branch prediction and
caches differ, so the result may differ there.

## Next (stage 2)

Real gains need generated AArch64 code per block:
- registers kept in host registers within a block;
- inline RAM access through the soft-TLB;
- blocks chained directly to their successors.

The decoded-block framework, the invalidation rules and the differential
tests carry over unchanged.
