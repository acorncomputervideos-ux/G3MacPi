// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/cpu/ppc750/ppc750_loadstore.cpp — integer load/store instructions
// (PEM 4.2.3), load-reserve/store-conditional, load/store multiple and
// string instructions, and dcbz.
//
// On a failed memory access the CPU takes a machine check (bus error) and
// the instruction has no further effect: target registers and rA (update
// forms) are only written after all memory accesses succeeded, except for
// lmw/lsw*, which architecturally may be partially done when interrupted.

#include "core/common/bitops.h"
#include "core/common/endian.h"
#include "core/cpu/ppc750/ppc750.h"

namespace g3::ppc {

namespace {

struct LsForm {
    u8 size;       // bytes: 1, 2, 4
    bool store;
    bool update;   // rA <- EA
    bool sign;     // lha/lhau/lhax/lhaux
    bool reverse;  // byte-reversed (lhbrx, lwbrx, sthbrx, stwbrx)
};

// D-form opcodes 32..45.
constexpr LsForm kDForm[14] = {
    {4, false, false, false, false},  // 32 lwz
    {4, false, true, false, false},   // 33 lwzu
    {1, false, false, false, false},  // 34 lbz
    {1, false, true, false, false},   // 35 lbzu
    {4, true, false, false, false},   // 36 stw
    {4, true, true, false, false},    // 37 stwu
    {1, true, false, false, false},   // 38 stb
    {1, true, true, false, false},    // 39 stbu
    {2, false, false, false, false},  // 40 lhz
    {2, false, true, false, false},   // 41 lhzu
    {2, false, false, true, false},   // 42 lha
    {2, false, true, true, false},    // 43 lhau
    {2, true, false, false, false},   // 44 sth
    {2, true, true, false, false},    // 45 sthu
};

bool x_form(u32 xo, LsForm& f)
{
    switch (xo) {
    case 23:  f = {4, false, false, false, false}; return true;  // lwzx
    case 55:  f = {4, false, true, false, false}; return true;   // lwzux
    case 87:  f = {1, false, false, false, false}; return true;  // lbzx
    case 119: f = {1, false, true, false, false}; return true;   // lbzux
    case 151: f = {4, true, false, false, false}; return true;   // stwx
    case 183: f = {4, true, true, false, false}; return true;    // stwux
    case 215: f = {1, true, false, false, false}; return true;   // stbx
    case 247: f = {1, true, true, false, false}; return true;    // stbux
    case 279: f = {2, false, false, false, false}; return true;  // lhzx
    case 311: f = {2, false, true, false, false}; return true;   // lhzux
    case 343: f = {2, false, false, true, false}; return true;   // lhax
    case 375: f = {2, false, true, true, false}; return true;    // lhaux
    case 407: f = {2, true, false, false, false}; return true;   // sthx
    case 439: f = {2, true, true, false, false}; return true;    // sthux
    case 534: f = {4, false, false, false, true}; return true;   // lwbrx
    case 790: f = {2, false, false, false, true}; return true;   // lhbrx
    case 662: f = {4, true, false, false, true}; return true;    // stwbrx
    case 918: f = {2, true, false, false, true}; return true;    // sthbrx
    default:  return false;
    }
}

} // namespace

// --- memory helpers ----------------------------------------------------------
//
// All data accesses go through these. They translate the effective address
// (ppc750_mmu.cpp) and access physical memory. On a translation fault they
// raise DSI, on a bus error a machine check, and return false.
//
// An access that crosses a 4 KB page boundary with translation enabled is
// split: both pages are translated first (so a fault on the second page
// leaves nothing half-written), then the bytes are transferred one by one.

namespace {
constexpr bool crosses_page(u32 ea, u32 size) { return (ea & 0xFFF) + size > 0x1000; }
} // namespace

bool Ppc750::mem_access(u32 ea, u32 size, bool write, u32& value)
{
    const Access acc = write ? Access::Write : Access::Read;
    if ((s_.msr & msr::DR) && crosses_page(ea, size)) {
        const u32 second = (ea + size - 1) & ~0xFFFu;
        u32 pa1, pa2;
        if (!translate(ea, acc, pa1) || !translate(second, acc, pa2))
            return false;
        u32 v = 0;
        for (u32 i = 0; i < size; ++i) {
            const u32 e = ea + i;
            const u32 pa = e < second ? pa1 + i : pa2 + (e - second);
            const u32 shift = 8 * (size - 1 - i);
            if (write) {
                if (!mem_.write8(pa, static_cast<u8>(value >> shift))) {
                    machine_check();
                    return false;
                }
            } else {
                u8 b;
                if (!mem_.read8(pa, b)) {
                    machine_check();
                    return false;
                }
                v |= static_cast<u32>(b) << shift;
            }
        }
        if (!write)
            value = v;
        return true;
    }

    u32 pa;
    if (!translate(ea, acc, pa))
        return false;
    if (u8* p = ram_at(pa, size)) {  // plain RAM: no bus dispatch
        if (write) {
            if (size == 4) store_be32(p, value);
            else if (size == 2) store_be16(p, static_cast<u16>(value));
            else *p = static_cast<u8>(value);
        } else {
            value = size == 4 ? load_be32(p) : size == 2 ? load_be16(p) : *p;
        }
        return true;
    }
    bool ok;
    if (write) {
        ok = size == 4 ? mem_.write32(pa, value)
           : size == 2 ? mem_.write16(pa, static_cast<u16>(value))
                       : mem_.write8(pa, static_cast<u8>(value));
    } else {
        if (size == 4) {
            ok = mem_.read32(pa, value);
        } else if (size == 2) {
            u16 h;
            ok = mem_.read16(pa, h);
            value = h;
        } else {
            u8 b;
            ok = mem_.read8(pa, b);
            value = b;
        }
    }
    if (!ok) {
        machine_check();
        return false;
    }
    return true;
}


u32 Ppc750::ea_d(u32 insn) const
{
    const u32 ra = field::ra(insn);
    return (ra ? s_.gpr[ra] : 0) + field::simm(insn);
}

u32 Ppc750::ea_x(u32 insn) const
{
    const u32 ra = field::ra(insn);
    return (ra ? s_.gpr[ra] : 0) + s_.gpr[field::rb(insn)];
}

// --- common load/store path --------------------------------------------------

namespace {
inline u32 reverse_bytes(u32 v, u32 size)
{
    return size == 4 ? byteswap32(v) : (size == 2 ? byteswap16(static_cast<u16>(v)) : v);
}
} // namespace

bool Ppc750::load_store_d(u32 insn, u32 opcode)
{
    const LsForm& f = kDForm[opcode - 32];
    const u32 ea = ea_d(insn);
    const u32 rd = field::rd(insn);
    u32 v = 0;

    // Invalid forms (update with rA = 0, or load-update with rA = rD) are
    // executed like valid ones; their result is architecturally undefined.
    if (f.store) {
        v = s_.gpr[rd];
        const bool ok = f.size == 4 ? mem_write32(ea, v)
                      : f.size == 2 ? mem_write16(ea, v)
                                    : mem_write8(ea, v);
        if (!ok)
            return true;
    } else {
        const bool ok = f.size == 4 ? mem_read32(ea, v)
                      : f.size == 2 ? mem_read16(ea, v)
                                    : mem_read8(ea, v);
        if (!ok)
            return true;
        if (f.sign)
            v = static_cast<u32>(sign_extend16(v));
        s_.gpr[rd] = v;
    }
    if (f.update)
        s_.gpr[field::ra(insn)] = ea;
    return true;
}

// Returns false if 'xo' is not an integer load/store (caller: illegal).
bool Ppc750::load_store_x(u32 insn, u32 xo)
{
    LsForm f;
    if (!x_form(xo, f))
        return false;

    const u32 ea = ea_x(insn);
    const u32 rd = field::rd(insn);
    u32 v = 0;

    if (f.store) {
        v = s_.gpr[rd];
        if (f.reverse)
            v = reverse_bytes(v, f.size);
        const bool ok = f.size == 4 ? mem_write32(ea, v)
                      : f.size == 2 ? mem_write16(ea, v)
                                    : mem_write8(ea, v);
        if (!ok)
            return true;
    } else {
        const bool ok = f.size == 4 ? mem_read32(ea, v)
                      : f.size == 2 ? mem_read16(ea, v)
                                    : mem_read8(ea, v);
        if (!ok)
            return true;
        if (f.reverse)
            v = reverse_bytes(v, f.size);
        if (f.sign)
            v = static_cast<u32>(sign_extend16(v));
        s_.gpr[rd] = v;
    }
    if (f.update)
        s_.gpr[field::ra(insn)] = ea;
    return true;
}

// --- special forms -------------------------------------------------------------

bool Ppc750::op_lmw_stmw(u32 insn, bool store)
{
    const u32 ea = ea_d(insn);
    if (ea & 3) {
        // 750: lmw/stmw with a non-word-aligned EA cause an alignment
        // exception. DSISR is not filled in yet (docs/hardware/ppc750-cpu.md).
        s_.spr[spr::DAR] = ea;
        raise_exception(Exception::Alignment, s_.pc);
        return true;
    }
    u32 addr = ea;
    for (u32 r = field::rd(insn); r < 32; ++r, addr += 4) {
        if (store) {
            if (!mem_write32(addr, s_.gpr[r]))
                return true;
        } else {
            u32 v;
            if (!mem_read32(addr, v))
                return true;
            s_.gpr[r] = v;
        }
    }
    return true;
}

bool Ppc750::op_string(u32 insn, bool store, u32 nbytes, u32 ea)
{
    // Bytes go to/from registers starting at rD/rS, high-order byte first,
    // wrapping from r31 to r0. For loads, unfilled low-order bytes of the
    // last register are cleared (PEM lswi/lswx). nbytes == 0: no effect.
    u32 r = field::rd(insn);
    u32 v = 0;
    for (u32 i = 0; i < nbytes; ++i) {
        const u32 pos = i & 3;
        if (store) {
            const u32 byte = (s_.gpr[r] >> (24 - 8 * pos)) & 0xFF;
            if (!mem_write8(ea + i, byte))
                return true;
            if (pos == 3)
                r = (r + 1) & 31;
        } else {
            u32 byte;
            if (!mem_read8(ea + i, byte))
                return true;
            v |= byte << (24 - 8 * pos);
            if (pos == 3 || i == nbytes - 1) {
                s_.gpr[r] = v;
                v = 0;
                r = (r + 1) & 31;
            }
        }
    }
    return true;
}

bool Ppc750::op_lwarx(u32 insn)
{
    const u32 ea = ea_x(insn);
    if (ea & 3) {
        s_.spr[spr::DAR] = ea;
        raise_exception(Exception::Alignment, s_.pc);
        return true;
    }
    u32 pa, v;
    if (!translate(ea, Access::Read, pa))
        return true;
    if (!mem_.read32(pa, v)) {
        machine_check();
        return true;
    }
    s_.gpr[field::rd(insn)] = v;
    // The reservation is on the physical address; the 750's reservation
    // granule is one 32-byte cache block.
    s_.reservation_valid = true;
    s_.reservation_addr = pa & ~31u;
    return true;
}

bool Ppc750::op_stwcx(u32 insn)
{
    const u32 ea = ea_x(insn);
    if (ea & 3) {
        s_.spr[spr::DAR] = ea;
        raise_exception(Exception::Alignment, s_.pc);
        return true;
    }
    u32 pa;
    if (!translate(ea, Access::Write, pa))
        return true;
    const bool success = s_.reservation_valid && s_.reservation_addr == (pa & ~31u);
    s_.reservation_valid = false;
    if (success && !mem_.write32(pa, s_.gpr[field::rd(insn)])) {
        machine_check();
        return true;
    }
    u32 f = success ? crf::EQ : 0;
    if (s_.xer() & xer::SO)
        f |= crf::SO;
    set_cr_field(0, f);
    return true;
}

bool Ppc750::op_dcbz(u32 insn)
{
    // Zeroes the 32-byte cache block containing EA (750 cache block size).
    const u32 block = ea_x(insn) & ~31u;
    for (u32 off = 0; off < 32; off += 4)
        if (!mem_write32(block + off, 0))
            return true;
    return true;
}

} // namespace g3::ppc
