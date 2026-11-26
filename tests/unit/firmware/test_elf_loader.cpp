// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the ELF init-program (CHRP binding 10.4.1) and "go".

#include "core/cpu/ppc750/ppc750.h"
#include "core/firmware/openfirm/elf_loader.h"
#include "core/firmware/openfirm/firmware.h"
#include "core/mem/physical_bus.h"
#include "tests/framework/g3test.h"
#include "tests/unit/cpu/ppc_asm.h"

#include <cstring>
#include <memory>
#include <vector>

using namespace g3;
using namespace g3::of;

namespace {

constexpr u32 kImage = 0x40000;  // load-base

struct Elf {
    std::vector<u8> d;
    void b16(size_t a, u32 v) { d[a] = static_cast<u8>(v >> 8); d[a + 1] = static_cast<u8>(v); }
    void b32(size_t a, u32 v) { b16(a, v >> 16); b16(a + 2, v & 0xFFFF); }
    struct Seg { u32 type, offset, vaddr, paddr, filesz, memsz; };
    // Header + program headers at 52, then payload.
    Elf(const std::vector<Seg>& segs, u32 entry, size_t size) : d(size)
    {
        d[0] = 0x7F; d[1] = 'E'; d[2] = 'L'; d[3] = 'F';
        d[4] = 1;            // ELFCLASS32
        d[5] = 2;            // ELFDATA2MSB
        d[6] = 1;            // EV_CURRENT
        b16(16, 2);          // ET_EXEC
        b16(18, 20);         // EM_PPC
        b32(20, 1);          // e_version
        b32(24, entry);
        b32(28, 52);         // e_phoff
        b16(40, 52);         // e_ehsize
        b16(42, 32);         // e_phentsize
        b16(44, static_cast<u32>(segs.size()));
        for (size_t i = 0; i < segs.size(); ++i) {
            const size_t p = 52 + 32 * i;
            b32(p + 0, segs[i].type);
            b32(p + 4, segs[i].offset);
            b32(p + 8, segs[i].vaddr);
            b32(p + 12, segs[i].paddr);
            b32(p + 16, segs[i].filesz);
            b32(p + 20, segs[i].memsz);
        }
    }
};

struct Rig {
    std::vector<u8> ram = std::vector<u8>(0x100000, 0xFF);  // 0xFF so zero-fill is visible
    PhysicalBus bus;
    MemoryMap mm;
    Rig()
    {
        bus.map_ram(0, ram.data(), static_cast<u32>(ram.size()));
        mm.add_free(0x1000, 0x3F000);  // below load-base
    }
    void place(const Elf& e) { std::memcpy(&ram[kImage], e.d.data(), e.d.size()); }
};

// A typical image: note, text at 0x10000, data + bss at 0x20000.
Elf sample()
{
    Elf e({{4, 0x100, 0, 0, 12 + 8 + 24, 0},
           {1, 0x200, 0x10000, 0x10000, 0x40, 0x40},
           {1, 0x300, 0x20000, 0xFFFFFFFFu, 0x10, 0x1000}},
          0x10000, 0x400);
    // Note: namesz 8 "PowerPC\0", descsz 24, type 0x1275.
    e.b32(0x100, 8); e.b32(0x104, 24); e.b32(0x108, 0x1275);
    std::memcpy(&e.d[0x10C], "PowerPC", 8);
    const u32 desc[6] = {1, 0x00C00000, 0x00400000, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00400000};
    for (u32 i = 0; i < 6; ++i)
        e.b32(0x114 + 4 * i, desc[i]);
    for (u32 i = 0; i < 0x40; ++i) e.d[0x200 + i] = static_cast<u8>(0xA0 + i);
    for (u32 i = 0; i < 0x10; ++i) e.d[0x300 + i] = static_cast<u8>(0x50 + i);
    return e;
}

} // namespace

