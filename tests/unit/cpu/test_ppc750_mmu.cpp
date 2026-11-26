// SPDX-License-Identifier: GPL-3.0-or-later
//
// MMU tests: BAT translation, hashed page table (primary/secondary hash,
// HTABMASK), R/C bits, protection keys, no-execute, DSI/ISI cause bits,
// page-crossing accesses and BAT priority.
//
// Page-table addresses are computed BY HAND from the PEM formulas (shown in
// the comments), not with the code under test.

#include "core/cpu/ppc750/ppc750.h"
#include "core/mem/flat_memory.h"
#include "tests/framework/g3test.h"
#include "tests/unit/cpu/ppc_asm.h"

#include <initializer_list>

using namespace g3;
using namespace g3::ppc;
using namespace asm_ppc;

namespace {

constexpr u32 kRam = 2 * 1024 * 1024;

struct MmuMachine {
    u8* ram = new u8[kRam]();
    FlatMemory mem{ram, kRam};
    Ppc750 cpu{mem};
    ~MmuMachine() { delete[] ram; }

    CpuState& s() { return cpu.state(); }
    void w32(u32 a, u32 v) { mem.write32(a, v); }
    u32 r32(u32 a)
    {
        u32 v = 0;
        mem.read32(a, v);
        return v;
    }
    // Runs one instruction placed at physical address 'pa' with PC = 'pc'.
    void run_at(u32 pc, u32 pa, u32 insn)
    {
        w32(pa, insn);
        s().pc = pc;
        cpu.run(1);  // run(): the direct RAM paths are in use, as in the machine
    }
};

// Example 1: SDR1 = 0x00010000 (HTABORG 0x00010000, HTABMASK 0 -> 64 KB).
// SR0 = VSID 0x123. EA 0x00003000: page index 3, API 0.
//   primary hash   = 0x123 ^ 3 = 0x120          -> PTEG 0x10000 + (0x120 << 6) = 0x14800
//   secondary hash = ~0x120 & 0x7FFFF = 0x7FEDF -> PTEG 0x10000 + (0x2DF << 6) = 0x1B7C0
//   PTE word 0 = V | VSID << 7 | H << 6 | API = 0x80009180 (H=0), 0x800091C0 (H=1)
constexpr u32 kSdr1 = 0x00010000u;
constexpr u32 kVsid = 0x123u;
constexpr u32 kPteg1 = 0x00014800u;
constexpr u32 kPteg2 = 0x0001B7C0u;
constexpr u32 kPte0Primary = 0x80009180u;
constexpr u32 kPte0Secondary = 0x800091C0u;

void setup_paged(MmuMachine& m, u32 sr_flags = 0)
{
    m.s().spr[spr::SDR1] = kSdr1;
    m.s().sr[0] = sr_flags | kVsid;
    m.s().msr = msr::DR | msr::IR;
}

} // namespace

TEST_CASE("cpu/mmu/real_mode_is_identity")
{
    MmuMachine m;
    m.w32(0x4000, 0xDEADBEEFu);
    m.s().gpr[3] = 0x4000;
    m.run_at(0x1000, 0x1000, lwz(4, 0, 3));
    CHECK_EQ(m.s().gpr[4], 0xDEADBEEFu);
}

TEST_CASE("cpu/mmu/dbat_translation_and_protection")
{
    MmuMachine m;
    // DBAT0: EA 0x80000000, 128 KB (BL=0), Vs=1 -> PA 0x00020000, PP=10 (RW).
    m.s().spr[spr::DBAT0U] = 0x80000000u | mmu::BATU_VS;
    m.s().spr[spr::DBAT0U + 1] = 0x00020000u | 2;
    m.s().msr = msr::DR;
    m.w32(0x00020010, 0x11223344u);
    m.s().gpr[3] = 0x80000010u;
    m.s().gpr[5] = 0xCAFEBABEu;
    m.run_at(0x1000, 0x1000, lwz(4, 0, 3));
    CHECK_EQ(m.s().gpr[4], 0x11223344u);
    m.run_at(0x1004, 0x1004, stw(5, 4, 3));
    CHECK_EQ(m.r32(0x00020014), 0xCAFEBABEu);

    // PP=01: read-only -> store gives DSI with PROTECTION | STORE.
    // (Direct state poke: flush the TLB as the mtspr would.)
    m.s().spr[spr::DBAT0U + 1] = 0x00020000u | 1;
    m.cpu.flush_tlb();
    m.run_at(0x1008, 0x1008, stw(5, 8, 3));
    CHECK_EQ(m.s().pc, 0x300u);
    CHECK_EQ(m.s().spr[spr::DSISR], dsisr::PROTECTION | dsisr::STORE);
    CHECK_EQ(m.s().spr[spr::DAR], 0x80000018u);
    CHECK_EQ(m.s().spr[spr::SRR0], 0x1008u);
}

