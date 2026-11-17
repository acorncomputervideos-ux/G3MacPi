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
| Floating point | **Not started** (Milestone 2b): FP instructions raise FP-unavailable when MSR[FP]=0, illegal otherwise | — |
| MMU (BAT, segments, hashed page table), DSI/ISI | **Not started** (Milestone 3): addresses are physical | — |
| Disassembler | Not started (452 vectors available) | — |

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
