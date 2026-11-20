# CHRP bootinfo objects — implementation notes

Code: `core/firmware/openfirm/bootinfo.*`. Tests:
`tests/unit/firmware/test_bootinfo.cpp`.

Mac OS's boot file (`Mac OS ROM`, Finder type `tbxi`) starts with a CHRP
"bootinfo object": a short SGML-style text that carries, among other
things, the Open Firmware script that loads the rest of the file.

## Source

*PowerPC Microprocessor CHRP System binding to IEEE 1275-1994*,
revision 1.7 (September 23, 1996), section 3.1.6 "Bootinfo Objects",
read from the PDF (mirror:
`archive.decromancer.ca/bitsavers.org/pdf/apple/powerpc/CHRP/chrp1_7a.pdf`).

| Fact (CHRP binding §3.1.6) | Implemented |
|---|---|
| Elements `<CHRP-BOOT>` ⊃ `<DESCRIPTION>`, `<OS-NAME>`, `<BOOT-SCRIPT>`, `<ICON SIZE=w,h COLOR-SPACE=r,g,b><BITMAP>…</BITMAP></ICON>`, in any order | ✅ |
| Tags and entity names are case-insensitive; other text is case-sensitive; Latin-1 | ✅ (text is copied byte for byte) |
| Unrecognized tags: material ignored until the end tag | ✅ (e.g. Apple's `<COMPATIBLE>`) |
| Several CHRP-BOOT sections allowed, one per boot choice | ✅ (`index`) |
| `&lt;` `&gt;` `&amp;` | ✅ in all elements |
| In BOOT-SCRIPT: `&device;` `&partition;` `&directory;` `&filename;` `&full-path;` relative to the bootinfo object's own path, `&device;:[&partition;][,]\&directory;\&filename;` | ✅ (comma only if there's a partition) |
| BOOT-SCRIPT lines run as if typed at the user interface | the Forth engine (next) |
| Icon defaults SIZE 64,64 and COLOR-SPACE 3,3,2; bitmap = hex digit pairs, white space allowed | ✅ |

Choices:
- An unknown entity is kept as written.
- Anything after a section's `</CHRP-BOOT>` isn't interpreted.
  `body_offset` gives its position, because Mac OS's boot file carries
  binary data there.
- Spaces are allowed around `=` and `,` in attributes.
- Elements longer than their buffer (256 bytes for text, 32 KB for the
  script) make parsing fail rather than truncate.

## Tests

- A full object with mixed-case tags, an unknown element, all entities,
  an icon and binary data after the end.
- Defaults, entity scope, unknown entities, several sections.
- Malformed input: missing or stray end tags, an unterminated element,
  bad hex, an oversized script, a different tag name.
- An end tag that only starts with the element's name, and spaces in
  attributes. That test found a real bug: spaces before a comma ended the
  number list.
