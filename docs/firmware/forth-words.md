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