TEST_CASE("cpu/mmu/bat_block_length_and_user_validity")
{
    MmuMachine m;
    // DBAT1: EA 0x40000000, 8 MB block (BL = 0x3F), Vp only -> PA 0x00000000.
    m.s().spr[spr::DBAT0U + 2] = 0x40000000u | (0x3Fu << 2) | mmu::BATU_VP;
    m.s().spr[spr::DBAT0U + 3] = 0x00000000u | 2;
    m.w32(0x00123450, 0x5A5A5A5Au);
    m.s().gpr[3] = 0x40123450u;  // inside the 8 MB block
    // Supervisor: Vs = 0, so the BAT does not apply -> page table -> no PTE.
    m.s().msr = msr::DR;
    m.s().spr[spr::SDR1] = kSdr1;
    m.run_at(0x1000, 0x1000, lwz(4, 0, 3));
    CHECK_EQ(m.s().pc, 0x300u);
    CHECK_EQ(m.s().spr[spr::DSISR], dsisr::NOT_FOUND);
    // User mode: Vp = 1 -> translated.
    m.s().msr = msr::DR | msr::PR;
    m.run_at(0x1000, 0x1000, lwz(4, 0, 3));
    CHECK_EQ(m.s().gpr[4], 0x5A5A5A5Au);
}

TEST_CASE("cpu/mmu/ibat_instruction_fetch")
{
    MmuMachine m;
    // IBAT0: EA 0xFFF00000 (128 KB) -> PA 0x00040000, PP=10.
    m.s().spr[spr::IBAT0U] = 0xFFF00000u | mmu::BATU_VS;
    m.s().spr[spr::IBAT0U + 1] = 0x00040000u | 2;
    m.s().msr = msr::IR;
    m.run_at(0xFFF00100u, 0x00040100u, li(7, 0x77));
    CHECK_EQ(m.s().gpr[7], 0x77u);
    CHECK_EQ(m.s().pc, 0xFFF00104u);
}

TEST_CASE("cpu/mmu/page_table_primary_hash_and_rc_bits")
{
    MmuMachine m;
    setup_paged(m);
    // Map EA 0x3000 -> PA 0x5000, PP=10, in slot 2 of the primary PTEG.
    m.w32(kPteg1 + 16, kPte0Primary);
    m.w32(kPteg1 + 20, 0x00005000u | 2);
    m.w32(0x5008, 0x01020304u);
    m.s().msr = msr::DR;  // instruction fetch untranslated
    m.s().gpr[3] = 0x3008;
    m.s().gpr[5] = 0xA5A5A5A5u;
    m.run_at(0x1000, 0x1000, lwz(4, 0, 3));
    CHECK_EQ(m.s().gpr[4], 0x01020304u);
    CHECK_EQ(m.r32(kPteg1 + 20), 0x00005000u | mmu::PTE1_R | 2);  // R set, C not
    m.run_at(0x1004, 0x1004, stw(5, 4, 3));
    CHECK_EQ(m.r32(0x500C), 0xA5A5A5A5u);
    CHECK_EQ(m.r32(kPteg1 + 20), 0x00005000u | mmu::PTE1_R | mmu::PTE1_C | 2);
}

TEST_CASE("cpu/mmu/page_table_secondary_hash")
{
    MmuMachine m;
    setup_paged(m);
    m.w32(kPteg2 + 56, kPte0Secondary);  // last slot of the secondary PTEG
    m.w32(kPteg2 + 60, 0x00007000u | 2);
    m.w32(0x7000, 0x0BADF00Du);
    m.s().msr = msr::DR;
    m.s().gpr[3] = 0x3000;
    m.run_at(0x1000, 0x1000, lwz(4, 0, 3));
    CHECK_EQ(m.s().gpr[4], 0x0BADF00Du);
}

