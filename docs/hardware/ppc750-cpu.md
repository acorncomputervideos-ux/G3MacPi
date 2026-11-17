# PowerPC 750 CPU — implementation notes

Code: `core/cpu/ppc750/`. Tests: `tests/unit/cpu/`.
Sources: PowerPC Programming Environments Manual for 32-bit Implementations
(PEM); MPC750 RISC Microprocessor User's Manual (750 UM). Edge cases the
manuals leave undefined follow the hardware-derived vectors in
`tests/vectors/dingusppc/` where those cover them.

## Status (Milestone 2, step 1 — 2026-11-17)

| Area | Status | Tests |
|---|---|---|
| Integer arithmetic, logical, shift, rotate, compare | **Done** | all 5620 integer vectors pass; unit tests |
| Branches (b, bc, bclr, bcctr; LK/AA; CTR) | **Done** | unit tests |
| CR logical, mcrf, mtcrf, mfcr, mcrxr | **Done** | unit tests |
| Integer loads/stores incl. update, indexed, byte-reversed | **Done** | unit tests |
| lmw/stmw, lswi/lswx/stswi/stswx | **Done** | unit tests (lswx/stswx: not yet) |
| lwarx/stwcx. (32-byte reservation granule) | **Done** | unit tests |
| dcbz (32-byte block); other cache ops are no-ops | **Done** | unit tests |
| SPR access, privilege checks, unimplemented SPR -> illegal | **Done** | unit tests |
| Time base, decrementer (exception on DEC bit 0: 0 -> 1) | **Done** | unit tests |
| Exceptions: program (illegal/privileged/trap), sc, FP unavailable, alignment (lwarx/stwcx./lmw/stmw), machine check / checkstop, external, decrementer; rfi | **Done** | unit tests |
| Floating point (Milestone 2b): all 750 FP arithmetic, fused multiply-add family, compares, frsp, fctiw[z], moves, fsel, fres, frsqrte, FPSCR instructions, FP loads/stores, FP-unavailable and FP-enabled exceptions | **Done** | all 2054 FP vectors pass; 18 unit tests; IEEE core checked against host FPU |
| MMU: IBAT/DBAT (Vs/Vp, BL, PP), segment registers (Ks/Kp/N/T), hashed page table (primary/secondary hash, HTABMASK), R/C updates, page-crossing accesses, DSI (DAR/DSISR) and ISI (SRR1 causes) | **Done** (no TLB cache yet) | 14 unit tests with hand-computed page tables |
| Disassembler (`disasm.h`): simplified mnemonics, freestanding | **Done** | all 397 reference vectors (52 POWER/601/603-only encodings correctly shown as `dc.l`); 1 M random words cross-checked against the interpreter |

## Decisions and known deviations (to verify or complete)

- **divw / divwu overflow results** (architecturally undefined): divw gives
  -1 if the dividend is negative, else 0; divwu by zero gives 0. CR0 (Rc=1)
  follows the result. Matches all vectors.
- **XER** keeps only implemented bits (SO, OV, CA, byte count): mask 0xE000007F.
- **PVR**: upper half 0x0008 = 750 family; the revision (lower half) of the
  B&W G3's CPU is **unknown** — `kPvrMpc750Placeholder` = 0x00080000 until a
  real machine's value is obtained.
- **mtspr PVR** is ignored; **mfspr 268/269** (TBL/TBU) is treated as
  illegal — software must use mftb. Both to be verified against hardware.
- **mtspr DEC** with bit 0 becoming 1 does not raise an exception here.
- **Alignment exceptions**: only lwarx/stwcx. and lmw/stmw with a misaligned
  EA; DSISR is not filled in (DAR is). Other misaligned accesses are done
  as the 750 does for most cases (no exception). Little-endian mode
  (MSR[LE]) is not supported.
- **Machine check** = bus error (memory interface returned failure); SRR0 is
  the faulting instruction. With MSR[ME]=0 the CPU checkstops.
- **Invalid instruction forms** (e.g. update with rA=0) execute like valid
  ones; their result is architecturally undefined.
- Not implemented: eciwx/ecowx, tlbia (not on 750: illegal), trace (SE/BE),
  performance monitor events, IABR/DABR breakpoints, thermal/SMI exceptions.

## Floating point (2026-11-17)

Design: `core/fpu/ieee754.*` is a software IEEE 754 implementation (exact
or sticky-jammed intermediate, one rounding in `round_pack`). It is checked
against the host CPU's FPU on ~2.2 million random operand sets (all four
rounding modes, double and single precision, add/sub/mul/div/fma/sqrt,
conversions) including all status flags. `ppc750_fpu.cpp` applies the
PowerPC rules (NaN precedence frA > frB > frC, VX causes, FR/FI, FPRF,
FX/FEX/VX summaries, CR1, enabled-exception result suppression, FE0/FE1).

