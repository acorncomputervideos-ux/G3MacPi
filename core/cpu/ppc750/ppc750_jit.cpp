// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/cpu/ppc750/ppc750_jit.cpp — AArch64 code for decoded blocks (JIT
// stage 2, docs/cpu/jit-a64.md).
//
// A block becomes one function void(Ppc750*). Simple integer instructions
// are done in generated code on the CpuState in memory; every other
// operation calls its block handler (the same function stage 1 runs), so
// results are those of the interpreter by construction. Generated code
// does not keep the PC and the instruction count up to date instruction by
// instruction: it writes them before each handler call (handlers, and
// whatever they call, see exact values) and at the end. Only handlers can
// raise exceptions or otherwise set exit_, so exit_ is checked after each
// handler call.
//
// Registers: x19 = the Ppc750, x20 = its CpuState, w21 = the PC at block
// entry (a block is found by physical address and may be entered at any
// effective address that maps to it). In blocks with loads or stores:
// x22 = guest RAM, x23 = the block cache's per-page code flags, x24 = the
// data TLB. w8-w15 are scratch; x16 holds call targets, x17 far addresses.
//
// Chaining: at its end a block looks up the next one itself, with the
// checks run_blocks makes between blocks (exit_, a pending interrupt with
// MSR[EE], MSR[POW], the fetch page and MSR[IR|PR] unchanged, a valid block with
// code, the run budget), and jumps into it past its prologue (word
// kChainEntry), keeping the first block's frame. Any check failing returns
// to run_blocks, which does the same thing the slow way.

#include "core/cpu/ppc750/ppc750.h"
#include "core/cpu/ppc750/jit_worker.h"
#include "core/cpu/ppc750/jit/a64_assembler.h"
#include "core/common/bitops.h"

namespace {
constexpr g3::u32 kXerMask = 0xE000007Fu;  // as ppc750_system.cpp: the bits mtspr XER keeps
}


namespace g3::ppc {

using namespace a64;

u32 Ppc750::jit_op_id(u32 insn)
{
    const u32 op = field::opcd(insn);
    u32 group;
    switch (op) {
    case 19: group = 0; break;
    case 31: group = 1; break;
    case 59: group = 2; break;
    case 63: group = 3; break;
    default: return op;
    }
    u32 xo = field::xo10(insn);
    if ((op == 59 || op == 63) && (xo & 0x10))
        xo &= 0x1F;  // A-form: the extended opcode is bits 26-30 only
    return 64 + group * 1024 + xo;
}

bool Ppc750::set_jit(u32* code, u32 words, JitRunner runner, void* context)
{
    jit_code_ = nullptr;
#if !defined(__aarch64__)
    if (!runner)
        return false;  // generated code can only run natively on AArch64
#endif
    if (!cache_ || !code || words < 1024)
        return false;
    jit_code_ = code;
    jit_words_ = words;
    jit_used_ = 0;
    jit_epoch_ = cache_->stats().flushes;
    jit_runner_ = runner;
    jit_ctx_ = context;
    cache_->flush();  // existing blocks have no code; start clean
    return true;
}

struct JitCompiler {
    Ppc750& c;
    Assembler& a;
    u32 cnt_off, exit_off;

    static constexpr u32 gpr_off(u32 r) { return static_cast<u32>(__builtin_offsetof(CpuState, gpr)) + 4 * r; }
    static constexpr u32 pc_off() { return static_cast<u32>(__builtin_offsetof(CpuState, pc)); }
    static constexpr u32 spr_off(u32 n) { return static_cast<u32>(__builtin_offsetof(CpuState, spr)) + 4 * n; }
    static constexpr u32 msr_off() { return static_cast<u32>(__builtin_offsetof(CpuState, msr)); }
    static constexpr u32 tlb_tag_off = __builtin_offsetof(Ppc750::TlbEntry, tag);
    static constexpr u32 tlb_pa_off = __builtin_offsetof(Ppc750::TlbEntry, pa_page);
    static constexpr u32 tlb_writable_off = __builtin_offsetof(Ppc750::TlbEntry, writable);
    static constexpr u32 tlb_changed_off = __builtin_offsetof(Ppc750::TlbEntry, changed);
    static constexpr u32 tlb_valid = Ppc750::kTlbValid;
    static_assert(sizeof(Ppc750::TlbEntry) == 12, "the generated TLB lookup assumes 12-byte entries");
    static_assert(Ppc750::kTlbSize == 256, "the generated TLB lookup assumes 256 entries");
    u32 ram_off, ram_size_off, dtlb_off, tlb_on_off, slow_off;
    Ppc750::JitStats* st;  // compile-time counts (the worker keeps its own)
    // For chaining (set by jit_generate).
    u32 profile_off = 0, break_off = 0, checkstop_off = 0, ext_off = 0, dec_off = 0, fetch_host_off = 0, fetch_page_off = 0,
        fetch_msr_off = 0, fetch_pa_off = 0, limit_off = 0, chained_off = 0;
    BlockCache::Layout layout{};
    const u32* code_base = nullptr;
    // The prologue is padded to this many words; chained jumps enter here.
    static constexpr u32 kChainEntry = 11;