TEST_CASE("cpu/mmu/htabmask_selects_pteg")
{
    // SDR1 = 0x00100001: HTABORG 0x00100000, HTABMASK 1 (128 KB table).
    // SR0 VSID 0x400, EA 0x3000: hash = 0x400 ^ 3 = 0x403; hash[0-8] = 1,
    // masked -> 1, so PA bits 7-15 = 0x0010 | 1 = 0x0011:
    // PTEG = 0x00110000 + (0x003 << 6) = 0x001100C0. PTE0 = 0x80020000.
    MmuMachine m;
    m.s().spr[spr::SDR1] = 0x00100001u;
    m.s().sr[0] = 0x400;
    m.w32(0x001100C0u, 0x80020000u);
    m.w32(0x001100C4u, 0x00009000u | 2);
    m.w32(0x9010, 0x13579BDFu);
    m.s().msr = msr::DR;
    m.s().gpr[3] = 0x3010;
    m.run_at(0x1000, 0x1000, lwz(4, 0, 3));
    CHECK_EQ(m.s().gpr[4], 0x13579BDFu);
}

TEST_CASE("cpu/mmu/page_not_found_dsi")
{
    MmuMachine m;
    setup_paged(m);
    m.s().msr = msr::DR;
    m.s().gpr[3] = 0x3004;
    m.s().gpr[5] = 1;
    m.run_at(0x1000, 0x1000, stw(5, 0, 3));
    CHECK_EQ(m.s().pc, 0x300u);
    CHECK_EQ(m.s().spr[spr::DSISR], dsisr::NOT_FOUND | dsisr::STORE);
    CHECK_EQ(m.s().spr[spr::DAR], 0x3004u);
    CHECK_EQ(m.s().spr[spr::SRR0], 0x1000u);
    CHECK_EQ(m.s().msr & msr::DR, 0u);  // exceptions turn translation off
}

TEST_CASE("cpu/mmu/protection_keys")
{
    // PP=01 with key=0 (supervisor, Ks=0): read/write.
    // PP=01 with key=1 (user, Kp=1): read-only -> store faults.
    MmuMachine m;
    setup_paged(m, mmu::SR_KP);
    m.w32(kPteg1, kPte0Primary);
    m.w32(kPteg1 + 4, 0x00005000u | 1);
    m.s().gpr[3] = 0x3000;
    m.s().gpr[5] = 0x42;
    m.s().msr = msr::DR;
    m.run_at(0x1000, 0x1000, stw(5, 0, 3));
    CHECK_EQ(m.r32(0x5000), 0x42u);  // supervisor may write
    m.s().msr = msr::DR | msr::PR;
    m.run_at(0x1000, 0x1000, lwz(6, 0, 3));
    CHECK_EQ(m.s().gpr[6], 0x42u);   // user may read
    m.s().msr = msr::DR | msr::PR;
    m.run_at(0x1004, 0x1004, stw(5, 0, 3));
    CHECK_EQ(m.s().pc, 0x300u);
    CHECK_EQ(m.s().spr[spr::DSISR], dsisr::PROTECTION | dsisr::STORE);
}

TEST_CASE("cpu/mmu/isi_not_found_and_no_execute")
{
    MmuMachine m;
    setup_paged(m);
    m.s().msr = msr::IR;
    m.s().pc = 0x3000;
    m.cpu.step();  // no PTE for the code page
    CHECK_EQ(m.s().pc, 0x400u);
    CHECK_EQ(m.s().spr[spr::SRR0], 0x3000u);
    CHECK_EQ(m.s().spr[spr::SRR1] & isi::NOT_FOUND, isi::NOT_FOUND);

    // Mapped, but the segment is no-execute.
    MmuMachine n;
    setup_paged(n, mmu::SR_N);
    n.w32(kPteg1, kPte0Primary);
    n.w32(kPteg1 + 4, 0x00005000u | 2);
    n.s().msr = msr::IR;
    n.s().pc = 0x3000;
    n.cpu.step();
    CHECK_EQ(n.s().pc, 0x400u);
    CHECK_EQ(n.s().spr[spr::SRR1] & isi::NO_EXECUTE, isi::NO_EXECUTE);
}

