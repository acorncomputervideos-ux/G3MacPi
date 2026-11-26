# 2026-11-19 — decoded-block cache (JIT stage 1)

- Added the decoded-block cache (docs/cpu/block-cache.md). It is
  bit-identical to the interpreter:
  - random and directed differential tests, with every safety check
    sabotaged in turn and caught;
  - the Mac OS ROM boot (1.05 × 10⁹ instructions) ends with the same
    registers and RAM hash.
- Write reporting: PhysicalBus write observer; DBDMA and CMD646 report
  DMA writes; the CPU reports its own stores.
- Speed: 20% fewer host instructions, but wall time is about the same
  (−2–3%) on x86. Blocks in Mac OS code are about 3 instructions long.
  Off by default until stage 2 (AArch64 code generation) makes it worth it.
- Fixed a GCC -O2 false-positive warning in tests/unit/storage/hfs_image.h
  that broke the release build of the tests.