    // Base + offset into x17 when the offset does not fit the instruction.
    Reg far(Reg base, u32& off, u32 size)
    {
        if (Assembler::fits_offset(off, size))
            return base;
        a.mov_imm64(X17, off);
        a.add(true, X17, base, X17);
        off = 0;
        return X17;
    }
    void load_gpr(Reg w, u32 r) { a.ldr(false, w, X20, gpr_off(r)); }
    void store_gpr(Reg w, u32 r) { a.str(false, w, X20, gpr_off(r)); }
    // w = (rA|0)
    void load_gpr_or_zero(Reg w, u32 r)
    {
        if (r)
            load_gpr(w, r);
        else
            a.movz(false, w, 0, 0);
    }
    // wd = wn + imm (32-bit wrap-around)
    void add_const(Reg wd, Reg wn, u32 imm)
    {
        if (imm < 4096) {
            a.add_imm(false, wd, wn, imm);
        } else if (-imm < 4096) {
            a.sub_imm(false, wd, wn, -imm);
        } else {
            a.mov_imm32(X12, imm);
            a.add(false, wd, wn, X12);
        }
    }

    // Writes the PC (entry + 4 * index) and adds `pending` to the count.
    void sync(u32 index, u32& pending)
    {
        if (!pending)
            return;
        add_const(X8, X21, 4 * index);
        a.str(false, X8, X20, pc_off());
        u32 off = cnt_off;
        const Reg base = far(X19, off, 8);
        a.ldr(true, X9, base, off);
        a.add_imm(true, X9, X9, pending);
        a.str(true, X9, base, off);
        pending = 0;
    }

    static constexpr u32 cr_off() { return static_cast<u32>(__builtin_offsetof(CpuState, cr)); }

    // CR field `index` from the flags of the last compare (signed or
    // unsigned), plus XER[SO]: op_compare / update_cr0.
    void set_cr_field_from_flags(u32 index, bool is_signed)
    {
        a.cset(X10, is_signed ? Assembler::LT : Assembler::LO);
        a.cset(X11, is_signed ? Assembler::GT : Assembler::HI);
        a.cset(X12, Assembler::EQ);
        a.ldr(false, X13, X20, spr_off(spr::XER));
        a.lsr_imm(X13, X13, 31);                     // SO
        a.orr(false, X13, X13, X12, 1);
        a.orr(false, X13, X13, X11, 2);
        a.orr(false, X13, X13, X10, 3);
        a.ldr(false, X14, X20, cr_off());
        a.bfi(X14, X13, 28 - 4 * index, 4);
        a.str(false, X14, X20, cr_off());
    }
    // update_cr0 for the result in w8.
    void cr0_from_w8()
    {
        a.cmp_imm(false, X8, 0);
        set_cr_field_from_flags(0, true);
    }
    void compare(u32 insn, Reg wb_value, bool is_signed)
    {
        load_gpr(X8, field::ra(insn));
        a.cmp(false, X8, wb_value);
        set_cr_field_from_flags(field::crfd(insn), is_signed);
    }

    // The XO-form add/subtract family, as op_add_family: result =
    // x + y + carry-in in 64 bits (rD = the low word); CA from bit 32
    // (except add, subf, neg); with OE, OV = ((x ^ r) & (y ^ r)) bit 31
    // and SO |= OV; with Rc, CR0 from the result and the new SO. Matched
    // on the 9-bit extended opcode, like execute_op31. None can raise an
    // exception.
    bool add_family(u32 insn)
    {
        enum In : u8 { RA, NOT_RA, RB, ZERO, ONES, CA, ONE };
        In x, y, cin;
        bool writes_ca = true;
        switch (field::xo9(insn)) {
        case 266: x = RA; y = RB; cin = ZERO; writes_ca = false; break;      // add
        case 10:  x = RA; y = RB; cin = ZERO; break;                         // addc
        case 138: x = RA; y = RB; cin = CA; break;                           // adde
        case 234: x = RA; y = ONES; cin = CA; break;                         // addme
        case 202: x = RA; y = ZERO; cin = CA; break;                         // addze
        case 40:  x = NOT_RA; y = RB; cin = ONE; writes_ca = false; break;   // subf
        case 8:   x = NOT_RA; y = RB; cin = ONE; break;                      // subfc
        case 136: x = NOT_RA; y = RB; cin = CA; break;                       // subfe
        case 232: x = NOT_RA; y = ONES; cin = CA; break;                     // subfme
        case 200: x = NOT_RA; y = ZERO; cin = CA; break;                     // subfze
        case 104: x = NOT_RA; y = ZERO; cin = ONE; writes_ca = false; break; // neg
        default: return false;
        }
        const bool oe = field::oe(insn);
        const u32 ra = field::ra(insn), rb = field::rb(insn), rd = field::rd(insn);
        const bool need_xer = cin == CA || writes_ca || oe;
        if (need_xer)
            a.ldr(false, X13, X20, spr_off(spr::XER));   // w13 = XER
        auto operand = [&](Reg w, In what) {
            switch (what) {
            case RA: load_gpr(w, ra); break;
            case NOT_RA:
                load_gpr(w, ra);
                a.mov_imm32(X12, 0xFFFFFFFFu);
                a.eor(false, w, w, X12);
                break;
            case RB: load_gpr(w, rb); break;
            case ZERO: a.movz(false, w, 0, 0); break;
            case ONES: a.mov_imm32(w, 0xFFFFFFFFu); break;
            default: break;
            }
        };
        operand(X8, x);                                   // w8 = x (zero-extended in x8)
        operand(X9, y);                                   // w9 = y
        a.add(true, X11, X8, X9);                         // x11 = x + y (64-bit)
        if (cin == ONE) {
            a.add_imm(true, X11, X11, 1);
        } else if (cin == CA) {
            a.ubfx(X10, X13, 29, 1);                      // XER[CA]
            a.add(true, X11, X11, X10);
        }
        store_gpr(X11, rd);                               // the low word
        if (writes_ca) {
            a.ubfx64(X12, X11, 32, 1);                    // carry out
            a.bfi(X13, X12, 29, 1);                       // XER[CA]
        }
        if (oe) {
            a.eor(false, X14, X8, X11);
            a.eor(false, X15, X9, X11);
            a.and_(false, X14, X14, X15);
            a.lsr_imm(X14, X14, 31);                      // overflow
            a.bfi(X13, X14, 30, 1);                       // XER[OV] = it
            a.orr(false, X13, X13, X14, 31);              // XER[SO] |= it
        }
        if (writes_ca || oe)
            a.str(false, X13, X20, spr_off(spr::XER));
        if (field::rc(insn)) {
            a.mov(false, X8, X11);
            cr0_from_w8();                                // reads the new XER[SO]
        }
        return true;
    }