TEST_CASE("cpu/mmu/translated_instruction_fetch")
{
    MmuMachine m;
    setup_paged(m);
    m.w32(kPteg1, kPte0Primary);
    m.w32(kPteg1 + 4, 0x00005000u | 2);
    m.s().msr = msr::IR;
    m.run_at(0x3010, 0x5010, li(9, 0x99));
    CHECK_EQ(m.s().gpr[9], 0x99u);
    CHECK_EQ(m.s().pc, 0x3014u);
}

TEST_CASE("cpu/mmu/page_crossing_access")
{
    // EA 0x3FFE..0x4001 spans two pages mapped to unrelated physical pages:
    // 0x3000 -> 0x5000 and 0x4000 -> 0x8000.
    // Page 0x4000: page index 4, hash = 0x123 ^ 4 = 0x127 -> PTEG 0x10000 + 0x49C0 = 0x149C0.
    MmuMachine m;
    setup_paged(m);
    m.w32(kPteg1, kPte0Primary);
    m.w32(kPteg1 + 4, 0x00005000u | 2);
    m.w32(0x000149C0u, 0x80009180u);
    m.w32(0x000149C4u, 0x00008000u | 2);
    m.s().msr = msr::DR;
    m.s().gpr[3] = 0x3FFE;
    m.s().gpr[5] = 0xA1B2C3D4u;
    m.run_at(0x1000, 0x1000, stw(5, 0, 3));
    CHECK_EQ(m.ram[0x5FFE], 0xA1u);
    CHECK_EQ(m.ram[0x5FFF], 0xB2u);
    CHECK_EQ(m.ram[0x8000], 0xC3u);
    CHECK_EQ(m.ram[0x8001], 0xD4u);
    m.run_at(0x1004, 0x1004, lwz(6, 0, 3));
    CHECK_EQ(m.s().gpr[6], 0xA1B2C3D4u);

    // If the second page is unmapped, nothing is written to the first.
    // (PTE removed directly in memory: flush as the OS's tlbie would.)
    m.w32(0x000149C0u, 0);
    m.cpu.flush_tlb();
    m.ram[0x5FFE] = 0;
    m.run_at(0x1008, 0x1008, stw(5, 0, 3));
    CHECK_EQ(m.s().pc, 0x300u);
    CHECK_EQ(m.s().spr[spr::DAR], 0x4000u);
    CHECK_EQ(m.ram[0x5FFE], 0u);
}

TEST_CASE("cpu/mmu/bat_has_priority_over_page_table")
{
    MmuMachine m;
    setup_paged(m);
    m.w32(kPteg1, kPte0Primary);
    m.w32(kPteg1 + 4, 0x00005000u | 2);
    // DBAT0 maps EA 0x00000000 (128 KB) -> PA 0x00060000.
    m.s().spr[spr::DBAT0U] = 0x00000000u | mmu::BATU_VS;
    m.s().spr[spr::DBAT0U + 1] = 0x00060000u | 2;
    m.w32(0x00063000u, 0xBA7BA7BAu);
    m.w32(0x5000, 0x0000FFFFu);
    m.s().msr = msr::DR;
    m.s().gpr[3] = 0x3000;
    m.run_at(0x1000, 0x1000, lwz(4, 0, 3));
    CHECK_EQ(m.s().gpr[4], 0xBA7BA7BAu);
    CHECK_EQ(m.r32(kPteg1 + 4), 0x00005000u | 2);  // page table untouched (no R bit)
}

TEST_CASE("cpu/mmu/lwarx_stwcx_use_physical_reservation")
{
    MmuMachine m;
    setup_paged(m);
    m.w32(kPteg1, kPte0Primary);
    m.w32(kPteg1 + 4, 0x00005000u | 2);
    m.s().msr = msr::DR;
    m.s().gpr[3] = 0x3000;
    m.s().gpr[4] = 0;
    m.s().gpr[5] = 77;
    m.run_at(0x1000, 0x1000, lwarx(6, 3, 4));
    m.run_at(0x1004, 0x1004, stwcx(5, 3, 4));
    CHECK_EQ(m.r32(0x5000), 77u);
    CHECK_EQ(m.s().cr >> 28, crf::EQ);
}

// --- soft TLB ---------------------------------------------------------------------

