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
  - `rlwinm` without Rc;
  - `add`, `subf`, `and`, `or`, `xor` without OE or Rc;
  - `mflr`, `mfctr`.
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

Configuration: `YosemiteConfig::jit_code` / `jit_code_words`, and
`jit = yes` in `g3macpi.ini` on the Pi (block cache and code). Without a
runner, the code is called directly; that is only possible on an
AArch64 host.

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

Native execution on the Pi has not been tested yet.

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
- **ROM boot:** `G3_JIT=8 G3_JITCODE=8 g3_rom_probe` runs the Mac OS ROM
  boot with generated code on the simulator. Over 3 s of guest time
  (1.05 × 10⁹ instructions, 3.9 × 10⁸ block runs in generated code) it
  ends with the same registers and RAM hash as the interpreter.

## Coverage, and what comes next

In that boot, 27% of compiled operations were native and the rest were
handler calls. The next steps, in order of expected gain:

1. Loads and stores: real-mode and soft-TLB fast path inline, the
   handler otherwise.
2. Compares and CR0 updates.
3. Branches at block end, linking blocks directly.

Speed can only be measured on the Pi.
