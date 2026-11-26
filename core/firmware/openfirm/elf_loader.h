// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/elf_loader.h — init-program for ELF client
// programs: recognize the image at load-base and copy its segments into place.
//
// Facts (docs/firmware/elf-loader.md):
//  - CHRP binding rev 1.7, 10.4.1.2: an image is an ELF program if e_ident
//    starts with 7F 'E' 'L' 'F', EI_CLASS (offset 4) = 1, e_type (16) = 2,
//    e_machine (18) = 20 (PowerPC), e_version (20) = 1, e_flags (36) = 0;
//    multi-byte fields use the byte order given by EI_DATA (offset 5).
//  - 10.4.1.3: find the PT_NOTE named "PowerPC" of type 0x1275 (descriptor
//    real-mode, real-base, real-size, virt-base, virt-size, load-base);
//    claim memory for every PT_LOAD (p_type 1) segment, copy its p_filesz
//    bytes to p_paddr and zero the rest up to p_memsz; the entry point is
//    e_entry. ELF sections are ignored.
//  - 10.2: on entry r3 = 0 and r4 = 0.
//  - ELF32 layout (System V ABI): e_entry @24, e_phoff @28, e_phentsize
//    @42, e_phnum @44; program header: p_type @0, p_offset @4, p_vaddr @8,
//    p_paddr @12, p_filesz @16, p_memsz @20 (32 bytes).
//
// Scope: big-endian images in real mode (the firmware's mode). A segment
// whose p_paddr is -1 goes to p_vaddr (1:1). Restarting the firmware to
// match a note's mode is not done: the note is reported to the caller.

#ifndef G3_CORE_FIRMWARE_OPENFIRM_ELF_LOADER_H
#define G3_CORE_FIRMWARE_OPENFIRM_ELF_LOADER_H

#include "core/common/types.h"
#include "core/cpu/ppc750/memory_interface.h"
#include "core/firmware/openfirm/memory_map.h"

namespace g3::of {

struct ElfNote {
    bool present = false;
    u32 real_mode = 0, real_base = 0, real_size = 0, virt_base = 0, virt_size = 0, load_base = 0;
};

struct ElfProgram {
    u32 entry = 0;
    u32 segments = 0;        // PT_LOAD segments placed
    u32 low = 0, high = 0;   // lowest and highest (exclusive) address placed
    ElfNote note;
};

enum class ElfError : u8 {
    None, NotElf, NotExecutable, WrongMachine, BadVersion, BadFlags, LittleEndian,
    BadHeaders, SegmentOutsideImage, ClaimFailed, MemoryError,
};

// True if the image at `image` (size bytes) passes the CHRP recognition rules.
bool is_elf_program(ppc::MemoryInterface& mem, u32 image, u32 size);

// Places the segments. `claims` may be nullptr (no claiming).
ElfError load_elf_program(ppc::MemoryInterface& mem, MemoryMap* claims, u32 image, u32 size,
                          ElfProgram& out);

} // namespace g3::of

#endif // G3_CORE_FIRMWARE_OPENFIRM_ELF_LOADER_H
