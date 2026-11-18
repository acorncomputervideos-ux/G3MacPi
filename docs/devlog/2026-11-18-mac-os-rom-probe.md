# 2026-11-18 — first contact with Apple's Mac OS ROM file

The user supplied the NewWorld "Mac OS ROM" data fork:
- 1,945,746 bytes;
- SHA-1 a63053efcbf8c569f82ede6ec69a551c10b7e899;
- compatible list "iMac,1 PowerMac1,1 PowerBook1,1".

The same size appears inside Apple's Mac OS ROM Update 1.0 archive, which
installs version 1.6. Nothing from it is committed.

Its bootinfo boot script was run by our firmware with
`g3_rom_probe "Mac OS ROM"`. That tool places the file at load-base, as
`load` would, and runs `init-program`.

What it needed, in order:
1. **Interpreted control structures.** The script uses `if … then` and a
   multi-line `begin … while … repeat` outside any definition, as IEEE 1275
   allows. Our Forth refused ("compile-only word"). Now implemented, see
   docs/firmware/forth.md.
2. **Interpreted `abort"`.** Now implemented.
3. **`$=`, `bounds`, `3drop`, `$find`, `.registers`.** Now defined.
   `g3_rom_probe … --words` checks every word of a script at once.
4. **The script's checks pass** on our B&W device tree: model, then
   compatible "PowerMac1,1".
5. **It now stops at `memory??`.** `/chosen` has no `memory` and `mmu`
   ihandles. The script calls `claim`, `release`, `map`, `unmap` and
   `translate` on them. It claims physical pages for the compressed
   toolbox image and maps them at a separately claimed virtual address.
   That address goes into `/rom/macos`'s "AAPL,toolbox-image,lzss" for
   the ELF loader (MacOS.elf), which `init-program` / `go` then start.

So the firmware must run in the PowerPC binding's Virtual-Mode
(binding rev. 2.1, real-mode? false):
- it keeps its own translations, published as the "translations"
  property `(virt size phys mode)` on the mmu package;
- the client program starts with those translations active.

That is the next piece of work.

## Later the same day: Virtual-Mode, and Apple's loader runs

Our firmware now runs in Virtual-Mode (docs/firmware/virtual-mode.md),
with these additions:
- `my-self` is a value (`0 to my-self`);
- init-program reports which ELF check fails;
- physical claims come from the top (with the bottom-up rule, the
  compressed ROM image took the loader's fixed addresses);
- `exit` no longer returns to the client: it is parked on an idle trap.

With these, the whole boot script runs:
1. it claims and maps memory and copies the compressed toolbox image
   (1.8 MB);
2. it creates `/rom/macos`;
3. it copies MacOS.elf to load-base;
4. `init-program` loads its two segments (0x200000 code and 0x100000
   data), and `go` starts it at 0x208780 with translation on.

**Apple's loader then runs on the emulated 750 and calls our client
interface.** `g3_rom_probe` logs every call:

    finddevice "/chosen"    getprop stdout
    finddevice "/"          getprop "AAPL,debug"  (absent)
                            getprop "AAPL,writable-ROM-aperture"  (absent)
                            getproplen "copyright"  (absent)
    write "MacOS: Boot Failure!  (0xF3C481F6)"
    write "Official Apple copyright message missing."
    exit

Before `exit` was fixed, the call returned. The loader went on to look
for `/rtas` (absent: "Unable to find RTAS. Proceed with caution"), then
claimed memory, then hung.

The loader checks the root node's "copyright" property. Whether our
firmware should carry Apple's copyright notice there is the user's
decision.

## The copyright property

The project owner agreed to add the root node's "copyright" property, for
interoperability only. MacOS.elf holds the strings "Copyright " and
" Apple Computer, Inc." next to its error message. The value is
"Copyright 1983-1999 Apple Computer, Inc. All Rights Reserved"
(`kAppleCopyrightNotice`, bw_device_tree.h).

With it, the loader passes that check. It then reads `/chosen` mmu and
memory and looks for `/rtas`. Ours is absent, so it writes
"MacOS: Boot Failure!" and "/rtas not found!" and exits.

Next: RTAS (CHRP Run-Time Abstraction Services), meaning a `/rtas` node,
`rtas-size`, `instantiate-rtas` and the RTAS calls Mac OS makes.