    // Generated code for `insn` if it is one of the simple instructions
    // (no exception possible; CR updates only as compares and Rc do them,
    // no XER changes); false otherwise.
    bool native(u32 insn)
    {
        const u32 rd = field::rd(insn), ra = field::ra(insn), rb = field::rb(insn);
        switch (field::opcd(insn)) {
        case 10:  // cmpli
            a.mov_imm32(X9, field::uimm(insn));
            compare(insn, X9, false);
            return true;
        case 11:  // cmpi
            a.mov_imm32(X9, field::simm(insn));
            compare(insn, X9, true);
            return true;
        case 28: case 29: {  // andi. andis.
            const u32 imm = field::opcd(insn) == 29 ? field::uimm(insn) << 16 : field::uimm(insn);
            load_gpr(X8, rd);
            a.mov_imm32(X9, imm);
            a.and_(false, X8, X8, X9);
            store_gpr(X8, ra);
            cr0_from_w8();
            return true;
        }
        case 14:  // addi
        case 15:  // addis
            load_gpr_or_zero(X8, ra);
            add_const(X8, X8, field::opcd(insn) == 14 ? field::simm(insn) : field::simm(insn) << 16);
            store_gpr(X8, rd);
            return true;
        case 24: case 25: case 26: case 27: {  // ori oris xori xoris: rA = rS op imm
            const u32 op = field::opcd(insn);
            const u32 imm = (op & 1) ? field::uimm(insn) << 16 : field::uimm(insn);
            load_gpr(X8, rd);
            if (imm) {
                a.mov_imm32(X9, imm);
                if (op <= 25)
                    a.orr(false, X8, X8, X9);
                else
                    a.eor(false, X8, X8, X9);
            }
            store_gpr(X8, ra);
            return true;
        }
        case 20: {  // rlwimi(.): rA = (rotl(rS, SH) & m) | (rA & ~m)
            const u32 mask = ppc_mask(field::mb(insn), field::me(insn));
            load_gpr(X8, rd);
            if (rb & 31)
                a.ror_imm(X8, X8, (32 - rb) & 31);
            a.mov_imm32(X9, mask);
            a.and_(false, X8, X8, X9);
            load_gpr(X10, ra);
            a.mov_imm32(X9, ~mask);
            a.and_(false, X10, X10, X9);
            a.orr(false, X8, X8, X10);
            store_gpr(X8, ra);
            if (field::rc(insn))
                cr0_from_w8();
            return true;
        }
        case 21: {  // rlwinm(.)
            const u32 mask = ppc_mask(field::mb(insn), field::me(insn));
            load_gpr(X8, rd);
            if (rb & 31)
                a.ror_imm(X8, X8, (32 - rb) & 31);  // rotate left by SH
            if (mask != 0xFFFFFFFFu) {
                a.mov_imm32(X9, mask);
                a.and_(false, X8, X8, X9);
            }
            store_gpr(X8, ra);
            if (field::rc(insn))
                cr0_from_w8();
            return true;
        }
        case 31: {
            if (add_family(insn))
                return true;
            switch (field::xo10(insn)) {
            case 467: {  // mtspr LR / CTR / XER (user-writable: no exception)
                const u32 n = ((insn >> 16) & 31) | ((insn >> 6) & 0x3E0);
                if (field::rc(insn) || (n != spr::LR && n != spr::CTR && n != spr::XER))
                    return false;
                load_gpr(X8, rd);
                if (n == spr::XER) {
                    a.mov_imm32(X9, kXerMask);
                    a.and_(false, X8, X8, X9);
                }
                a.str(false, X8, X20, spr_off(n));
                return true;
            }
            case 0:   // cmp
            case 32:  // cmpl
                load_gpr(X9, rb);
                compare(insn, X9, field::xo10(insn) == 0);
                return true;
            default:
                break;
            }
            switch (field::xo10(insn)) {
            case 28: case 444: case 316:  // and or xor: rA = rS op rB
                load_gpr(X8, rd);
                load_gpr(X9, rb);
                if (field::xo10(insn) == 28)
                    a.and_(false, X8, X8, X9);
                else if (field::xo10(insn) == 444)
                    a.orr(false, X8, X8, X9);
                else
                    a.eor(false, X8, X8, X9);
                store_gpr(X8, ra);
                if (field::rc(insn))
                    cr0_from_w8();
                return true;
            case 339: {  // mfspr LR / CTR (user-readable: no exception)
                const u32 n = ((insn >> 16) & 31) | ((insn >> 6) & 0x3E0);
                if (field::rc(insn) || (n != spr::LR && n != spr::CTR))
                    return false;
                a.ldr(false, X8, X20, spr_off(n));
                store_gpr(X8, rd);
                return true;
            }
            default:
                return false;
            }
        }
        default:
            return false;
        }
    }

