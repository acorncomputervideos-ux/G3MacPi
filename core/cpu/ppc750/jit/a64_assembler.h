// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/cpu/ppc750/jit/a64_assembler.h — a small AArch64 instruction
// encoder for the JIT (docs/cpu/jit-a64.md).
//
// Only the instructions the code generator uses. Encodings follow the
// Arm Architecture Reference Manual for A-profile (A64 base instruction
// encodings); tests/unit/cpu/test_a64_assembler.cpp checks each one against
// bytes produced by the GNU assembler (tools/jit/a64_vectors.sh).
//
// Freestanding: writes into a caller-supplied buffer; never allocates. If
// the buffer runs out, ok() becomes false and nothing more is written.

#ifndef G3_CORE_CPU_PPC750_JIT_A64_ASSEMBLER_H
#define G3_CORE_CPU_PPC750_JIT_A64_ASSEMBLER_H

#include "core/common/types.h"

namespace g3::a64 {

// Register numbers. 31 is SP or the zero register depending on the
// instruction, as in the architecture.
enum Reg : u32 {
    X0 = 0, X1, X2, X3, X4, X5, X6, X7, X8, X9, X10, X11, X12, X13, X14, X15,
    X16, X17, X18, X19, X20, X21, X22, X23, X24, X25, X26, X27, X28, X29, X30,
    SP = 31, ZR = 31,
};

class Assembler {
public:
    Assembler(u32* buffer, u32 capacity_words) : buf_(buffer), cap_(capacity_words) {}

    bool ok() const { return ok_; }
    u32 size() const { return pos_; }       // words written
    u32* start() const { return buf_; }
    u32 word(u32 i) const { return buf_[i]; }

    // A forward branch target: branches to it are patched by bind().
    struct Label {
        u32 pos = 0;
        bool bound = false;
        u32 pending[128];
        u32 npending = 0;
    };
    void bind(Label& l);

    // --- moves
    void movz(bool x, Reg rd, u32 imm16, u32 shift);   // shift: 0, 16 (, 32, 48 for x)
    void movk(bool x, Reg rd, u32 imm16, u32 shift);
    void movn(bool x, Reg rd, u32 imm16, u32 shift);
    void mov_imm32(Reg wd, u32 value);                 // shortest movz/movn/movk sequence
    void mov_imm64(Reg xd, u64 value);
    void mov(bool x, Reg rd, Reg rm);                  // ORR rd, zr, rm (not for SP)
    void mov_from_sp(Reg xd);                          // ADD xd, sp, #0

    // --- arithmetic and logic
    void add_imm(bool x, Reg rd, Reg rn, u32 imm12, bool lsl12 = false);
    void sub_imm(bool x, Reg rd, Reg rn, u32 imm12, bool lsl12 = false);
    // Register forms; `lsl` shifts rm left first.
    void add(bool x, Reg rd, Reg rn, Reg rm, u32 lsl = 0);
    void sub(bool x, Reg rd, Reg rn, Reg rm, u32 lsl = 0);
    void and_(bool x, Reg rd, Reg rn, Reg rm, u32 lsl = 0);
    void orr(bool x, Reg rd, Reg rn, Reg rm, u32 lsl = 0);
    void eor(bool x, Reg rd, Reg rn, Reg rm, u32 lsl = 0);
    // Compare (SUBS to the zero register): sets NZCV.
    void cmp(bool x, Reg rn, Reg rm);
    void cmp_imm(bool x, Reg rn, u32 imm12);
    // Unsigned bitfield move and its aliases.
    void ubfm(bool x, Reg rd, Reg rn, u32 immr, u32 imms);
    void lsr_imm(Reg wd, Reg wn, u32 shift) { ubfm(false, wd, wn, shift & 31, 31); }
    void ubfx(Reg wd, Reg wn, u32 lsb, u32 width) { ubfm(false, wd, wn, lsb, lsb + width - 1); }
    // Bitfield move (keeps the other bits of rd) and its alias BFI.
    void bfm(bool x, Reg rd, Reg rn, u32 immr, u32 imms);
    void bfi(Reg wd, Reg wn, u32 lsb, u32 width) { bfm(false, wd, wn, (32 - lsb) & 31, width - 1); }
    // Byte reversal (32-bit): all four bytes / within each halfword.
    void rev(Reg wd, Reg wn);
    void rev16(Reg wd, Reg wn);
    void extr(bool x, Reg rd, Reg rn, Reg rm, u32 lsb);
    void ror_imm(Reg wd, Reg wn, u32 amount) { extr(false, wd, wn, wn, amount & 31); }

    // --- loads and stores, unsigned scaled offset (offset must be a
    // multiple of the access size and below 4096 * size)
    void ldr(bool x, Reg rt, Reg rn, u32 offset);
    void str(bool x, Reg rt, Reg rn, u32 offset);
    void ldrb(Reg wt, Reg rn, u32 offset);
    void strb(Reg wt, Reg rn, u32 offset);
    // Register offset [xn, xm] (no shift); size 1, 2 or 4 bytes, W register.
    void ldr_reg(u32 size, Reg wt, Reg xn, Reg xm);
    void str_reg(u32 size, Reg wt, Reg xn, Reg xm);
    static bool fits_offset(u32 offset, u32 size) { return offset % size == 0 && offset / size < 4096; }
    // Pairs of X registers: signed offset / pre-index / post-index, in bytes
    // (a multiple of 8 in [-512, 504]).
    void stp(Reg rt, Reg rt2, Reg rn, s32 offset);
    void stp_pre(Reg rt, Reg rt2, Reg rn, s32 offset);
    void ldp(Reg rt, Reg rt2, Reg rn, s32 offset);
    void ldp_post(Reg rt, Reg rt2, Reg rn, s32 offset);

    // --- branches
    void blr(Reg rn);
    void ret();
    void cbnz(bool x, Reg rt, Label& target);
    void cbz(bool x, Reg rt, Label& target);
    void tbz(Reg rt, u32 bit, Label& target);
    void tbnz(Reg rt, u32 bit, Label& target);
    enum Cond : u32 { EQ = 0, NE = 1, HS = 2, LO = 3, MI = 4, PL = 5, VS = 6, VC = 7,
                      HI = 8, LS = 9, GE = 10, LT = 11, GT = 12, LE = 13 };
    void b_cond(Cond c, Label& target);
    // CSINC rd, rn, rm, cond; CSET rd, cond = CSINC rd, zr, zr, !cond.
    void csinc(bool x, Reg rd, Reg rn, Reg rm, Cond c);
    void cset(Reg wd, Cond c) { csinc(false, wd, ZR, ZR, static_cast<Cond>(c ^ 1)); }
    void b(Label& target);
    void nop();

    void emit(u32 insn);

private:
    void branch_to(Label& l, u32 insn_without_offset, u32 kind);
    static void patch(u32* at, u32 from, u32 to, u32 kind);

    u32* buf_;
    u32 cap_;
    u32 pos_ = 0;
    bool ok_ = true;
};

} // namespace g3::a64

#endif // G3_CORE_CPU_PPC750_JIT_A64_ASSEMBLER_H
