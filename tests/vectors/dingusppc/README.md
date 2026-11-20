# PowerPC test vectors from DingusPPC

Data files only — no DingusPPC code is used by G3MacPi (ADR-0011).

| File | Lines | Content |
|---|---|---|
| `ppcinttests.csv` | 5620 | Integer instructions: inputs in r3 (`rA=`) and r4 (`rB=`), XER = CR = 0 before; expected r3 (`rD=`), XER, CR after one instruction |
| `ppcfloattests.csv` | 2054 | Floating-point instructions with FPSCR/CR expectations |
| `ppcdisasmtest.csv` | 452 | Disassembler expectations |

- Source: https://github.com/dingusdev/dingusppc, `cpu/ppc/test/`, upstream
  commit `d0fb711842f6e9851394ea036b049668b3119d52` (2026-11-10).
- Licence: GPL-3.0 (`LICENSE.GPL-3.0`), © The DingusPPC Development Team.
- Provenance of the expected values: upstream also ships `ppcinttest.txt`,
  which looks like captured output of a test program run on real PowerPC
  hardware. **Which CPU model produced it is not documented** — treat the
  vectors as a strong reference, not as ground truth. Where they disagree
  with the PowerPC manuals (e.g. results the manuals call "undefined", such
  as divide by zero), we document the case explicitly.
- Used under ADR-0004 (offline reference data). Unmodified copies.
