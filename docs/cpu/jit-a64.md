# JIT stage 2: AArch64 code for blocks

`core/cpu/ppc750/jit/a64_assembler.*`, `core/cpu/ppc750/ppc750_jit.cpp`,
`tools/a64sim/`. This builds on the decoded-block cache
([block-cache.md](block-cache.md)).

## What is generated

Each block of the block cache gets one function, `void(Ppc750*)`.

- **Native instructions:** simple integer instructions are done in
  generated code, reading and writing the `CpuState` in memory:
  - `addi`, `addis`;
  - `ori`, `oris`, `xori`, `xoris`;
  - `rlwinm`, `rlwimi` (with Rc too);
  - the whole add/subtract family (`add`, `addc`, `adde`, `addme`,
    `addze`, `subf`, `subfc`, `subfe`, `subfme`, `subfze`, `neg`, with
    OE and Rc): as `op_add_family`, x + y + carry-in in 64 bits, CA from
    bit 32, OV from the operand and result signs, SO sticky, CR0 last;
  - `and`, `or`, `xor` (with Rc too);
  - `mtspr` to LR, CTR and XER (XER masked as the interpreter does);
    these no longer end a block;
  - `cmp`, `cmpl`, `cmpi`, `cmpli`, `andi.`, `andis.`, `rlwinm.`: the CR
    field is set with `cset` and `bfi` from the AArch64 flags, plus
    XER[SO];
  - `mflr`, `mfctr`;
  - branches at the end of a block: `b`, `bc`, `bclr`, `bcctr`, with
    LK and AA. The CTR decrement and test, the CR bit test, and the
    target read before LK updates LR are as in the interpreter. Then the
    PC and the whole count are written;
  - the D-form integer loads and stores (`lwz`, `lbz`, `lhz`, `lha`,
    `stw`, `stb`, `sth` and their update forms; see below).
- **Everything else** calls its stage-1 block handler, the same function
  the block cache runs. Results therefore come from the interpreter's own
  code.
- **PC and instruction count:** native instructions do not update them.
  They are written before each handler call, so handlers and anything
  they call (time hooks, exceptions) see exact values, and again at the
  end.
- **Exit checks:** only handlers can raise exceptions or set `exit_`.
  The flag is checked after each handler call; when it is set, the
  function returns at once.
- **Registers:**
  - x19 holds the CPU, x20 the `CpuState`;
  - w21 holds the PC at entry (a block is found by physical address and
    may be entered at any effective address that maps to it);
  - w8–w12 are scratch; x16 holds call targets, x17 far addresses.
- **Code buffer:** filled linearly and emptied together with the block
  cache (on its flushes, and through a flush when it runs out).
- **When it runs:** only whole blocks run as generated code. Near the end
  of a `run()` budget, a block runs operation by operation instead.

### Loads and stores

The generated fast path repeats `mem_fast`'s checks inline, in the same
order:
1. the access stays within its 4 KB page;
2. real mode, or a data-TLB hit (the tag includes MSR[PR]; stores also
   need the writable and changed bits);
3. the physical address lies inside the RAM window;
4. for stores, the page holds no blocks.

Then the access itself, byte-swapped (`rev`/`rev16`). Anything else runs
the instruction's handler, which does the whole instruction again;
nothing has been changed before that decision. Pages with blocks are left
to the handler, so `note_write` sees every store to code. A memory
instruction is a sync point: PC and count are written before it and,
on the fast path, after it. `JitStats::memory_slow` counts the handler
calls. In 0.6 s of the ROM boot, 129,529 loads and stores took the
handler.

Configuration: `YosemiteConfig::jit_code` / `jit_code_words`, and
`jit = yes` in `g3macpi.ini` on the Pi (block cache and code). Without a
runner, the code is called directly; that is only possible on an
AArch64 host.

## Chaining (block to block without returning)

At its end, a block's code finds the next block itself and jumps into it,
instead of returning to `run_blocks`. The sequence (`JitCompiler::chain`)
makes the checks `run_blocks` makes before running a block, and returns
to it if any fails:

1. `exit_`, `break_run_` (an `mtspr DEC` sets it without `exit_`) and
   `checkstop_` are clear;
2. no interrupt is pending that MSR[EE] lets in;
3. the fetch page is still valid: same effective page, same MSR[IR|PR];
4. the next PC's physical address has a block in the cache's table, with
   the current page generation (not dropped) and code;
5. it fits the run's budget (`instr_count_ + count <= jit_limit_`, which
   `run_blocks` sets from what is left before calling the code).

Then it jumps (`br`) to that block's code past its prologue (word
`kChainEntry` = 11: every prologue is padded to that length), keeping the
first block's stack frame; the last block's epilogue returns to
`run_blocks`. Computed branches (`bclr`, `bcctr`) go through the same
lookup, so they chain too. `JitStats::chained` counts the jumps.

