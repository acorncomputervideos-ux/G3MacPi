# CPU self-test

`platform/common/cpu_selftest.{h,cpp}`. On the Pi it is the last item of
the settings menu, "CPU self-test". On a PC, run `g3_host --selftest
[programs]`.

## What it does

1. **Correctness.** 200 random PowerPC programs, the generator of
   `tests/unit/cpu/test_block_cache.cpp`:
   - they are self-modifying and cover the exception vectors;
   - half run in real mode, half translated through BATs;
   - interrupts and time-base advances arrive between slices of random
     length, so they land mid-block.

   Each program runs on the interpreter and on the block cache with
   generated code. After every slice, registers, instruction counts and
   all memory (1 MB) must be identical. On the Pi the generated AArch64
   code runs natively; that cannot be checked on a PC. On a PC it runs on
   the A64 simulator.
2. **Speed.** A fixed loop (load, add, rotate, xor, store, compare, call,
   `bdnz`), translated through a BAT as Mac OS runs, timed for 20 million
   instructions:
   - with the interpreter;
   - with the block cache;
   - with the block cache and generated code.

## Output

The result shows on the menu's status line. It is also written to the
log, and so to `SD:/g3macpi/logs/boot.log`:

```
CPU self-test: PASS, 200/200 programs identical with generated code
MIPS: interpreter 118.8, block cache 162.4, generated code 10.9
CPU self-test: 13000000 instructions compared, ... block runs in generated code, ... ms
```

The numbers above are from `g3_host --selftest` on the x86 build host.
There, "generated code" means the simulator, so its MIPS figure says
nothing about speed. On the Pi all three are real.

"FAIL" names the first program, by seed, and what differed first. With
the seed, the program can be replayed on a PC in the same generator.

## Measured on the Pi 400

First run, commit 2c31e79, owner's Pi 400 (stock clock), from a photo of
the screen:

```
Self-test PASS. MIPS: interpreter 30.2, block cache 53.3, generated code 107
```

- **PASS:** the generated AArch64 code, run natively, matched the
  interpreter in all 200 random programs.
- **Generated code:** about 3.5 times the interpreter on this loop; the
  block cache alone, 1.8 times.
- **This loop favours the JIT:** blocks are long and the TLB always hits.
  Mac OS code has short blocks (about 3 instructions on average in the ROM
  boot), so expect less there.
- **Pi against x86:** the Pi's interpreter is about 4 times slower than
  the x86 build host's (118.8).

## Memory

- **Work memory:** 6 MB from the heap: two 1 MB machines and a 4 MB block
  cache.
- **Code buffer:** 512 KB of its own (`g3_selftest_code`), in the same
  executable `.text_g3jit` section as the Mac's JIT buffer (see
  [jit-a64.md](jit-a64.md)). A running Mac's generated code is therefore
  not disturbed; the Mac waits while the menu shows.

## Tests

`tests/unit/platform/test_cpu_selftest.cpp`:
- it passes, with all three speed figures;
- without code memory it reports "block cache only";
- with a runner that flips a register bit now and then it reports FAIL,
  so it does notice a broken CPU.