    // --- branches (always a block's last operation) -------------------------------
    // As op_b / op_bc / op_bclr_bcctr: CTR decrement and test, CR bit test,
    // the target read before LK updates LR. Then PC and the whole pending
    // count are written; the block ends.
    static bool is_branch(u32 insn)
    {
        const u32 op = field::opcd(insn);
        return op == 16 || op == 18 || (op == 19 && (field::xo10(insn) == 16 || field::xo10(insn) == 528));
    }
    void branch(u32 i, u32 insn, u32& pending)
    {
        ++pending;  // the branch itself
        const u32 here = 4 * i;  // offset of the branch from the entry PC
        const u32 op = field::opcd(insn), bo = field::rd(insn), bi = field::ra(insn);
        const bool lk = field::lk(insn);
        auto set_lr = [&] {
            add_const(X9, X21, here + 4);
            a.str(false, X9, X20, spr_off(spr::LR));
        };
        if (op == 18) {  // b
            const u32 li = static_cast<u32>(sign_extend(insn & 0x03FFFFFCu, 26));
            if (field::aa(insn))
                a.mov_imm32(X8, li);
            else
                add_const(X8, X21, here + li);
            if (lk)
                set_lr();
        } else {
            const bool to_ctr = op == 19 && field::xo10(insn) == 528;
            if (op == 19) {  // target first: bclrl uses the old LR
                a.ldr(false, X11, X20, spr_off(to_ctr ? spr::CTR : spr::LR));
                a.mov_imm32(X12, ~3u);
                a.and_(false, X11, X11, X12);
            }
            if (lk)
                set_lr();
            Assembler::Label not_taken;
            add_const(X8, X21, here + 4);  // not taken: the next instruction
            if (!to_ctr && !(bo & 0x04)) {  // decrement CTR, test it
                a.ldr(false, X9, X20, spr_off(spr::CTR));
                a.sub_imm(false, X9, X9, 1);
                a.str(false, X9, X20, spr_off(spr::CTR));
                if (bo & 0x02)
                    a.cbnz(false, X9, not_taken);  // branch if CTR == 0
                else
                    a.cbz(false, X9, not_taken);
            }
            if (!(bo & 0x10)) {  // test CR bit bi
                a.ldr(false, X10, X20, cr_off());
                if (bo & 0x08)
                    a.tbz(X10, 31 - bi, not_taken);
                else
                    a.tbnz(X10, 31 - bi, not_taken);
            }
            if (op == 19) {
                a.mov(false, X8, X11);
            } else {
                const u32 bd = static_cast<u32>(sign_extend(insn & 0xFFFCu, 16));
                if (field::aa(insn))
                    a.mov_imm32(X8, bd);
                else
                    add_const(X8, X21, here + bd);
            }
            a.bind(not_taken);
        }
        a.str(false, X8, X20, pc_off());
        u32 off = cnt_off;
        const Reg base = far(X19, off, 8);
        a.ldr(true, X9, base, off);
        a.add_imm(true, X9, X9, pending);
        a.str(true, X9, base, off);
        pending = 0;
    }

    void call_handler(u32 i, u32 count, const BlockOp& op, Assembler::Label& out)
    {
        {   // the profile: ++jit_profile_[id]; one instruction word of the kind kept
            const u32 id = Ppc750::jit_op_id(op.insn);
            __atomic_store_n(&c.jit_sample_[id], op.insn, __ATOMIC_RELAXED);
            u32 off = profile_off + 8 * id;
            const Reg base = far(X19, off, 8);
            a.ldr(true, X9, base, off);
            a.add_imm(true, X9, X9, 1);
            a.str(true, X9, base, off);
        }
        a.mov(true, X0, X19);
        a.mov_imm32(X1, op.insn);
        a.mov_imm64(X16, reinterpret_cast<u64>(op.fn));
        a.blr(X16);
        if (i + 1 < count) {
            u32 off = exit_off;
            const Reg base = far(X19, off, 1);
            a.ldrb(X8, base, off);
            a.cbnz(false, X8, out);
        }
    }