Checks: the differential tests (random programs, instruction counts and
memory identical to the interpreter at every slice end) plus
`chained_blocks_stop_where_run_blocks_would` (every budget from 1 up; an
`mtspr DEC` in the loop) and `chaining_follows_an_msr_change` (IR turned
on: the same effective address is another physical page). Sabotage runs,
each check removed in turn: `break_run_`, the interrupt test, the fetch
page, the generation, the budget (the simulator then runs past the end),
MSR[IR|PR] and a wrong entry offset (the stack moves) are all caught. A
missing `exit_` test is not: every way `exit_` is set is also seen by
another check, so it is kept as a cheap safeguard. The Mac OS ROM boot
(1.05 × 10⁹ instructions on the simulator) ends with the same registers
and RAM hash as the interpreter; run_blocks entered generated code 10 562
times (it was about 3.9 × 10⁸ times without chaining).

## Register cache, and cheaper chaining (2026-11-24)

Measured on the CD boot (6 guest seconds, 1.59 × 10⁹ instructions) with the
generated code on the simulator, which counts the AArch64 instructions it
executes (`g3_host` prints them with `G3_JITSIM=1`):

| step | AArch64 instructions | returns to run_blocks |
|---|---|---|
| before | 54.6 × 10⁹ | 194 M (46% of block ends) |
| register cache | about the same (+2%) | 194 M |
| chaining across pages (inline ITLB) | 66.4 × 10⁹ (*) | 10.9 M |
| small fields first in Ppc750 | 61.1 × 10⁹ | 10.5 M |
| known targets, fields for pointers, one stop flag | 52.5 × 10⁹ | 10.3 M |

(*) work run_blocks did in C++ (not counted by the simulator) moved into
generated code. Every step ends the 6 s with the same screen, pixel for
pixel, and the Mac OS ROM boot with the interpreter's registers and RAM.
Mac OS's blocks are short (3.7 instructions on average), so the work
between blocks dominated:

- **Register cache.** The most used GPRs of a block live in x25-x28 (and
  x22-x24 without loads/stores), loaded at first use, written back before
  handler calls, a memory access's slow path and the block's end, and
  reloaded after a handler. Hot instructions (addi, logic, rotates,
  compares, loads/stores) work on them directly: a run of `addi r3,r3,1`
  costs one AArch64 instruction each (three before;
  `cpu/jit/register_cache_keeps_values_in_registers`). With blocks this
  short it saves little yet; it pays off as blocks get longer.
- **Chaining across pages.** On a fetch-page change the chain sequence now
  does run_blocks' miss itself when its fast case applies (no code watch;
  real mode or an ITLB hit; a page of RAM) and updates the fetch page.
- **Layout.** The large members of Ppc750 (CpuState, the two TLBs, the
  profile) come last, so the small fields read between blocks are within
  one load's reach of x19 (they used to need 4-5 instructions each).
- **Known targets.** For a final b/bc whose targets (taken, fall-through)
  lie in the block's page, the next block is looked for at a fixed table
  slot and checked against fixed values (its physical address; the
  page's generation, which is this block's own as long as exit_ is clear):
  no fetch-page work, no generation lookup.
- `mtspr DEC` now sets `exit_` as well as `break_run_`, so one flag stops
  chaining; table, pool, generation and code-buffer pointers are fields
  (one load each, not a 64-bit constant).

Sabotage runs: no write-back before a handler, no reload after one, none
around the slow path, none at the block's end, x25 not saved, the ITLB
tag check, the code-watch check, the RAM check (a crash), the known
target's offset, address and generation checks, and `mtspr DEC` not
setting `exit_`: all caught (the slot check by
`chained_static_target_slot_collision`, added for it).

## Code size and the block table (2026-11-24, measured in QEMU)

Running the Pi kernel in QEMU (docs/qemu-pi.md) showed 26-28% of the Mac's
instructions running without generated code. With compiling on another
core, every block runs through its handlers until its code arrives; and
blocks were compiled over and over (CD boot at a 10 MHz effective clock,
5 × 10⁸ instructions, `g3_host` with `G3_JITSIM=1`):

| | blocks compiled | code buffer resets | words per block |
|---|---|---|---|
| before | 3.2 M | 1262 | 205 |
| shared stubs (dispatch, enter, epilogue) | 1.86 M | 397 | 112 |
| + shared entry and known-target stubs | 1.49 M | 253 | 89 |
| + table scaled to the arena (8 MB pool, 2 MB code) | 298 k | 63 | |
| + 64 MB pool, 4 MB code (the Pi now) | 65 k | 7 | |

