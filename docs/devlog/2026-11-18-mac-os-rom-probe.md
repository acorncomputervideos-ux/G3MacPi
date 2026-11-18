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