    // --- D-form integer loads and stores ------------------------------------------
    // The fast path is mem_fast's: the access stays in its page; real mode
    // or a data-TLB hit (stores: writable, C bit set); inside the RAM
    // window; stores: not to a page holding blocks. Then the access itself,
    // big-endian. Anything else runs the instruction's handler, which does
    // it all over (nothing has been changed before the decision).
    struct MemOp {
        u32 size;
        bool store, update;
        bool sign = false;  // lha, lhau: sign-extend the halfword
    };
    static bool memory_op(u32 insn, MemOp& m)
    {
        switch (field::opcd(insn)) {
        case 32: m = {4, false, false}; return true;  // lwz
        case 33: m = {4, false, true}; return true;   // lwzu
        case 34: m = {1, false, false}; return true;  // lbz
        case 35: m = {1, false, true}; return true;   // lbzu
        case 36: m = {4, true, false}; return true;   // stw
        case 37: m = {4, true, true}; return true;    // stwu
        case 38: m = {1, true, false}; return true;   // stb
        case 39: m = {1, true, true}; return true;    // stbu
        case 40: m = {2, false, false}; return true;  // lhz
        case 41: m = {2, false, true}; return true;   // lhzu
        case 42: m = {2, false, false, true}; return true;  // lha
        case 43: m = {2, false, true, true}; return true;   // lhau
        case 44: m = {2, true, false}; return true;   // sth
        case 45: m = {2, true, true}; return true;    // sthu
        default: return false;
        }
    }
    static bool has_memory_ops(const BlockCache::Block& b)
    {
        MemOp m;
        for (u32 i = 0; i < b.count; ++i)
            if (memory_op(b.ops[i].insn, m))
                return true;
        return false;
    }
    static constexpr u32 bit_of(u32 mask) { return static_cast<u32>(__builtin_ctz(mask)); }

    // PC and count are already written (a memory op is a sync point).
    void memory(u32 i, u32 count, const BlockOp& op, const MemOp& m, Assembler::Label& out)
    {
        const u32 insn = op.insn, rd = field::rd(insn), ra = field::ra(insn);
        Assembler::Label slow, have_pa, done;
        // w8 = EA
        load_gpr_or_zero(X8, ra);
        add_const(X8, X8, field::simm(insn));
        if (m.size > 1) {
            a.ubfx(X9, X8, 0, 12);
            a.cmp_imm(false, X9, 0x1000 - m.size);
            a.b_cond(Assembler::HI, slow);  // crosses into the next page
        }
        // w9 = PA
        a.ldr(false, X10, X20, msr_off());
        a.mov(false, X9, X8);
        a.tbz(X10, bit_of(msr::DR), have_pa);
        {
            u32 off = tlb_on_off;
            const Reg base = far(X19, off, 1);
            a.ldrb(X11, base, off);
            a.cbz(false, X11, slow);
        }
        a.lsr_imm(X11, X8, 12);                         // page
        a.ubfx(X12, X11, 0, 8);                         // index (256 entries)
        a.add(true, X12, X12, X12, 1);                  // * 3
        a.add(true, X12, X24, X12, 2);                  // * 4: the entry (12 bytes)
        a.ldr(false, X13, X12, tlb_tag_off);
        a.ubfx(X14, X10, bit_of(msr::PR), 1);
        a.orr(false, X14, X14, X11, 1);
        a.mov_imm32(X15, tlb_valid);
        a.orr(false, X14, X14, X15);                    // the tag a hit has
        a.cmp(false, X13, X14);
        a.b_cond(Assembler::NE, slow);
        if (m.store) {
            a.ldrb(X13, X12, tlb_writable_off);
            a.cbz(false, X13, slow);
            a.ldrb(X13, X12, tlb_changed_off);
            a.cbz(false, X13, slow);
        }
        a.ldr(false, X13, X12, tlb_pa_off);
        a.ubfx(X9, X8, 0, 12);
        a.orr(false, X9, X13, X9);
        a.bind(have_pa);
        {   // inside the RAM window (which starts at 0): PA <= size - access
            u32 off = ram_size_off;
            const Reg base = far(X19, off, 4);
            a.ldr(false, X13, base, off);
        }
        a.sub_imm(false, X13, X13, m.size);
        a.cmp(false, X9, X13);
        a.b_cond(Assembler::HI, slow);
        if (m.store) {
            a.lsr_imm(X13, X9, 12);
            a.ldr_reg(1, X13, X23, X13);
            a.cbnz(false, X13, slow);                   // page holds blocks: note_write's business
            load_gpr(X10, rd);
            if (m.size == 4) a.rev(X10, X10);
            else if (m.size == 2) a.rev16(X10, X10);
            a.str_reg(m.size, X10, X22, X9);
        } else {
            a.ldr_reg(m.size, X10, X22, X9);
            if (m.size == 4) a.rev(X10, X10);
            else if (m.size == 2) a.rev16(X10, X10);
            if (m.sign)
                a.sxth(X10, X10);
            store_gpr(X10, rd);
        }
        if (m.update)
            store_gpr(X8, ra);
        u32 one = 1;
        sync(i + 1, one);
        a.b(done);
        a.bind(slow);
        {
            u32 off = slow_off;
            const Reg base = far(X19, off, 8);
            a.ldr(true, X13, base, off);
            a.add_imm(true, X13, X13, 1);
            a.str(true, X13, base, off);
        }
        call_handler(i, count, op, out);
        a.bind(done);
    }

