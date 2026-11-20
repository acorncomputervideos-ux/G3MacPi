# 2026-11-17 — Disassembler

- `core/cpu/ppc750/disasm.*`: freestanding PowerPC 750 disassembler with
  simplified mnemonics (branch conditions with hints, CR-bit names, rotate
  aliases, li/lis/mr/nop, mflr/mtctr/..., named SPRs), `dc.l` for invalid
  encodings, safe truncation.
- Reference: all 397 DingusPPC disassembler vectors pass. The 52 vectors for
  POWER/601-only and 603-only instructions are checked to produce `dc.l`,
  because those encodings are illegal on the 750.
- New cross-check of disassembler vs interpreter on 1 million random words;
  it found one real inconsistency (unimplemented SPRs), fixed.
- Sabotage checks (rotlwi/rotrwi threshold, CR-bit naming) are caught.
- 83/83 tests: gcc + ASan/UBSan, clang; check-core passes.
