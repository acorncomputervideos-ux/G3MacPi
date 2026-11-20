# ELF client programs (init-program and go) — implementation notes

Code: `core/firmware/openfirm/elf_loader.*`, `of::Firmware::start_client`.
Tests: `tests/unit/firmware/test_elf_loader.cpp`.

Mac OS 9 itself boots through the bootinfo script of its `Mac OS ROM`
file, but ELF is the standard format for Open Firmware client programs.
It is used by Linux boot loaders and kernels, and by test programs we
write ourselves. It is the first way to run guest code under our firmware.

## Facts

Source: CHRP binding rev 1.7 (see `bootinfo.md` for the document), 10.1
to 10.4.1; ELF32 field offsets from the System V ABI.

| Fact | Section |
|---|---|
| `load` reads the client program file to `load-base`; that is not where it runs. Memory from load-base up to the firmware (or the end of RAM) is available for loading | 10.1 |
| On client entry r3 = 0 and r4 = 0 | 10.2 |
| ELF program if: e_ident = 7F 'E' 'L' 'F'; EI_CLASS (4) = 1; e_type (16) = 2; e_machine (18) = 20; e_version (20) = 1; e_flags (36) = 0. Byte order from EI_DATA (5) | 10.4.1.2 |
| Note segment (PT_NOTE): name "PowerPC", type 0x1275, descriptor real-mode, real-base, real-size, virt-base, virt-size, load-base | 10.4.1.1 |
| init-program: check the note; claim memory for each PT_LOAD; copy p_filesz bytes from the image to p_paddr; zero to p_memsz; `go` starts at e_entry. ELF sections are ignored | 10.4.1.3 |
| ELF32 header: e_entry @24, e_phoff @28, e_phentsize @42, e_phnum @44. Program header (32 bytes): p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, ... | System V ABI |

## Choices and limits

- Big-endian images only (EI_DATA = 2). A little-endian image is reported
  as such and refused.
- Real mode only. A segment with p_paddr = −1 goes to p_vaddr (1:1),
  as rule b) does for virtual mode. The note's values are returned to the
  caller. Restarting the firmware to match them (rule a) is not done.
- All headers are checked, and copied to host memory, before any guest
  memory changes. A bad image leaves memory untouched (tested).
- Copies are overlap-safe in both directions, which goes beyond the
  spec's "need not take precautions".
- `start_client`: pc = entry; r1 = a 16-byte aligned stack below the
  given top; r3 = r4 = 0; r5 = the client-interface entry (from the
  PowerPC binding as I know it: **to verify**); MSR IR, DR and EE clear.

## Tests

- A typical image: note, text, data with bss. Copying and zero-filling
  are checked against RAM pre-filled with 0xFF; memory is claimed, and a
  second load fails to claim.
- Every recognition rule, one test each; a short image.
- A segment outside the image, filesz > memsz, no PT_LOAD, an empty
  note, and overlapping moves down and up.
- `go`: a loaded program checks r3/r4/r1 on entry and calls `finddevice`
  through r5 and the HLE trap; the result arrives in its argument array.
