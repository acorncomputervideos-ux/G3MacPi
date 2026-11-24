// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/cpu/ppc750/jit/a64_assembler.cpp — see a64_assembler.h.
//
// Encodings: Arm ARM (DDI 0487), section C4 "A64 Instruction Set
// Encoding"; the field layouts are given next to each function.

#include "core/cpu/ppc750/jit/a64_assembler.h"

namespace g3::a64 {

namespace {
constexpr u32 sf(bool x) { return x ? 0x80000000u : 0u; }
constexpr u32 kBranchCb = 0;   // imm19 at bits 5..23 (CBZ/CBNZ, B.cond)
constexpr u32 kBranchB = 1;    // imm26 at bits 0..25
constexpr u32 kBranchTb = 2;   // imm14 at bits 5..18 (TBZ/TBNZ)
} // namespace

void Assembler::emit(u32 insn)
{
    if (!ok_ || pos_ >= cap_) {
        ok_ = false;
        return;
    }
    buf_[pos_++] = insn;
}

// --- moves --------------------------------------------------------------------------
// MOVN/MOVZ/MOVK (move wide immediate): sf opc(2) 100101 hw(2) imm16 Rd;
// opc 00 = MOVN, 10 = MOVZ, 11 = MOVK.

static u32 move_wide(bool x, u32 opc, Reg rd, u32 imm16, u32 shift)
{
    return sf(x) | (opc << 29) | (0x25u << 23) | ((shift / 16) << 21) | ((imm16 & 0xFFFF) << 5) | rd;
}

void Assembler::movz(bool x, Reg rd, u32 imm16, u32 shift) { emit(move_wide(x, 2, rd, imm16, shift)); }
void Assembler::movk(bool x, Reg rd, u32 imm16, u32 shift) { emit(move_wide(x, 3, rd, imm16, shift)); }
void Assembler::movn(bool x, Reg rd, u32 imm16, u32 shift) { emit(move_wide(x, 0, rd, imm16, shift)); }

void Assembler::mov_imm32(Reg wd, u32 v)
{
    const u32 lo = v & 0xFFFF, hi = v >> 16;
    if (hi == 0) {
        movz(false, wd, lo, 0);
    } else if (lo == 0) {
        movz(false, wd, hi, 16);
    } else if (hi == 0xFFFF) {
        movn(false, wd, ~lo & 0xFFFF, 0);        // ~(~lo) with the top half all ones
    } else if (lo == 0xFFFF) {
        movn(false, wd, ~hi & 0xFFFF, 16);
    } else {
        movz(false, wd, lo, 0);
        movk(false, wd, hi, 16);
    }
}

void Assembler::mov_imm64(Reg xd, u64 v)
{
    bool first = true;
    for (u32 s = 0; s < 64; s += 16) {
        const u32 part = static_cast<u32>(v >> s) & 0xFFFF;
        if (part == 0)
            continue;
        if (first)
            movz(true, xd, part, s);
        else
            movk(true, xd, part, s);
        first = false;
    }
    if (first)
        movz(true, xd, 0, 0);
}

// ORR (shifted register): sf 01 01010 shift(2) 0 Rm imm6 Rn Rd.
void Assembler::mov(bool x, Reg rd, Reg rm) { orr(x, rd, ZR, rm); }

// ADD (immediate) with Rn = SP: see add_imm.
void Assembler::mov_from_sp(Reg xd) { add_imm(true, xd, SP, 0); }

// --- arithmetic and logic -----------------------------------------------------------
// ADD/SUB (immediate): sf op S 100010 sh imm12 Rn Rd; op 0 = ADD, 1 = SUB.

void Assembler::add_imm(bool x, Reg rd, Reg rn, u32 imm12, bool lsl12)
{
    emit(sf(x) | 0x11000000u | (lsl12 ? 1u << 22 : 0) | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}

void Assembler::sub_imm(bool x, Reg rd, Reg rn, u32 imm12, bool lsl12)
{
    emit(sf(x) | 0x51000000u | (lsl12 ? 1u << 22 : 0) | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}

// ADD/SUB (shifted register), shift type LSL: sf op S 01011 00 0 Rm imm6 Rn Rd.
static u32 shifted(u32 base, bool x, Reg rd, Reg rn, Reg rm, u32 lsl)
{
    return sf(x) | base | (rm << 16) | ((lsl & 63) << 10) | (rn << 5) | rd;
}
void Assembler::add(bool x, Reg rd, Reg rn, Reg rm, u32 lsl) { emit(shifted(0x0B000000u, x, rd, rn, rm, lsl)); }
void Assembler::sub(bool x, Reg rd, Reg rn, Reg rm, u32 lsl) { emit(shifted(0x4B000000u, x, rd, rn, rm, lsl)); }
// SUBS (shifted register) with Rd = ZR is CMP; SUBS (immediate) likewise.
void Assembler::cmp(bool x, Reg rn, Reg rm) { emit(shifted(0x6B000000u, x, ZR, rn, rm, 0)); }
void Assembler::cmp_imm(bool x, Reg rn, u32 imm12) { emit(sf(x) | 0x71000000u | ((imm12 & 0xFFF) << 10) | (rn << 5) | 31); }

// Logical (shifted register), shift type LSL: sf opc 01010 00 N Rm imm6 Rn Rd;
// opc 00 = AND, 01 = ORR, 10 = EOR.
void Assembler::and_(bool x, Reg rd, Reg rn, Reg rm, u32 lsl) { emit(shifted(0x0A000000u, x, rd, rn, rm, lsl)); }
void Assembler::orr(bool x, Reg rd, Reg rn, Reg rm, u32 lsl) { emit(shifted(0x2A000000u, x, rd, rn, rm, lsl)); }
void Assembler::eor(bool x, Reg rd, Reg rn, Reg rm, u32 lsl) { emit(shifted(0x4A000000u, x, rd, rn, rm, lsl)); }

// UBFM: sf 10 100110 N immr imms Rn Rd (N = sf).
void Assembler::ubfm(bool x, Reg rd, Reg rn, u32 immr, u32 imms)
{
    emit(sf(x) | (x ? 1u << 22 : 0) | 0x53000000u | ((immr & 63) << 16) | ((imms & 63) << 10) | (rn << 5) | rd);
}

// SBFM: sf 00 100110 N immr imms Rn Rd (N = sf).
void Assembler::sbfm(bool x, Reg rd, Reg rn, u32 immr, u32 imms)
{
    emit(sf(x) | (x ? 1u << 22 : 0) | 0x13000000u | ((immr & 63) << 16) | ((imms & 63) << 10) | (rn << 5) | rd);
}

// BFM: sf 01 100110 N immr imms Rn Rd (N = sf).
void Assembler::bfm(bool x, Reg rd, Reg rn, u32 immr, u32 imms)
{
    emit(sf(x) | (x ? 1u << 22 : 0) | 0x33000000u | ((immr & 63) << 16) | ((imms & 63) << 10) | (rn << 5) | rd);
}

// CSINC: sf 0 0 11010100 Rm cond 0 1 Rn Rd.
void Assembler::csinc(bool x, Reg rd, Reg rn, Reg rm, Cond c)
{
    emit(sf(x) | 0x1A800400u | (rm << 16) | (static_cast<u32>(c) << 12) | (rn << 5) | rd);
}

// REV / REV16 (32-bit): 0 1 0 11010110 00000 0000 opc(2) Rn Rd; opc 10 = REV, 01 = REV16.
void Assembler::rev(Reg wd, Reg wn) { emit(0x5AC00800u | (wn << 5) | wd); }
void Assembler::rev16(Reg wd, Reg wn) { emit(0x5AC00400u | (wn << 5) | wd); }

// EXTR: sf 00 100111 N 0 Rm imms Rn Rd (N = sf).
void Assembler::extr(bool x, Reg rd, Reg rn, Reg rm, u32 lsb)
{
    emit(sf(x) | (x ? 1u << 22 : 0) | 0x13800000u | (rm << 16) | ((lsb & 63) << 10) | (rn << 5) | rd);
}

// --- loads and stores ---------------------------------------------------------------
// LDR/STR (immediate, unsigned offset): size(2) 111 0 01 opc(2) imm12 Rn Rt;
// opc 00 = STR, 01 = LDR; imm12 = offset / size.

void Assembler::ldr(bool x, Reg rt, Reg rn, u32 off)
{
    emit((x ? 0xF9400000u : 0xB9400000u) | (((off / (x ? 8 : 4)) & 0xFFF) << 10) | (rn << 5) | rt);
}
void Assembler::str(bool x, Reg rt, Reg rn, u32 off)
{
    emit((x ? 0xF9000000u : 0xB9000000u) | (((off / (x ? 8 : 4)) & 0xFFF) << 10) | (rn << 5) | rt);
}
void Assembler::ldrb(Reg wt, Reg rn, u32 off) { emit(0x39400000u | ((off & 0xFFF) << 10) | (rn << 5) | wt); }
void Assembler::strb(Reg wt, Reg rn, u32 off) { emit(0x39000000u | ((off & 0xFFF) << 10) | (rn << 5) | wt); }

// LDR/STR (register): size(2) 111 0 00 opc(2) 1 Rm option(3) S 10 Rn Rt, with
// option 011 (LSL / UXTX, X index) and S = 0 (no shift).
static u32 reg_offset(u32 size, bool load, Reg wt, Reg xn, Reg xm)
{
    const u32 sz = size == 4 ? 2u : size == 2 ? 1u : 0u;
    return (sz << 30) | 0x38206800u | (load ? 1u << 22 : 0) | (xm << 16) | (xn << 5) | wt;
}
void Assembler::ldr_reg(u32 size, Reg wt, Reg xn, Reg xm) { emit(reg_offset(size, true, wt, xn, xm)); }
void Assembler::str_reg(u32 size, Reg wt, Reg xn, Reg xm) { emit(reg_offset(size, false, wt, xn, xm)); }

// LDP/STP (64-bit): 10 101 0 0 mode(3) L imm7 Rt2 Rn Rt; mode 001 post-index,
// 010 signed offset, 011 pre-index; L 0 = store, 1 = load; imm7 = offset / 8.
static u32 pair(u32 mode, bool load, Reg rt, Reg rt2, Reg rn, s32 off)
{
    return 0xA8000000u | (mode << 23) | (load ? 1u << 22 : 0) | ((static_cast<u32>(off / 8) & 0x7F) << 15) |
           (rt2 << 10) | (rn << 5) | rt;
}
void Assembler::stp(Reg rt, Reg rt2, Reg rn, s32 off) { emit(pair(2, false, rt, rt2, rn, off)); }
void Assembler::stp_pre(Reg rt, Reg rt2, Reg rn, s32 off) { emit(pair(3, false, rt, rt2, rn, off)); }
void Assembler::ldp(Reg rt, Reg rt2, Reg rn, s32 off) { emit(pair(2, true, rt, rt2, rn, off)); }
void Assembler::ldp_post(Reg rt, Reg rt2, Reg rn, s32 off) { emit(pair(1, true, rt, rt2, rn, off)); }

// --- branches -----------------------------------------------------------------------
// BLR: 1101011 0001 11111 000000 Rn 00000. RET (x30): 0xD65F03C0.
// CBNZ: sf 011010 1 imm19 Rt. B: 000101 imm26. NOP: 0xD503201F.

void Assembler::blr(Reg rn) { emit(0xD63F0000u | (rn << 5)); }
void Assembler::br(Reg rn) { emit(0xD61F0000u | (rn << 5)); }
void Assembler::ret() { emit(0xD65F03C0u); }
void Assembler::nop() { emit(0xD503201Fu); }

void Assembler::cbnz(bool x, Reg rt, Label& l) { branch_to(l, sf(x) | 0x35000000u | rt, kBranchCb); }
void Assembler::cbz(bool x, Reg rt, Label& l) { branch_to(l, sf(x) | 0x34000000u | rt, kBranchCb); }
void Assembler::b(Label& l) { branch_to(l, 0x14000000u, kBranchB); }
// B.cond: 0101010 0 imm19 0 cond. TBZ/TBNZ: b5 011011 op b40(5) imm14 Rt.
void Assembler::b_cond(Cond c, Label& l) { branch_to(l, 0x54000000u | c, kBranchCb); }
void Assembler::tbz(Reg rt, u32 bit, Label& l)
{
    branch_to(l, ((bit >> 5) << 31) | 0x36000000u | ((bit & 31) << 19) | rt, kBranchTb);
}
void Assembler::tbnz(Reg rt, u32 bit, Label& l)
{
    branch_to(l, ((bit >> 5) << 31) | 0x37000000u | ((bit & 31) << 19) | rt, kBranchTb);
}

void Assembler::patch(u32* at, u32 from, u32 to, u32 kind)
{
    const s32 delta = static_cast<s32>(to) - static_cast<s32>(from);  // in words
    if (kind == kBranchCb)
        *at = (*at & ~(0x7FFFFu << 5)) | ((static_cast<u32>(delta) & 0x7FFFF) << 5);
    else if (kind == kBranchTb)
        *at = (*at & ~(0x3FFFu << 5)) | ((static_cast<u32>(delta) & 0x3FFF) << 5);
    else
        *at = (*at & ~0x3FFFFFFu) | (static_cast<u32>(delta) & 0x3FFFFFF);
}

void Assembler::branch_to(Label& l, u32 insn, u32 kind)
{
    const u32 at = pos_;
    emit(insn);
    if (!ok_)
        return;
    if (l.bound) {
        patch(&buf_[at], at, l.pos, kind);
    } else if (l.npending < sizeof l.pending / sizeof l.pending[0]) {
        l.pending[l.npending++] = at | (kind << 30);
    } else {
        ok_ = false;  // too many forward references to one label
    }
}

void Assembler::bind(Label& l)
{
    l.pos = pos_;
    l.bound = true;
    if (!ok_)
        return;
    for (u32 i = 0; i < l.npending; ++i) {
        const u32 at = l.pending[i] & 0x3FFFFFFFu;
        patch(&buf_[at], at, pos_, l.pending[i] >> 30);
    }
    l.npending = 0;
}

} // namespace g3::a64