- **Shared code.** The general lookup ("dispatch"), entering a found block
  ("enter"), the known-target check ("static"), the entry from run_blocks
  ("entry": pushes the frame, then jumps to `jit_entry_`) and the epilogue
  are emitted once at the start of the code buffer
  (`Ppc750::jit_emit_stubs`); blocks have no prologue and end in branches
  to them. Buffer resets keep them.
- **The block table** was direct-mapped with 2^15 slots: blocks 128 KB apart
  evict each other, and Mac OS keeps about 10^5 blocks busy, so most
  "compiles" were rebuilds of evicted blocks. It now takes an eighth of the
  cache's memory (2^15 to 2^22 slots).
- **Requests dropped** when the worker's queue was full are asked again
  (a block still without code 2^20 instructions after its request).
- The Pi's block cache is 64 MB (of a ~2 GB heap) and its code buffer 4 MB.

## Profile: what still goes to handlers

Generated code counts each instruction it hands to a handler, per kind
(`Ppc750::jit_op_id`: the primary opcode, plus the extended opcode for
opcodes 19, 31, 59 and 63), and keeps one instruction word of each kind
for its name. `JitStats::interpreted` counts what `run_blocks` ran
without generated code (blocks whose code is not ready, the end of a
budget). `platform/common/jit_report.h` turns this into one line:

    JIT: 66.4% native, 33.6% handlers, 0.0% interpreted; most handled: mullw 32.9%, mtspr 0.7%

The Pi writes it to the log every 10 s with the speed line, and the web
page shows it under Status ("CPU emulation"); `g3_host` prints it every 10
guest seconds (with `G3_JITSIM=1`: generated code on the simulator) and
`g3_rom_probe` at the end. It costs four instructions per handler call.
The kinds handled most are the ones to compile natively next.

### Profile of the Mac OS 9.2.1 CD boot (30 guest seconds, dozing on)

| | native | handlers | most handled |
|---|---|---|---|
| chaining only | 67.0% | 33.0% | mtspr 7.5%, rlwimi 6.1%, lhau 5.9%, lha 1.7%, addco. 1.7% |
| + mtspr LR/CTR/XER, rlwimi, lha/lhau, add family | 90.8% | 9.2% | mfspr 1.5%, lbzx 1.5%, lwzx 0.9%, mtcrf 0.7%, lhaux 0.5% |

(`mtspr` in that boot: LR 113 M, CTR 36 M, XER 2.9 M; all other SPRs
together under 6 M.) Checks: the differential tests' random programs
contain all of these (rlwimi with Rc, the add family with random OE and
Rc, lha/lhau, mtspr XER/LR/CTR); sabotage runs (mask, CA position, SO,
carry-in, ~rA, sign extension, XER mask, OV operands) are all caught.
The ROM boot on the simulator still ends with the interpreter's
registers and RAM hash; the CD boot's screen after 30 s differs only in
the menu-bar clock.

## On the Pi (Circle)

- **Executable memory:** Circle maps everything after `_etext` as not
  executable (PXN), the heap included, and all memory read/write. The
  code buffer (2 MB) is therefore defined in assembly in a section
  `.text_g3jit`, which the linker script's `*(.text*)` places before
  `_etext`. The image grows by 2 MB. In the image built here,
  `g3_jit_code` is at 0xA0000 and `_etext` at 0x31D65C.
- **Cache maintenance:** after each block is written, the data cache is
  cleaned to the point of unification and the instruction cache
  invalidated (`dc cvau`, `ic ivau`, `dsb ish`, `isb`; the line sizes
  come from CTR_EL0).

Native execution has been tested in QEMU's raspi4b (see Background
compilation); not yet on a real Pi.

## Checking it without an ARM machine

- **Encodings:** `tests/unit/cpu/test_a64_assembler.cpp` checks every
  encoder function against words produced by the GNU AArch64 assembler.
  The cases are in `a64_cases.inc`; `tools/jit/a64_vectors.sh`
  regenerates `a64_golden.inc` from them.
- **Simulator:** `tools/a64sim` simulates exactly the emitted subset, from
  the Arm ARM definitions. It is strict, and each check has a test in
  `test_a64_sim.cpp`. These are errors:
  - any other encoding;
  - a call with a misaligned stack;
  - returning with SP moved or with a callee-saved register (x19–x29)
    changed.

  Also, after each host call it overwrites the caller-saved registers
  (x0–x18), so code that wrongly relies on them fails.
- **Differential tests:** every block-cache test also runs as `cpu/jit/*`,
  with the generated code on the simulator, plus a test where the code
  buffer keeps running out. The random tests require that generated code
  actually ran (block runs and natively compiled instructions are
  counted). 15 sabotages of the code generator were all caught (swapped
  operands, a missing PC write, wrong counts, a missing exit check, a
  wrong rotate, the rA = 0 rule, Rc forms compiled natively, a wrong
  register restore, a missing code-buffer reset, and others).
