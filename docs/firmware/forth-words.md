# Open Firmware Forth words — implementation notes

Code: `core/firmware/openfirm/forth_words.*` (class `of::FirmwareWords`).
Tests: `tests/unit/firmware/test_forth_words.cpp`.

These words connect the Forth interpreter (`forth.md`) to the device tree,
the device instances and the disk drivers, so a boot script can explore
the machine and load files.

## Stack effects

From IEEE 1275-1994 as I know it. **To verify** against the standard's
text, and against what Mac OS's boot script actually calls (it will be
logged when booting).

| Word | Stack effect |
|---|---|
| `dev` | ( "path" -- ) select the active package (aliases allowed) |
| `find-device` | ( path len -- ) |
| `device-end`, `unselect-dev` | ( -- ) no active package |
| `active-package` | ( -- phandle ) |
| `find-package` | ( name len -- false \| phandle true ); a name is looked up under `/packages`, a path anywhere |
| `get-package-property` | ( name len phandle -- true \| prop len false ) |
| `get-inherited-property` | ( name len -- true \| prop len false ); from the active package up to the root |
| `property` | ( prop len name len -- ) into the active package |
| `new-device` / `finish-device` | ( -- ) a child of the active package / back to its parent |
| `device-name`, `device-type` | ( str len -- ) |
| `encode-int` | ( n -- prop 4 ), big-endian |
| `encode-string` | ( str len -- prop len+1 ), with a NUL |
| `encode-bytes` | ( data len -- prop len ) |
| `encode+` | ( p1 l1 p2 l2 -- p3 l1+l2 ) |
| `decode-int` | ( prop len -- prop+4 len-4 n ) |
| `decode-string` | ( prop len -- prop' len' str n ): up to the first NUL, which is skipped |
| `open-dev` / `close-dev` | ( path len -- ihandle \| 0 ) / ( ihandle -- ) |
| `$call-method` | ( ... method len ihandle -- ... ): `read`/`write` ( addr len -- actual ), `seek` ( pos.lo pos.hi -- status ) |
| `load-base`, `load-size` | ( -- addr ), ( -- n ) |
| `load` | ( "device-specifier<eol>" -- ): reads the whole file to load-base; the path runs to the end of the line, since HFS names contain spaces |
| `ms`, `get-msecs` | ( n -- ), ( -- ms ) |
| `reg` | ( phys size -- ): the active package's "reg" (1 address cell and 1 size cell assumed: limit) |
| `my-self` | ( -- ihandle ): between `new-device` and `finish-device`, an instance of the node being built (IEEE 1275 `new-device` makes one current); otherwise 0 |
| `ihandle>phandle` | ( ihandle -- phandle ) |
| `init-program` | ( -- ): prepares the image at load-base: an ELF program (see `elf-loader.md`), or a CHRP bootinfo object, whose boot script is then run |
| `go` | ( -- ): starts the prepared ELF program through the machine's `go` callback |
| `boot` | ( "device-specifier<eol>" -- ): `load`, `init-program`, then `go` if an ELF program was prepared |

## Choices

- Property values handed to Forth (from `get-*-property` and the
  `encode-*` words) are copied into a scratch region of guest memory,
  used round-robin. A script must use a value before the region (16 KB
  here) has been reused.
- A node made with `new-device` has no unit address. `reg`-derived unit
  addresses come with the B&W device tree builder.
- `ms` doesn't wait: emulated time passes on its own.
- `load` closes its instance, and fails with −37 (file I/O) if the file
  doesn't fit below the load limit.
- `$call-method` supports read, write and seek, through the C++ drivers.
  Other methods fail with −21 and will be added as the boot script needs
  them.

## Booting

`boot hd:9,\\:tbxi` loads Mac OS's boot file. Its text header is a
bootinfo object, so `init-program` runs its BOOT-SCRIPT, as the CHRP
binding's multiboot menu does when the user picks an OS (§3.1.7). That a
plain `boot` of a bootinfo file does the same is our choice, matching how
Mac OS boot paths are used. The script may `load` another file and
`init-program go` it. `boot` then does not call `go` again. It only calls
`go` when its own `init-program` prepared an ELF program.

The path entities for the script come from the boot path: device, the
partition number, and, after the comma, the rest split at its last
backslash into directory and filename. So `hd:9,\System Folder\Mac OS ROM`
gives `hd`, `9`, `System Folder`, `Mac OS ROM`, and `hd:9,\\:tbxi` gives
`hd`, `9`, empty, `:tbxi` (Apple's values for such paths: to verify). A
boot script can't boot another bootinfo object (−21), so recursion is
impossible.

## Tests

- Queries: properties of the root and the CPU, missing properties,
  inherited properties, `device-end`, `find-package` by name and by
  path, aliases in `dev`, failures.
- Building: a node with `device-name`, `device-type`, `reg` from
  `encode-int encode+`, and a `compatible` string. The NUL is checked
  with the scratch memory pre-filled with 0xFF, because the zeroed RAM
  first hid a missing NUL.
- Decoding strings and ints from one combined property.
- Packages: raw partition seek and read, a file read past its end, an
  unsupported method, a closed instance.
- `load` by Finder type and by a path with spaces. The file's bytes are
  checked at load-base.
- `boot` of a bootinfo file: the script's output with all entities,
  through both path forms.
- `boot` of an ELF: the segment is in place and `go` is called with the
  entry point; also step by step with `load`, `init-program`, `go`.
- The Mac-like chain: a boot script that loads an ELF from another file
  and starts it. `go` happens exactly once; this test found a bug where
  `boot` called `go` a second time.
- Errors: an unknown format, `go` with nothing prepared, a missing file,
  a nested bootinfo, a bootinfo without a script, no `go` callback.