TEST_CASE("cpu/mmu/tlb_keeps_stale_entry_until_tlbie")
{
    // Hardware semantics: after changing a PTE in memory, software must
    // execute tlbie; until then the old translation may still be used.
    MmuMachine m;
    setup_paged(m);
    m.w32(kPteg1, kPte0Primary);
    m.w32(kPteg1 + 4, 0x00005000u | 2);
    m.w32(0x5000, 0x55555555u);
    m.w32(0x6000, 0x66666666u);
    m.s().msr = msr::DR;
    m.s().gpr[3] = 0x3000;
    m.run_at(0x1000, 0x1000, lwz(4, 0, 3));
    CHECK_EQ(m.s().gpr[4], 0x55555555u);
    m.w32(kPteg1 + 4, 0x00006000u | 2);  // remap to 0x6000 without tlbie
    m.run_at(0x1004, 0x1004, lwz(4, 0, 3));
    CHECK_EQ(m.s().gpr[4], 0x55555555u);  // stale, as on hardware
    m.run_at(0x1008, 0x1008, X(31, 0, 0, 3, 306));  // tlbie r3
    m.run_at(0x100C, 0x100C, lwz(4, 0, 3));
    CHECK_EQ(m.s().gpr[4], 0x66666666u);
}

TEST_CASE("cpu/mmu/tlb_flushed_by_mtsr_mtspr")
{
    MmuMachine m;
    setup_paged(m);
    m.w32(kPteg1, kPte0Primary);
    m.w32(kPteg1 + 4, 0x00005000u | 2);
    m.w32(0x5000, 0x12121212u);
    m.s().msr = msr::DR;
    m.s().gpr[3] = 0x3000;
    m.run_at(0x1000, 0x1000, lwz(4, 0, 3));
    CHECK_EQ(m.s().gpr[4], 0x12121212u);
    // mtsr SR0 <- VSID 0x777 (no PTE for it): the cached translation must go.
    m.s().gpr[6] = 0x777;
    m.run_at(0x1004, 0x1004, mtsr(0, 6));
    m.run_at(0x1008, 0x1008, lwz(4, 0, 3));
    CHECK_EQ(m.s().pc, 0x300u);
    CHECK_EQ(m.s().spr[spr::DSISR], dsisr::NOT_FOUND);

    // mtspr DBAT0L: changing a BAT through the instruction flushes too.
    MmuMachine n;
    n.s().spr[spr::DBAT0U] = 0x80000000u | mmu::BATU_VS;
    n.s().spr[spr::DBAT0U + 1] = 0x00020000u | 2;
    n.s().msr = msr::DR;
    n.s().gpr[3] = 0x80000000u;
    n.s().gpr[5] = 0x00020001u;  // PP = 01: read-only
    n.run_at(0x1000, 0x1000, lwz(4, 0, 3));
    n.run_at(0x1004, 0x1004, mtspr(spr::DBAT0U + 1, 5));
    n.run_at(0x1008, 0x1008, stw(4, 0, 3));
    CHECK_EQ(n.s().pc, 0x300u);
    CHECK_EQ(n.s().spr[spr::DSISR], dsisr::PROTECTION | dsisr::STORE);
}

TEST_CASE("cpu/mmu/tlb_store_sets_c_bit_after_cached_read")
{
    // A read caches the page with C clear; a later store must still set C.
    MmuMachine m;
    setup_paged(m);
    m.w32(kPteg1, kPte0Primary);
    m.w32(kPteg1 + 4, 0x00005000u | 2);
    m.s().msr = msr::DR;
    m.s().gpr[3] = 0x3000;
    m.s().gpr[5] = 9;
    m.run_at(0x1000, 0x1000, lwz(4, 0, 3));
    CHECK_EQ(m.r32(kPteg1 + 4) & mmu::PTE1_C, 0u);
    m.run_at(0x1004, 0x1004, stw(5, 0, 3));
    CHECK_EQ(m.r32(kPteg1 + 4) & mmu::PTE1_C, mmu::PTE1_C);
}

TEST_CASE("cpu/mmu/tlb_user_supervisor_contexts_are_separate")
{
    // PP=00 with Ks=0 (supervisor: R/W) and Kp=1 (user: no access).
    MmuMachine m;
    setup_paged(m, mmu::SR_KP);
    m.w32(kPteg1, kPte0Primary);
    m.w32(kPteg1 + 4, 0x00005000u | 0);
    m.s().gpr[3] = 0x3000;
    m.s().msr = msr::DR;
    m.run_at(0x1000, 0x1000, lwz(4, 0, 3));  // supervisor: cached OK
    CHECK_EQ(m.s().pc, 0x1004u);
    m.s().msr = msr::DR | msr::PR;
    m.run_at(0x1004, 0x1004, lwz(4, 0, 3));  // user: must not hit that entry
    CHECK_EQ(m.s().pc, 0x300u);
    CHECK_EQ(m.s().spr[spr::DSISR], dsisr::PROTECTION);
}

