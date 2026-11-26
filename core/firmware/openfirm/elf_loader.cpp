// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/elf_loader.cpp — see elf_loader.h.

#include "core/firmware/openfirm/elf_loader.h"

namespace g3::of {

namespace {

constexpr u32 PT_LOAD = 1, PT_NOTE = 4;
constexpr u32 kMaxPhdrs = 32;

struct Phdr { u32 type, offset, vaddr, paddr, filesz, memsz; };

bool rd8(ppc::MemoryInterface& m, u32 a, u8& v) { return m.read8(a, v); }
bool rd16(ppc::MemoryInterface& m, u32 a, u32& v)
{
    u16 h = 0;
    const bool ok = m.read16(a, h);
    v = h;
    return ok;
}
bool rd32(ppc::MemoryInterface& m, u32 a, u32& v) { return m.read32(a, v); }

ElfError check_header(ppc::MemoryInterface& m, u32 img, u32 size)
{
    if (size < 52)
        return ElfError::NotElf;
    u8 id[6] = {};
    for (u32 i = 0; i < 6; ++i)
        if (!rd8(m, img + i, id[i]))
            return ElfError::MemoryError;
    if (id[0] != 0x7F || id[1] != 'E' || id[2] != 'L' || id[3] != 'F' || id[4] != 1)
        return ElfError::NotElf;
    if (id[5] != 2)
        return ElfError::LittleEndian;  // EI_DATA: 2 = big-endian; only that is supported
    u32 type = 0, machine = 0, version = 0, flags = 0;
    if (!rd16(m, img + 16, type) || !rd16(m, img + 18, machine) || !rd32(m, img + 20, version) ||
        !rd32(m, img + 36, flags))
        return ElfError::MemoryError;
    if (type != 2)
        return ElfError::NotExecutable;
    if (machine != 20)
        return ElfError::WrongMachine;
    if (version != 1)
        return ElfError::BadVersion;
    if (flags != 0)
        return ElfError::BadFlags;
    return ElfError::None;
}

// Copies n bytes inside guest memory, correct for overlapping ranges.
bool guest_move(ppc::MemoryInterface& m, u32 from, u32 to, u32 n)
{
    u8 b = 0;
    if (to <= from) {
        for (u32 i = 0; i < n; ++i)
            if (!m.read8(from + i, b) || !m.write8(to + i, b))
                return false;
    } else {
        for (u32 i = n; i-- > 0;)
            if (!m.read8(from + i, b) || !m.write8(to + i, b))
                return false;
    }
    return true;
}

} // namespace

bool is_elf_program(ppc::MemoryInterface& mem, u32 image, u32 size)
{
    return check_header(mem, image, size) == ElfError::None;
}

ElfError load_elf_program(ppc::MemoryInterface& m, MemoryMap* claims, u32 img, u32 size, ElfProgram& out)
{
    out = ElfProgram{};
    const ElfError h = check_header(m, img, size);
    if (h != ElfError::None)
        return h;
    u32 entry = 0, phoff = 0, phentsize = 0, phnum = 0;
    if (!rd32(m, img + 24, entry) || !rd32(m, img + 28, phoff) || !rd16(m, img + 42, phentsize) ||
        !rd16(m, img + 44, phnum))
        return ElfError::MemoryError;
    if (phentsize < 32 || phnum == 0 || phnum > kMaxPhdrs ||
        static_cast<u64>(phoff) + static_cast<u64>(phnum) * phentsize > size)
        return ElfError::BadHeaders;

    // d) Copy the program headers to a safe place (host memory) first.
    Phdr ph[kMaxPhdrs];
    for (u32 i = 0; i < phnum; ++i) {
        const u32 p = img + phoff + i * phentsize;
        if (!rd32(m, p + 0, ph[i].type) || !rd32(m, p + 4, ph[i].offset) || !rd32(m, p + 8, ph[i].vaddr) ||
            !rd32(m, p + 12, ph[i].paddr) || !rd32(m, p + 16, ph[i].filesz) || !rd32(m, p + 20, ph[i].memsz))
            return ElfError::MemoryError;
    }

    // a) The PowerPC 1275 note.
    for (u32 i = 0; i < phnum; ++i) {
        if (ph[i].type != PT_NOTE)
            continue;
        if (static_cast<u64>(ph[i].offset) + ph[i].filesz > size)
            return ElfError::SegmentOutsideImage;
        if (ph[i].filesz < 12)
            continue;  // too small to hold a note header: not ours
        const u32 n = img + ph[i].offset;
        u32 namesz = 0, descsz = 0, type = 0;
        if (!rd32(m, n, namesz) || !rd32(m, n + 4, descsz) || !rd32(m, n + 8, type))
            return ElfError::MemoryError;
        const u32 name_pad = (namesz + 3) & ~3u;
        if (type != 0x1275 || namesz != 8 || descsz < 24 || 12 + name_pad + 24 > ph[i].filesz)
            continue;
        const char want[8] = {'P', 'o', 'w', 'e', 'r', 'P', 'C', '\0'};
        bool match = true;
        for (u32 k = 0; k < 8 && match; ++k) {
            u8 c = 0;
            match = rd8(m, n + 12 + k, c) && c == static_cast<u8>(want[k]);
        }
        if (!match)
            continue;
        const u32 d = n + 12 + name_pad;
        ElfNote& note = out.note;
        if (!rd32(m, d, note.real_mode) || !rd32(m, d + 4, note.real_base) ||
            !rd32(m, d + 8, note.real_size) || !rd32(m, d + 12, note.virt_base) ||
            !rd32(m, d + 16, note.virt_size) || !rd32(m, d + 20, note.load_base))
            return ElfError::MemoryError;
        note.present = true;
        break;
    }

    // Check every PT_LOAD before changing memory.
    for (u32 i = 0; i < phnum; ++i) {
        if (ph[i].type != PT_LOAD)
            continue;
        if (ph[i].filesz > ph[i].memsz || static_cast<u64>(ph[i].offset) + ph[i].filesz > size)
            return ElfError::SegmentOutsideImage;
        const u32 dest = ph[i].paddr == 0xFFFFFFFFu ? ph[i].vaddr : ph[i].paddr;
        if (static_cast<u64>(dest) + ph[i].memsz > 0x100000000ull)
            return ElfError::BadHeaders;
    }

    // c) + e) Claim, copy, zero.
    bool first = true;
    for (u32 i = 0; i < phnum; ++i) {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0)
            continue;
        const u32 dest = ph[i].paddr == 0xFFFFFFFFu ? ph[i].vaddr : ph[i].paddr;
        if (claims && claims->claim(dest, ph[i].memsz, 0) != dest)
            return ElfError::ClaimFailed;
        if (!guest_move(m, img + ph[i].offset, dest, ph[i].filesz))
            return ElfError::MemoryError;
        for (u32 k = ph[i].filesz; k < ph[i].memsz; ++k)
            if (!m.write8(dest + k, 0))
                return ElfError::MemoryError;
        if (first || dest < out.low)
            out.low = dest;
        if (first || dest + ph[i].memsz > out.high)
            out.high = dest + ph[i].memsz;
        first = false;
        ++out.segments;
    }
    if (out.segments == 0)
        return ElfError::BadHeaders;
    out.entry = entry;  // f)
    return ElfError::None;
}

} // namespace g3::of
