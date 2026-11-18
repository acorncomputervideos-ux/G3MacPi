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