    // --- chaining: the checks run_blocks makes before a block, then a jump
    // into the next block's code. Anything not as run_blocks would find it
    // goes to `out` (return to run_blocks).
    void load_field(bool x, Reg r, u32 off, u32 size)
    {
        const Reg base = far(X19, off, size);
        if (size == 1)
            a.ldrb(r, base, off);
        else
            a.ldr(x, r, base, off);
    }
    void chain(Assembler::Label& out)
    {
        Assembler::Label no_irq;
        // exit_ (an exception, break_run(), blocks dropped ...); and, as
        // run_blocks' loop condition, break_run_ (mtspr DEC sets it alone)
        // and checkstop_.
        load_field(false, X8, exit_off, 1);
        a.cbnz(false, X8, out);
        load_field(false, X8, break_off, 1);
        a.cbnz(false, X8, out);
        load_field(false, X8, checkstop_off, 1);
        a.cbnz(false, X8, out);
        // A pending interrupt that MSR[EE] lets in: run_blocks takes it.
        a.ldr(false, X9, X20, msr_off());
        a.tbz(X9, bit_of(msr::EE), no_irq);
        load_field(false, X10, ext_off, 1);
        load_field(false, X11, dec_off, 1);
        a.orr(false, X10, X10, X11);
        a.cbnz(false, X10, out);
        a.bind(no_irq);
        // MSR[POW]: perhaps dozing (Ppc750::dozing); run_blocks decides.
        a.tbnz(X9, bit_of(msr::POW), out);
        // The fetch page: still valid, same effective page, same MSR[IR|PR].
        a.ldr(false, X8, X20, pc_off());              // w8 = the next PC
        load_field(true, X10, fetch_host_off, 8);
        a.cbz(true, X10, out);
        load_field(false, X10, fetch_page_off, 4);
        a.lsr_imm(X10, X10, 12);
        a.lsr_imm(X11, X8, 12);
        a.cmp(false, X10, X11);
        a.b_cond(Assembler::NE, out);
        a.mov_imm32(X12, msr::IR | msr::PR);
        a.and_(false, X11, X9, X12);
        load_field(false, X10, fetch_msr_off, 4);
        a.cmp(false, X10, X11);
        a.b_cond(Assembler::NE, out);
        // w10 = the physical address; its page must be one the cache covers.
        load_field(false, X10, fetch_pa_off, 4);
        a.ubfx(X11, X8, 0, 12);
        a.orr(false, X10, X10, X11);
        a.lsr_imm(X15, X10, 12);                      // w15 = page
        a.mov_imm32(X12, layout.pages);
        a.cmp(false, X15, X12);
        a.b_cond(Assembler::HS, out);
        // The table slot: pool offset / kAlign + 1, 0 = empty.
        a.ubfx(X11, X10, 2, 15);
        static_assert(BlockCache::kTableSize == 1u << 15, "the generated lookup assumes 2^15 slots");
        a.mov_imm64(X12, reinterpret_cast<u64>(layout.table));
        a.add(true, X12, X12, X11, 2);
        a.ldr(false, X13, X12, 0);
        a.cbz(false, X13, out);
        a.sub_imm(false, X13, X13, 1);
        a.mov_imm64(X14, reinterpret_cast<u64>(layout.pool));
        a.add(true, X14, X14, X13, 4);                // x14 = the block
        static_assert(BlockCache::kAlign == 16, "the generated lookup assumes 16-byte block alignment");
        a.ldr(false, X13, X14, blk_pa);
        a.cmp(false, X13, X10);
        a.b_cond(Assembler::NE, out);
        a.mov_imm64(X12, reinterpret_cast<u64>(layout.gen));
        a.add(true, X12, X12, X15, 2);
        a.ldr(false, X12, X12, 0);
        a.ldr(false, X13, X14, blk_gen);
        a.cmp(false, X12, X13);
        a.b_cond(Assembler::NE, out);                 // built under an older generation: dropped
        a.ldr(false, X15, X14, blk_code);
        a.cbz(false, X15, out);                       // no code (yet)
        // The budget: instr_count_ + count <= jit_limit_.
        a.ldr(false, X13, X14, blk_count);
        load_field(true, X11, cnt_off, 8);
        a.add(true, X11, X11, X13);
        load_field(true, X12, limit_off, 8);
        a.cmp(true, X11, X12);
        a.b_cond(Assembler::HI, out);
        {
            u32 off = chained_off;
            const Reg base = far(X19, off, 8);
            a.ldr(true, X13, base, off);
            a.add_imm(true, X13, X13, 1);
            a.str(true, X13, base, off);
        }
        // Into its code, past the prologue: code_base + 4 * (code - 1 + kChainEntry).
        a.add_imm(false, X15, X15, kChainEntry - 1);
        a.mov_imm64(X12, reinterpret_cast<u64>(code_base));
        a.add(true, X12, X12, X15, 2);
        a.br(X12);
    }
    static constexpr u32 blk_pa = __builtin_offsetof(BlockCache::Block, pa);
    static constexpr u32 blk_gen = __builtin_offsetof(BlockCache::Block, gen);
    static constexpr u32 blk_count = __builtin_offsetof(BlockCache::Block, count);
    static constexpr u32 blk_code = __builtin_offsetof(BlockCache::Block, code);