TEST_CASE("firmware/elf/load_segments_note_and_entry")
{
    Rig r;
    Elf e = sample();
    r.place(e);
    CHECK(is_elf_program(r.bus, kImage, 0x400));
    ElfProgram p;
    REQUIRE(load_elf_program(r.bus, &r.mm, kImage, 0x400, p) == ElfError::None);
    CHECK_EQ(p.entry, 0x10000u);
    CHECK_EQ(p.segments, 2u);
    CHECK_EQ(p.low, 0x10000u);
    CHECK_EQ(p.high, 0x21000u);
    CHECK(std::memcmp(&r.ram[0x10000], &e.d[0x200], 0x40) == 0);
    CHECK(std::memcmp(&r.ram[0x20000], &e.d[0x300], 0x10) == 0);
    CHECK_EQ(r.ram[0x20010], 0u);                             // bss zero-filled
    CHECK_EQ(r.ram[0x20FFF], 0u);
    CHECK_EQ(r.ram[0x21000], 0xFFu);                          // and no further
    CHECK_EQ(r.ram[0x10040], 0xFFu);
    REQUIRE(p.note.present);
    CHECK_EQ(p.note.real_mode, 1u);
    CHECK_EQ(p.note.real_base, 0x00C00000u);
    CHECK_EQ(p.note.load_base, 0x00400000u);
    CHECK(!r.mm.is_free(0x10000, 0x40));                      // segments are claimed
    CHECK(!r.mm.is_free(0x20000, 0x1000));
    // Loading again: the memory is taken now.
    r.place(e);
    CHECK(load_elf_program(r.bus, &r.mm, kImage, 0x400, p) == ElfError::ClaimFailed);
}

TEST_CASE("firmware/elf/recognition_rules")
{
    struct Case { size_t at; u8 value; ElfError want; };
    const Case cases[] = {
        {0, 0x7E, ElfError::NotElf},          // magic
        {4, 2, ElfError::NotElf},             // ELFCLASS64
        {5, 1, ElfError::LittleEndian},       // EI_DATA little-endian
        {17, 3, ElfError::NotExecutable},     // ET_DYN
        {19, 3, ElfError::WrongMachine},      // EM_386
        {23, 2, ElfError::BadVersion},
        {39, 1, ElfError::BadFlags},
    };
    for (const Case& c : cases) {
        Rig r;
        Elf e = sample();
        e.d[c.at] = c.value;
        r.place(e);
        ElfProgram p;
        CHECK(!is_elf_program(r.bus, kImage, 0x400));
        CHECK(load_elf_program(r.bus, &r.mm, kImage, 0x400, p) == c.want);
        CHECK_EQ(r.ram[0x10000], 0xFFu);                      // nothing was written
    }
    Rig r;
    r.place(sample());
    ElfProgram p;
    CHECK(load_elf_program(r.bus, &r.mm, kImage, 40, p) == ElfError::NotElf);     // too short
}

TEST_CASE("firmware/elf/bad_segments_and_overlap")
{
    {
        Rig r;
        Elf e({{1, 0x380, 0x10000, 0x10000, 0x100, 0x100}}, 0x10000, 0x400);  // past the image
        r.place(e);
        ElfProgram p;
        CHECK(load_elf_program(r.bus, &r.mm, kImage, 0x400, p) == ElfError::SegmentOutsideImage);
        CHECK(r.mm.is_free(0x10000, 0x100));                   // nothing claimed
    }
    {
        Rig r;
        Elf e({{1, 0x100, 0x10000, 0x10000, 0x20, 0x10}}, 0x10000, 0x400);   // filesz > memsz
        r.place(e);
        ElfProgram p;
        CHECK(load_elf_program(r.bus, &r.mm, kImage, 0x400, p) == ElfError::SegmentOutsideImage);
    }
    {
        Rig r;
        Elf e({{4, 0x100, 0, 0, 0, 0}}, 0x10000, 0x400);                     // no PT_LOAD
        r.place(e);
        ElfProgram p;
        CHECK(load_elf_program(r.bus, &r.mm, kImage, 0x400, p) == ElfError::BadHeaders);
    }
    {
        // A segment that moves down by 16 bytes inside the image itself.
        Rig r;
        Elf e({{1, 0x100, kImage + 0xF0, kImage + 0xF0, 0x80, 0x80}}, kImage + 0xF0, 0x400);
        for (u32 i = 0; i < 0x80; ++i) e.d[0x100 + i] = static_cast<u8>(i + 1);
        r.place(e);
        ElfProgram p;
        REQUIRE(load_elf_program(r.bus, nullptr, kImage, 0x400, p) == ElfError::None);
        for (u32 i = 0; i < 0x80; ++i)
            if (r.ram[kImage + 0xF0 + i] != static_cast<u8>(i + 1)) {
                CHECK(false);
                break;
            }
    }
    {
        // ... and one that moves up by 16 bytes.
        Rig r;
        Elf e({{1, 0x100, kImage + 0x110, kImage + 0x110, 0x80, 0x80}}, kImage + 0x110, 0x400);
        for (u32 i = 0; i < 0x80; ++i) e.d[0x100 + i] = static_cast<u8>(i + 1);
        r.place(e);
        ElfProgram p;
        REQUIRE(load_elf_program(r.bus, nullptr, kImage, 0x400, p) == ElfError::None);
        bool ok = true;
        for (u32 i = 0; i < 0x80; ++i)
            ok &= r.ram[kImage + 0x110 + i] == static_cast<u8>(i + 1);
        CHECK(ok);
    }
}

