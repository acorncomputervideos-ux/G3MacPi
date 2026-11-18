# PEF writer — implementation notes

Code: `tools/pef/pef_writer.*` (library `g3pef`). Tests:
`tests/unit/tools/test_pef.cpp`. A host-side build tool for guest code;
not part of the emulator.

Mac OS 9 loads display drivers ("NDRV") and other native code as PEF
containers, through the Code Fragment Manager. Our Mac OS display driver
will be compiled with `powerpc-linux-gnu-gcc` (see `guest/README.md`)
and packaged with this writer.

## Sources

- *Inside Macintosh: Mac OS Runtime Architectures* (Apple, 1997),
  chapter 8 "PEF Structure", read on Apple's documentation archive
  (`developer.apple.com/library/archive/documentation/mac/runtimehtml/RTArch-89.html`
  to `RTArch-101.html`). It covers the container header, section
  headers and kinds, the section name table, the loader header, imported
  libraries and symbols, relocation headers, the relocation instruction
  set and variables, the loader string table, and exports.
- Apple's `PEFBinaryFormat.h` for what the chapter shows only in figures.
  Facts only, no code copied: the relocation instructions' bit layouts,
  the hash word and hash index functions, the hash table sizing, the hash
  slot layout (14-bit count, 18-bit first index), and exported symbols
  (10 bytes, packed).

## What is written

- Container header: `Joy!` `peff` `pwpc`, version 1, then the section
  and instantiated-section counts.
- Section headers in section order, followed immediately by the section
  name table. Contents start on 16 bytes for code and data, and on
  4 bytes for the loader section.
- Supported section kinds: code (0), unpacked data (1, with a
  zero-initialized tail) and constant (3), then one loader section (4).
  Pattern-initialized data is not produced.
- Loader section, in order:
  1. the 56-byte header (main/init/term entry points or −1);
  2. imported libraries (24 bytes each; the weak and init-before flags);
  3. imported symbols (class byte + 24-bit name offset);
  4. relocation headers, then the relocation instructions;
  5. the string table;
  6. the export hash table (4-byte aligned), keys, and exported symbols,
     grouped by hash slot.
- Relocations: "add the address of an import, or of a section, to this
  word". Each section's relocations are sorted and encoded with
  `RelocIncrPosition` (gaps up to 4096 bytes) or `RelocSetPosition`,
  then `RelocSmByImport`, `RelocLgByImport` (index > 511) or
  `RelocSmBySection`.

## Tests and their limits

The tests prepare the built containers with a small preparer. It follows
the chapter's description of the Code Fragment Manager: sections placed
at chosen addresses, the relocation variables and their initial values,
and every relocation opcode group, including ones the writer never
emits. Exports are looked up through the hash table. Tests also cover:
- the container layout byte by byte;
- far relocations and import indexes above 511;
- 25 exports in a 4-slot table;
- every input error.

The hash function is checked against values worked out by hand for short
names, and, for long names where the running value goes negative,
against a separate Python implementation of the published algorithm.

The writer and the preparer have the same author. The real check is Mac
OS's Code Fragment Manager accepting our driver, which needs a Mac OS 9
boot.

## elf2pef: compiled C to PEF

`tools/pef/elf2pef.{h,cpp}` and the `g3_elf2pef` command line convert a
PowerPC ELF shared object into a PEF container:

    g3_elf2pef in.so out.pef --export DoDriverIO --export TheDriverDescription:data \
        --import imp_SynchronizeIO=DriverServicesLib:SynchronizeIO

Build the input with the flags in CMakeLists.txt (`G3_FRAGMENT_FLAGS`):
- `-fPIC -shared`;
- `-Wl,-Bsymbolic`, so that calls between our own functions are direct
  rather than through a PLT;
- 16-byte page sizes, so that the code and data segments are adjacent;
- `-z norelro`.

The conversion:
- **One section.** The whole loaded image goes into one PEF "executable
  data" section (kind 6). SVR4 PIC code finds its data at a fixed
  distance from its own address, so code and data must stay together.
  The bss is stored as zeros when transition vectors follow it.
- **Relocations.**
  - `R_PPC_RELATIVE` becomes "add the section address".
  - `R_PPC_ADDR32` / `R_PPC_GLOB_DAT` against a defined symbol also
    becomes "add the section address".
  - Against an undefined symbol, the same relocations become "add the
    import's address". Undeclared imports are refused.
  - `R_PPC_JMP_SLOT` is refused, because calls to Mac OS must go through
    `cfm_call`. Other types are refused too.
- **Exports.**
  - A code export gets a transition vector {code address, TOC}, appended
    after the image, with both words relocated by the section address.
    It is exported with class TVector.
  - Data exports point at the symbol.
  - Our code does not use r2, so the TOC value doesn't matter.

### Calling conventions

Mac OS code follows the AIX-derived conventions from Mac OS Runtime
Architectures, chapter 1. Our code follows SVR4. Between the two:
- integer and pointer arguments and results are passed alike, in r3 and
  up;
- the nonvolatile registers are compatible.

What differs:
- r2 (the TOC);
- r12 (the transition vector);
- the caller's linkage area: the callee may write CR at 4(SP), LR at
  8(SP) and the parameter area at 24..55(SP);
- where floating-point and stacked arguments go.

So:
- Calls into Mac OS go through `cfm_call` (guest/cfm/cfm_glue.S). It
  provides a 64-byte CFM frame, keeps its own return address out of the
  callee's reach, loads r2 and r12 from the vector, and restores r2.
- Our exports may be called directly. An SVR4 function only writes 4(SP)
  of its caller's frame (the CR save slot, which a CFM callee may also
  write) and reads its first 8 arguments from registers.
- Limits: integer and pointer arguments only (at most 7 through
  `cfm_call`, 8 for exports), and integer or pointer results.

### Tests

`g3_guest_cfm` (tests/guest/run_cfm.cpp, run in CI's guest job) checks
the whole path on the emulated 750, at two load addresses:
- results;
- the callee's r2, r12 and seven arguments;
- stack balance and r2 on return;
- the relocated pointer and the exported data;
- reproducible output;
- refusals: an undeclared import, a missing export, an executable, a
  truncated file, a PLT call.

Planted bugs it catches:
- in the glue: no r2 restore, LR saved where the callee writes, no r12,
  no callee TOC, a frame too small;
- in the converter: dropped relative relocations, an unrelocated vector
  TOC.

Unverified: the real Code Fragment Manager accepting a kind-6 section
with these relocations. That needs Mac OS 9.