    bool compile(BlockCache::Block& b)
    {
        Assembler::Label out;
        a.stp_pre(X29, X30, SP, -64);
        a.mov_from_sp(X29);
        a.stp(X19, X20, SP, 16);
        a.stp(X21, X22, SP, 32);
        a.stp(X23, X24, SP, 48);
        a.mov(true, X19, X0);
        const u32 s_off = static_cast<u32>(reinterpret_cast<const u8*>(&c.s_) - reinterpret_cast<const u8*>(&c));
        if (s_off < 4096) {
            a.add_imm(true, X20, X19, s_off);
        } else {
            a.mov_imm64(X20, s_off);
            a.add(true, X20, X19, X20);
        }
        if (a.size() > kChainEntry)
            return false;  // cannot happen: the prologue is at most 11 words
        while (a.ok() && a.size() < kChainEntry)  // (a full buffer stops growing)
            a.nop();
        // --- chained blocks enter here (x19, x20 set; frame pushed)
        a.ldr(false, X21, X20, pc_off());
        if (has_memory_ops(b)) {
            // x22 = RAM, x23 = the cache's per-page code flags, x24 = DTLB.
            u32 off = ram_off;
            const Reg base = far(X19, off, 8);
            a.ldr(true, X22, base, off);
            a.mov_imm64(X23, reinterpret_cast<u64>(c.cache_->code_pages()));
            a.mov_imm64(X24, dtlb_off);
            a.add(true, X24, X19, X24);
        }

        u32 pending = 0;
        for (u32 i = 0; i < b.count; ++i) {
            const BlockOp& op = b.ops[i];
            if (native(op.insn)) {
                ++pending;
                ++st->native_ops;
                continue;
            }
            if (i + 1 == b.count && is_branch(op.insn)) {
                branch(i, op.insn, pending);
                ++st->native_ops;
                continue;
            }
            sync(i, pending);
            MemOp m;
            if (memory_op(op.insn, m)) {
                memory(i, b.count, op, m, out);
                ++st->native_ops;
                continue;
            }
            call_handler(i, b.count, op, out);
            ++st->helper_ops;
        }
        sync(b.count, pending);
        chain(out);
        a.bind(out);
        a.ldp(X23, X24, SP, 48);
        a.ldp(X21, X22, SP, 32);
        a.ldp(X19, X20, SP, 16);
        a.ldp_post(X29, X30, SP, 64);
        a.ret();
        return a.ok();
    }
};

namespace {
// Makes freshly written code visible to instruction fetch (Arm ARM
// B2.4.4: clean the data cache to the point of unification, invalidate the
// instruction cache, then synchronise).
void sync_icache(const void* begin, const void* end)
{
#if defined(__aarch64__)
    u64 ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const u64 dline = 4u << ((ctr >> 16) & 15), iline = 4u << (ctr & 15);
    const u64 b = reinterpret_cast<u64>(begin), e = reinterpret_cast<u64>(end);
    for (u64 p = b & ~(dline - 1); p < e; p += dline)
        asm volatile("dc cvau, %0" ::"r"(p) : "memory");
    asm volatile("dsb ish" ::: "memory");
    for (u64 p = b & ~(iline - 1); p < e; p += iline)
        asm volatile("ic ivau, %0" ::"r"(p) : "memory");
    asm volatile("dsb ish\n\tisb" ::: "memory");
#else
    (void)begin;
    (void)end;
#endif
}
} // namespace

bool Ppc750::jit_generate(BlockCache::Block& b, u32* at, u32 room, u32& size, JitStats& st)
{
    Assembler a(at, room);
    auto off = [this](const void* p) {
        return static_cast<u32>(static_cast<const u8*>(p) - reinterpret_cast<const u8*>(this));
    };
    JitCompiler jc{*this, a, off(&instr_count_), off(&exit_), off(&ram_), off(&ram_size_), off(&dtlb_[0]),
                   off(&tlb_enabled_), off(&jit_stats_.memory_slow), &st};
    jc.profile_off = off(&jit_profile_[0]);
    jc.break_off = off(&break_run_);
    jc.checkstop_off = off(&checkstop_);
    jc.ext_off = off(&external_pending_);
    jc.dec_off = off(&dec_pending_);
    jc.fetch_host_off = off(&fetch_host_);
    jc.fetch_page_off = off(&fetch_page_);
    jc.fetch_msr_off = off(&fetch_msr_);
    jc.fetch_pa_off = off(&fetch_pa_page_);
    jc.limit_off = off(&jit_limit_);
    jc.chained_off = off(&jit_stats_.chained);
    jc.layout = cache_->layout();
    jc.code_base = jit_code_;
    if (!jc.compile(b))
        return false;  // out of room
    sync_icache(at, at + a.size());
    size = a.size();
    return true;
}

bool Ppc750::jit_compile(BlockCache::Block* b)
{
    if (cache_->stats().flushes != jit_epoch_) {
        // The cache was flushed (possibly while allocating this very
        // block): no block refers to the code buffer any more.
        jit_epoch_ = cache_->stats().flushes;
        jit_used_ = 0;
        ++jit_stats_.resets;
    }
    u32 size = 0;
    if (!jit_generate(*b, jit_code_ + jit_used_, jit_words_ - jit_used_, size, jit_stats_))
        return false;  // out of room: the block runs without code
    b->code = jit_used_ + 1;
    jit_used_ += size;
    ++jit_stats_.compiled;
    return true;
}

// --- background compilation (jit_worker.h) ------------------------------------------

void Ppc750::set_jit_worker(JitWorker* w)
{
    jit_worker_ = jit_code_ ? w : nullptr;
    if (cache_)
        cache_->flush();  // a clean start: the worker owns the code buffer now
    jit_used_ = 0;
}

void Ppc750::jit_request(BlockCache::Block* b)
{
    JitRequest& r = jit_worker_->req_out_;
    r.block = b;
    r.epoch = cache_->stats().flushes;
    r.pa = b->pa;
    r.count = b->count;
    for (u32 i = 0; i < b->count; ++i)
        r.ops[i] = b->ops[i];
    if (jit_worker_->requests_.push(r)) {
        ++jit_stats_.queued;
#if defined(__aarch64__)
        asm volatile("sev" ::: "memory");  // wakes a worker core waiting in WFE
#endif
    }
    else
        ++jit_stats_.queue_full;  // the block keeps running through its handlers
}

void Ppc750::jit_collect()
{
    JitResult r;
    bool installed = false;
    while (jit_worker_->results_.pop(r)) {
        if (r.epoch != cache_->stats().flushes || r.block->pa != r.pa) {
            ++jit_stats_.stale;  // the cache was flushed since: not that block any more
            continue;
        }
        if (r.code == 0) {
            jit_full_ = true;  // flush at the next build: a new epoch empties the buffer
            continue;
        }
        r.block->code = r.code;
        jit_stats_.native_ops += r.native_ops;
        jit_stats_.helper_ops += r.helper_ops;
        ++jit_stats_.compiled;
        ++jit_stats_.installed;
        installed = true;
    }
#if defined(__aarch64__)
    if (installed)
        asm volatile("isb" ::: "memory");  // code another core wrote is about to run here
#else
    (void)installed;
#endif
}

u32 JitWorker::poll(u32 max)
{
    u32 n = 0;
    while (n < max && requests_.pop(req_)) {
        ++n;
        if (have_epoch_ && req_.epoch < epoch_) {
            ++stats_.stale;  // made before a flush the worker has already seen
            continue;
        }
        if (!have_epoch_ || req_.epoch != epoch_) {
            epoch_ = req_.epoch;  // the CPU flushed its cache: no code is in use
            have_epoch_ = true;
            used_ = 0;
            ++stats_.resets;
        }
        auto* b = reinterpret_cast<BlockCache::Block*>(block_);
        b->pa = req_.pa;
        b->gen = 0;
        b->count = req_.count;
        b->code = 0;
        for (u32 i = 0; i < req_.count; ++i)
            b->ops[i] = req_.ops[i];
        Ppc750::JitStats st;
        u32 size = 0;
        JitResult res{req_.block, req_.epoch, req_.pa, 0, 0, 0};
        if (cpu_.jit_generate(*b, cpu_.jit_code_ + used_, cpu_.jit_words_ - used_, size, st)) {
            res.code = used_ + 1;
            res.native_ops = static_cast<u32>(st.native_ops);
            res.helper_ops = static_cast<u32>(st.helper_ops);
            used_ += size;
            ++stats_.compiled;
        } else {
            ++stats_.full;
        }
        if (!results_.push(res))
            ++stats_.dropped_results;  // that block keeps running through its handlers
    }
    return n;
}
void Ppc750::jit_run(BlockCache::Block* b)
{
    void* code = jit_code_ + (b->code - 1);
    ++jit_stats_.runs;
    if (jit_runner_)
        jit_runner_(code, this, jit_ctx_);
#if defined(__aarch64__)
    else
        reinterpret_cast<void (*)(Ppc750*)>(code)(this);
#endif
}

} // namespace g3::ppc