TEST_CASE("firmware/elf/go_runs_the_client_which_calls_firmware")
{
    using namespace asm_ppc;
    Rig r;
    std::fill(r.ram.begin(), r.ram.end(), 0);
    auto tree = std::make_unique<DeviceTree>();
    const Phandle cpus = tree->add_node(tree->root(), "cpus");
    Firmware fw(*tree, r.bus);
    REQUIRE(fw.install(0x3000));
    // Client: r3 = args (r6 = 0x8000 built below), call r5, store r3 and r4/r3 state.
    const u32 prog[] = {
        stw(3, 0x200, 0),                 // r3 on entry -> 0x200 (must be 0)
        stw(4, 0x204, 0),                 // r4 on entry -> 0x204 (must be 0)
        stw(1, 0x208, 0),                 // stack pointer
        mtspr(ppc::spr::CTR, 5),          // r5 = client interface entry
        lis(3, 0), ori(3, 3, 0x8000),
        bctr() | 1,                       // bctrl
        stw(3, 0x20C, 0),                 // call status
        b(0),
    };
    std::vector<u8> text(sizeof prog);
    for (u32 i = 0; i < sizeof prog / 4; ++i)
        for (u32 k = 0; k < 4; ++k)
            text[4 * i + k] = static_cast<u8>(prog[i] >> (24 - 8 * k));
    Elf e({{1, 0x100, 0x10000, 0x10000, static_cast<u32>(text.size()), static_cast<u32>(text.size())}},
          0x10000, 0x200);
    std::memcpy(&e.d[0x100], text.data(), text.size());
    r.place(e);
    // finddevice("/cpus") argument array at 0x8000.
    auto wr = [&](u32 a, u32 v) { for (u32 k = 0; k < 4; ++k) r.ram[a + k] = static_cast<u8>(v >> (24 - 8 * k)); };
    std::memcpy(&r.ram[0x8100], "finddevice", 11);
    std::memcpy(&r.ram[0x8120], "/cpus", 6);
    wr(0x8000, 0x8100); wr(0x8004, 1); wr(0x8008, 1); wr(0x800C, 0x8120);
    ElfProgram p;
    REQUIRE(load_elf_program(r.bus, nullptr, kImage, 0x200, p) == ElfError::None);
    ppc::Ppc750 cpu(r.bus);
    fw.attach(cpu);
    cpu.reset();
    cpu.state().gpr[3] = 0x1234;          // garbage that start_client must clear
    cpu.state().gpr[4] = 0x5678;
    fw.start_client(cpu.state(), p.entry, 0x7F000);
    CHECK_EQ(cpu.state().msr & (ppc::msr::IR | ppc::msr::DR | ppc::msr::EE), 0u);
    cpu.state().msr &= ~ppc::msr::IP;     // exception vectors at 0 for this test
    cpu.run(30);
    auto rd = [&](u32 a) { u32 v = 0; for (u32 k = 0; k < 4; ++k) v = (v << 8) | r.ram[a + k]; return v; };
    CHECK_EQ(rd(0x200), 0u);
    CHECK_EQ(rd(0x204), 0u);
    CHECK_EQ(rd(0x208) & 15u, 0u);        // aligned stack
    CHECK(rd(0x208) < 0x7F000u && rd(0x208) > 0x7E000u);
    CHECK_EQ(rd(0x20C), 0u);              // finddevice existed
    CHECK_EQ(rd(0x8010), cpus);           // and returned /cpus
    CHECK_EQ(fw.calls(), 1u);
}