- **Memory path:** 14 more sabotages, all caught:
  - byte swaps, the tag compare, the writable check, the PR bit, the
    code-page check;
  - the page-crossing check, caught by an access that leaves BAT1's
    window and by a store that spills onto a page with compiled code;
  - the RAM bound, real/translated selection, the TLB index, update
    writes and syncs.

  The TLB-index sabotage only costs speed. It is caught by
  `generated_loads_and_stores_take_the_fast_path`, which requires
  that almost all accesses in a loop are done in generated code.
- **ROM boot:** `G3_JIT=8 G3_JITCODE=8 g3_rom_probe` runs the Mac OS ROM
  boot with generated code on the simulator. Over 3 s of guest time
  (1.05 × 10⁹ instructions, 3.9 × 10⁸ block runs in generated code) it
  ends with the same registers and RAM hash as the interpreter.

## Background compilation (another core)

`core/cpu/ppc750/jit_worker.h`. With a `JitWorker` attached
(`Ppc750::set_jit_worker`, `YosemiteConfig::jit_background`), the CPU
doesn't compile a block when it builds it: it queues a copy of the
block's operations (`JitRequest`, single-producer/single-consumer queue)
and keeps running the block through its handlers. The worker, polled on
another core, compiles into the code buffer (which it owns from then on)
and queues a `JitResult`; the CPU collects results before looking up each
block and gives the block its code.

- **Epochs.** Each request carries the block cache's flush count. A result
  is used only if the cache hasn't been flushed since; the worker empties
  the code buffer when it sees a newer epoch. The CPU queues requests only
  while building blocks, never inside generated code, so code in use is
  never overwritten. Out of room: the result says so and the CPU flushes
  at its next block build, as without a worker.
- **Memory ordering.** The worker cleans the data cache and invalidates
  the instruction cache for what it wrote (as before); the queues publish
  with release/acquire; the CPU executes `isb` before running code it
  collected. On AArch64 the CPU executes `sev` after each request, so an
  idle worker core waits in `wfe`.
- **Tests.**
  - Every block-cache differential test also runs as `cpu/jitbg/*`, with
    the worker on another thread (code arrives whenever the scheduler
    allows; results must be identical).
  - `stale_code_is_never_installed` builds the dangerous case on purpose:
    a result made before a flush arriving after a new block for the same
    address was built in the same pool slot. Sabotage: without the epoch
    check, that test fails (the random tests did not catch it).
  - The Pi's CPU self-test has a background mode (`worker_attach`), tested
    on the host with a thread.
- **Mac OS boot.** `G3_JIT=16 G3_JITCODE=8 G3_JITBG=1 g3_rom_probe` with
  the Mac OS 9.2.1 CD, 4 s of guest time (1.4 x 10^9 instructions): 2.6
  million blocks compiled on the other thread, 28 stale results dropped;
  the end state (registers, instruction count, RAM hash) is identical to
  the interpreter's and to compiling on the CPU's own thread.
- **Pi.** Core 2 runs the worker (`CDrawCore::RunWorker`); the kernel
  attaches the running Mac's worker after starting it and detaches it
  before stopping it. The log says "JIT compiling: core 2".
- **First run on ARM.** In QEMU's raspi4b (a test build,
  `G3_DEFINES=-DG3_QEMU_DRAWTEST`), the CPU self-test ran the generated
  code natively on (emulated) Cortex-A72 cores for the first time: 20/20
  programs identical compiling on core 0, and 20/20 compiling on core 2.

## Coverage, and what comes next

Native share of compiled operations in that boot: 27% at first, 36% with
loads and stores, 39% with compares and CR0, 77% with branches. The
branch code had 11 sabotages. Ten were caught by the existing tests;
the eleventh (`bcctr` with BO[2] = 0 decrementing CTR) needed a new
directed test, `branch_forms`. The ROM boot still ends
bit-identical. The CR code had 9 sabotages. Eight were caught. The
ninth (compiling OE forms natively) is impossible by construction: OE
is part of the 10-bit extended opcode, so OE forms never match the
native cases.

Next:

1. ~~Linking blocks directly, without returning to the C++ loop.~~ Done
   (chaining, above).
2. Keeping the instruction count in a register between handler calls.
3. More instructions natively: done for `mtspr` LR/CTR/XER, `rlwimi`,
   `lha`/`lhau` and the add family (profile above); next `mfspr` (other
   SPRs), indexed loads and stores (`lbzx`, `lwzx`, `lhaux`), `mtcrf`.

Speed can only be measured on the Pi.