Decisions — evidence and confidence:

| Behaviour | Implemented as | Evidence |
|---|---|---|
| FR on overflow | set iff rounding the significand to target precision (unbounded exponent) increments it | vectors (fadd/fadds/fsubs overflow cases) — **confirmed by data** |
| Single-precision ops using frC (fmuls, fmadds, ...) | frC significand first rounded to 25 bits (24 fraction bits), half-up, independent of RN | one vector requires frC shortening; exact rule taken from documented Gekko (750-derivative) behaviour — **INFERRED, verify on a real 750** |
| Underflow tininess | before rounding (configurable: `set_fp_tininess`) | vectors pass either way — **UNDETERMINED** |
| NaN result of a single-precision op | quieted, fraction truncated to single (low 29 bits cleared) | PEM; covered by vectors with NaN inputs |
| fres / frsqrte | correctly rounded values (better than the hardware estimates: fres 1/4096, frsqrte 1/32) | low-order bits will differ from a real 750 — **known deviation** |
| fctiw[z] high word, mffs high word | 0 | architecturally undefined — **verify on hardware** |
| FPSCR[NI] (non-IEEE mode) | ignored (IEEE behaviour) | not emulated |
| Trap-enabled overflow/underflow (OE/UE) | exponent adjusted by 1536/192 (PEM) | not covered by vectors — unit-tested only indirectly |
| Imprecise FP exception modes | treated as precise | Mac OS 9 runs with FE0 = FE1 = 0 |
| fsqrt / fsqrts | illegal (the 750 does not implement them) | 750 UM |

## MMU (2026-11-17)

Implemented from PEM chapter 7. Decisions and gaps:
- Every access translates (BATs first, then segment/page table); no TLB
  cache yet, so `tlbie`/`tlbsync` need no action. **Next step: a soft TLB,
  verified against this uncached path.**
- R is set on every successful access and also on a protection fault; C on
  stores. Updates are written straight to the page table in memory.
- Accesses crossing a 4 KB boundary translate both pages before touching
  memory (no partial stores on a DSI). Exception: FP doubles are two 32-bit
  accesses, so a DSI on the second word can leave the first word written.
- lwarx/stwcx. reservations use the physical address (32-byte granule).
- Direct-store segments (T=1): data access -> DSI with DSISR bit 5,
  fetch -> ISI no-execute. The 750 does not support direct-store; exact DSISR
  bits **to verify**. Mac OS does not use T=1 segments.
- Instruction fetch from a guarded page (PTE G=1) -> ISI bit 3.
- Not yet: dcbz alignment exception for write-through/cache-inhibited
  pages; DABR/IABR breakpoints; the 750's TLB/BAT invalidation timing.

Throughput (`g3_cpu_bench`, 2.1 GHz Xeon container, 2026-11-17):
real mode 105-168 M instr/s (noisy host), BAT-mapped ~90 M,
page-table-mapped ~43 M. Adding the MMU cost ~20 % in real mode
(callgrind: 221 M -> 268 M host instructions per 2 M guest instructions;
forcing `execute` inline made it worse and was reverted).

## Disassembler / interpreter consistency

`tests/unit/cpu/test_decode_consistency.cpp` decodes one million random
instruction words with both independent decoders. Rule: anything the
disassembler accepts must not be illegal in the interpreter. Its first run
found that the disassembler accepted mfspr/mtspr of SPRs the 750 does not
implement (fixed: the disassembler now applies the interpreter's SPR rules).
The converse direction (~6 % of random words) consists only of encodings
with non-zero reserved fields, which the CPU executes like the valid form
(e.g. fmuls with frB != 0, sc with extra bits, bclr with rB != 0).

## Interfaces
- `MemoryInterface`: physical, big-endian guest values; returns false on
  bus error. `FlatMemory` (core/mem) is a contiguous implementation.
- `Ppc750::step()` takes pending external/decrementer interrupts (if
  MSR[EE]) before fetching; `execute(insn)` runs a pre-fetched instruction.
- `advance_timebase(ticks)` drives TB/DEC; the scheduler (Milestone 3/4)
  will call it.

## Performance (first measurement)
`build/release/g3_cpu_bench` (8-instruction integer loop incl. load/store):
~160 million guest instructions/s on the 2.1 GHz Xeon CI container
(2026-11-17). Not a Pi figure. No MMU yet; switch-based dispatch.