// --- fetch-page cache (instructions fetched straight from host RAM) --------

TEST_CASE("cpu/mmu/fetch_cache_follows_ibat_change")
{
    // Several instructions in ONE run() call: the IBAT is rewritten by the
    // guest, and the next fetch must use the new mapping, not the cached page.
    MmuMachine m;
    m.s().spr[spr::IBAT0U] = 0x10000000u | mmu::BATU_VS;     // EA 0x10000000, 128 KB
    m.s().spr[spr::IBAT0U + 1] = 0x00040000u | 2;            // -> PA 0x40000
    m.s().msr = msr::IR;
    m.s().gpr[5] = 0x00060000u | 2;                          // new mapping -> PA 0x60000
    m.w32(0x40000, li(7, 1));
    m.w32(0x40004, mtspr(spr::IBAT0U + 1, 5));
    m.w32(0x40008, isync());
    m.w32(0x4000C, li(7, 2));                                // old page: must not run
    m.w32(0x60008, isync());     // the fetch right after mtspr already uses the new page
    m.w32(0x6000C, li(7, 3));                                // new page
    m.s().pc = 0x10000000u;
    m.cpu.run(4);
    CHECK_EQ(m.s().gpr[7], 3u);
    CHECK_EQ(m.s().pc, 0x10000010u);
}

TEST_CASE("cpu/mmu/fetch_cache_follows_msr_ir")
{
    // Real-mode code turns instruction translation on: the following fetch
    // goes through the IBAT even though the page number is unchanged.
    MmuMachine m;
    m.s().spr[spr::IBAT0U] = 0x00000000u | mmu::BATU_VS;     // EA 0, 128 KB
    m.s().spr[spr::IBAT0U + 1] = 0x00040000u | 2;            // -> PA 0x40000
    m.s().msr = 0;
    m.s().gpr[5] = msr::IR;
    m.w32(0x1000, li(7, 1));
    m.w32(0x1004, mtmsr(5));
    m.w32(0x1008, li(7, 2));                                 // real-mode page: must not run
    m.w32(0x41008, li(7, 3));                                // translated page
    m.s().pc = 0x1000;
    m.cpu.run(3);
    CHECK_EQ(m.s().gpr[7], 3u);
}

TEST_CASE("cpu/mmu/fetch_sees_stores_to_its_own_page")
{
    // An interpreter has no stale instruction cache: a store to the next
    // instruction (after icbi/isync, as the architecture requires) runs.
    MmuMachine m;
    m.s().msr = 0;
    m.s().gpr[3] = 0x100C;
    m.s().gpr[4] = li(7, 9);
    m.w32(0x1000, stw(4, 0, 3));
    m.w32(0x1004, icbi(0, 3));
    m.w32(0x1008, isync());
    m.w32(0x100C, li(7, 1));
    m.s().pc = 0x1000;
    m.cpu.run(4);
    CHECK_EQ(m.s().gpr[7], 9u);
}

TEST_CASE("cpu/code_watch_reports_page_entries")
{
    // b to a watched page, run there, branch back: one report per entry.
    MmuMachine m;
    m.s().msr = 0;
    m.w32(0x1000, b(0x2000 - 0x1000));   // -> 0x2000
    m.w32(0x2000, li(7, 1));
    m.w32(0x2004, b(0x1004 - 0x2004));   // -> 0x1004
    m.w32(0x1004, b(0x2008 - 0x1004));   // -> 0x2008
    m.w32(0x2008, b(0));                 // spin
    struct Log { u32 n = 0; u32 first = 0; } log;
    m.cpu.set_code_watch(0x2000, 0x2FFF, [](void* c, u32 pc) {
        auto* l = static_cast<Log*>(c);
        if (!l->n++) l->first = pc;
    }, &log);
    m.s().pc = 0x1000;
    m.cpu.run(10);
    CHECK_EQ(log.first, 0x2000u);
    CHECK_EQ(log.n, 2u);          // entered twice; the spin stays on the cached page
    CHECK_EQ(m.s().gpr[7], 1u);
}
