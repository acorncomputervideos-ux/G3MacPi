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
// Register cache: in each block the most used guest GPRs live in host
// registers that calls preserve (x25-x28; also x22-x24 in blocks without
// loads or stores), loaded at first use. Dirty ones are written back before
// anything that reads the CpuState (handler calls, a memory access's slow
// path, the block's end); after a handler call every cached register is
// reloaded at its next use (the handler may have changed it).
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
    const BlockCache::Layout lay = cache_->layout();
    jit_table_ = lay.table;
    jit_pool_ = lay.pool;
    jit_gen_ = lay.gen;
    jit_code_pages_ = cache_->code_pages();
    jit_dtlb_ = dtlb_;
    if (!jit_emit_stubs()) {
        jit_code_ = nullptr;
        return false;
    }
    jit_used_ = jit_stub_words_;
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
    u32 table_off = 0, pool_off = 0, gen_off = 0, code_off = 0, watch_off = 0, itlb_off = 0, profile_off = 0, break_off = 0, checkstop_off = 0, ext_off = 0, dec_off = 0, fetch_host_off = 0, fetch_page_off = 0,
        fetch_msr_off = 0, fetch_pa_off = 0, limit_off = 0, chained_off = 0;
    BlockCache::Layout layout{};
    const u32* code_base = nullptr;
    const u32* stub_dispatch = nullptr;  // the shared code (emit_stubs)
    const u32* stub_enter = nullptr;
    const u32* stub_epilogue = nullptr;
    const u32* stub_static = nullptr;
    u32 entry_off = 0, code_pages_off = 0, dtlb_ptr_off = 0;
    // The prologue is padded to this many words; chained jumps enter here.
    static constexpr u32 kChainEntry = 0;  // blocks have no prologue: the shared entry stub sets up the frame

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
    // --- the register cache
    static constexpr Reg kCacheRegs[7] = {X25, X26, X27, X28, X22, X23, X24};
    s8 host_[32] = {};            // host register of guest GPR r, or -1 (plan_cache)
    bool valid_[32] = {}, dirty_[32] = {};  // its value is loaded / newer than the CpuState
    bool cached(u32 r) const { return host_[r] >= 0; }
    Reg host(u32 r) const { return static_cast<Reg>(host_[r]); }

    // Which GPRs to keep in registers: the most referenced (at least twice).
    void plan_cache(const BlockCache::Block& b, bool memory)
    {
        u32 uses[32] = {};
        for (u32 i = 0; i < b.count; ++i) {
            const u32 insn = b.ops[i].insn;
            ++uses[field::rd(insn)];
            ++uses[field::ra(insn)];
            ++uses[field::rb(insn)];
        }
        for (u32 r = 0; r < 32; ++r) {
            host_[r] = -1;
            valid_[r] = dirty_[r] = false;
        }
        const u32 slots = memory ? 4 : 7;
        for (u32 k = 0; k < slots; ++k) {
            u32 best = 32;
            for (u32 r = 0; r < 32; ++r)
                if (host_[r] < 0 && uses[r] >= 2 && (best == 32 || uses[r] > uses[best]))
                    best = r;
            if (best == 32)
                break;
            host_[best] = static_cast<s8>(kCacheRegs[k]);
        }
    }
    // The register holding GPR r's value: its host register (loaded if
    // needed), or `scratch` loaded from the CpuState.
    Reg use(u32 r, Reg scratch)
    {
        if (!cached(r)) {
            a.ldr(false, scratch, X20, gpr_off(r));
            return scratch;
        }
        if (!valid_[r]) {
            a.ldr(false, host(r), X20, gpr_off(r));
            valid_[r] = true;
        }
        return host(r);
    }
    // Where to put GPR r's new value: its host register, or `scratch`;
    // then defined(r, that register).
    Reg def(u32 r, Reg scratch) const { return cached(r) ? host(r) : scratch; }
    void defined(u32 r, Reg w)
    {
        if (!cached(r)) {
            a.str(false, w, X20, gpr_off(r));
            return;
        }
        if (w != host(r))
            a.mov(false, host(r), w);
        valid_[r] = dirty_[r] = true;
    }
    // Before code that reads the CpuState: write back what changed.
    void flush()
    {
        for (u32 r = 0; r < 32; ++r)
            if (cached(r) && dirty_[r]) {
                a.str(false, host(r), X20, gpr_off(r));
                dirty_[r] = false;
            }
    }
    // After code that may have changed the CpuState's GPRs.
    void forget()
    {
        for (u32 r = 0; r < 32; ++r)
            valid_[r] = dirty_[r] = false;
    }
    void load_gpr(Reg w, u32 r)
    {
        const Reg v = use(r, w);
        if (v != w)
            a.mov(false, w, v);
    }
    void store_gpr(Reg w, u32 r) { defined(r, w); }
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
    // update_cr0 for the result in w.
    void cr0_from(Reg w)
    {
        a.cmp_imm(false, w, 0);
        set_cr_field_from_flags(0, true);
    }
    void cr0_from_w8() { cr0_from(X8); }
    void compare(u32 insn, Reg wb_value, bool is_signed)
    {
        const Reg va = use(field::ra(insn), X8);
        a.cmp(false, va, wb_value);
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
            const Reg vs = use(rd, X8);
            a.mov_imm32(X9, imm);
            const Reg d = def(ra, X8);
            a.and_(false, d, vs, X9);
            defined(ra, d);
            cr0_from(d);
            return true;
        }
        case 14:  // addi
        case 15: {  // addis
            Reg va = X8;
            if (ra)
                va = use(ra, X8);
            else
                a.movz(false, X8, 0, 0);
            const Reg d = def(rd, X8);
            add_const(d, va, field::opcd(insn) == 14 ? field::simm(insn) : field::simm(insn) << 16);
            defined(rd, d);
            return true;
        }
        case 24: case 25: case 26: case 27: {  // ori oris xori xoris: rA = rS op imm
            const u32 op = field::opcd(insn);
            const u32 imm = (op & 1) ? field::uimm(insn) << 16 : field::uimm(insn);
            const Reg vs = use(rd, X8);
            const Reg d = def(ra, X8);
            if (imm) {
                a.mov_imm32(X9, imm);
                if (op <= 25)
                    a.orr(false, d, vs, X9);
                else
                    a.eor(false, d, vs, X9);
            } else if (d != vs) {
                a.mov(false, d, vs);
            }
            defined(ra, d);
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
            Reg v = use(rd, X8);
            const Reg d = def(ra, X8);
            if (mask != 0xFFFFFFFFu) {
                if (rb & 31) {
                    a.ror_imm(X8, v, (32 - rb) & 31);  // rotate left by SH
                    v = X8;
                }
                a.mov_imm32(X9, mask);
                a.and_(false, d, v, X9);
            } else if (rb & 31) {
                a.ror_imm(d, v, (32 - rb) & 31);
            } else if (d != v) {
                a.mov(false, d, v);
            }
            defined(ra, d);
            if (field::rc(insn))
                cr0_from(d);
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
                compare(insn, use(rb, X9), field::xo10(insn) == 0);
                return true;
            default:
                break;
            }
            switch (field::xo10(insn)) {
            case 28: case 444: case 316: {  // and or xor: rA = rS op rB
                const Reg vs = use(rd, X8);
                const Reg vb = use(rb, X9);
                const Reg d = def(ra, X8);
                if (field::xo10(insn) == 28)
                    a.and_(false, d, vs, vb);
                else if (field::xo10(insn) == 444)
                    a.orr(false, d, vs, vb);
                else
                    a.eor(false, d, vs, vb);
                defined(ra, d);
                if (field::rc(insn))
                    cr0_from(d);
                return true;
            }
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
        // The register cache as the slow path finds it: what is dirty now
        // (the fast path changes no guest register before its last check).
        bool dirty0[32];
        for (u32 r = 0; r < 32; ++r)
            dirty0[r] = dirty_[r];
        // w8 = EA
        Reg base_reg = X8;
        if (ra)
            base_reg = use(ra, X8);
        else
            a.movz(false, X8, 0, 0);
        add_const(X8, base_reg, field::simm(insn));
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
            Reg v = use(rd, X10);
            if (m.size == 4) { a.rev(X10, v); v = X10; }
            else if (m.size == 2) { a.rev16(X10, v); v = X10; }
            a.str_reg(m.size, v, X22, X9);
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
        bool valid1[32], dirty1[32];
        for (u32 r = 0; r < 32; ++r) {
            valid1[r] = valid_[r];
            dirty1[r] = dirty_[r];
        }
        a.b(done);
        a.bind(slow);
        {
            u32 off = slow_off;
            const Reg base = far(X19, off, 8);
            a.ldr(true, X13, base, off);
            a.add_imm(true, X13, X13, 1);
            a.str(true, X13, base, off);
        }
        // The handler does the whole instruction from the CpuState: write
        // back first, then reload what the fast path leaves in registers.
        for (u32 r = 0; r < 32; ++r)
            dirty_[r] = dirty0[r];
        flush();
        call_handler(i, count, op, out);
        for (u32 r = 0; r < 32; ++r) {
            if (cached(r) && valid1[r])
                a.ldr(false, host(r), X20, gpr_off(r));
            valid_[r] = valid1[r];
            dirty_[r] = dirty1[r];  // (at most stores one value again)
        }
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
    // A jump target known when the block is compiled: in the same page,
    // at `delta` bytes from the block's entry, physical address `pa`.
    struct Static {
        s32 delta;
        u32 pa;
    };
    void chain(Assembler::Label& out, const Static* statics, u32 nstatics, u32 own_gen)
    {
        Assembler::Label no_irq;
        // exit_: an exception, break_run_ (mtspr DEC), a checkstop, blocks
        // dropped ... (everything that sets those sets exit_ too).
        load_field(false, X8, exit_off, 1);
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
        a.ldr(false, X8, X20, pc_off());              // w8 = the next PC
        // Targets known now (b, bc and its fall-through within the page):
        // the fetch page, MSR[IR|PR] and the page's generation are this
        // block's (it changes none of them; anything that does sets exit_),
        // so the block is found at a fixed table slot and checked against
        // fixed values.
        for (u32 k = 0; k < nstatics; ++k) {
            Assembler::Label other;
            add_const(X10, X21, static_cast<u32>(statics[k].delta));
            a.cmp(false, X8, X10);
            a.b_cond(Assembler::NE, other);
            a.mov_imm32(X12, (statics[k].pa >> 2) & layout.table_mask);  // its table slot
            a.mov_imm32(X10, statics[k].pa);
            a.mov_imm32(X11, own_gen);
            a.b_abs(stub_static);
            a.bind(other);
        }
        a.b_abs(stub_dispatch);                       // w8: the PC, w9: the MSR
    }

    // --- the shared code (once, at the start of the code buffer; see
    // Ppc750::jit_emit_stubs): the general lookup ("dispatch", from w8 =
    // the next PC and w9 = the MSR), entering a found block ("enter", x14
    // = the block) and the common epilogue. They run in the frame the
    // first block of the chain pushed.
    void emit_stubs(u32& dispatch, u32& enter_at, u32& epilogue, u32& static_at, u32& entry)
    {
        Assembler::Label out, enter;
        // "static": a target known at compile time: w12 = its table slot,
        // w10 = its physical address, w11 = the generation it must have.
        static_at = a.size();
        load_field(true, X13, table_off, 8);
        a.add(true, X13, X13, X12, 2);
        a.ldr(false, X13, X13, 0);
        a.cbz(false, X13, out);
        a.sub_imm(false, X13, X13, 1);
        load_field(true, X14, pool_off, 8);
        a.add(true, X14, X14, X13, 4);                // x14 = the block
        a.ldr(false, X13, X14, blk_pa);
        a.cmp(false, X13, X10);
        a.b_cond(Assembler::NE, out);
        a.ldr(false, X13, X14, blk_gen);
        a.cmp(false, X13, X11);
        a.b_cond(Assembler::NE, out);                 // built under another generation
        a.b(enter);
        // "entry", from run_blocks (x0 = the CPU): push the frame, x19 and
        // x20 as every block expects, then the block at jit_entry_.
        entry = a.size();
        a.stp_pre(X29, X30, SP, -96);
        a.mov_from_sp(X29);
        a.stp(X19, X20, SP, 16);
        a.stp(X21, X22, SP, 32);
        a.stp(X23, X24, SP, 48);
        a.stp(X25, X26, SP, 64);
        a.stp(X27, X28, SP, 80);
        a.mov(true, X19, X0);
        {
            const u32 s_off = static_cast<u32>(reinterpret_cast<const u8*>(&c.s_) - reinterpret_cast<const u8*>(&c));
            if (s_off < 4096) {
                a.add_imm(true, X20, X19, s_off);
            } else {
                a.mov_imm64(X20, s_off);
                a.add(true, X20, X19, X20);
            }
        }
        load_field(true, X16, entry_off, 8);
        a.br(X16);
        dispatch = a.size();
        // The fetch page: still valid, same effective page, same MSR[IR|PR].
        Assembler::Label miss, have_fetch;
        a.mov_imm32(X12, msr::IR | msr::PR);
        a.and_(false, X11, X9, X12);                  // w11 = MSR[IR|PR]
        load_field(true, X10, fetch_host_off, 8);
        a.cbz(true, X10, miss);
        load_field(false, X10, fetch_page_off, 4);
        a.lsr_imm(X10, X10, 12);
        a.lsr_imm(X12, X8, 12);
        a.cmp(false, X10, X12);
        a.b_cond(Assembler::NE, miss);
        load_field(false, X10, fetch_msr_off, 4);
        a.cmp(false, X10, X11);
        a.b_cond(Assembler::EQ, have_fetch);
        // Another page (a call, a return ...): run_blocks' fetch-page miss,
        // when its fast case applies: no code watch, real mode or an ITLB
        // hit, a page of RAM. Then the fetch page is that one, as there.
        a.bind(miss);
        {
            Assembler::Label real, got;
            load_field(true, X10, watch_off, 8);
            a.cbnz(true, X10, out);                   // the code watch: run_blocks calls it
            a.tbz(X9, bit_of(msr::IR), real);
            load_field(false, X10, tlb_on_off, 1);
            a.cbz(false, X10, out);
            a.lsr_imm(X13, X8, 12);                   // page
            a.ubfx(X12, X13, 0, 8);                   // index (256 entries)
            a.add(true, X12, X12, X12, 1);            // * 3
            a.mov_imm64(X14, itlb_off);
            a.add(true, X14, X19, X14);               // the ITLB
            a.add(true, X12, X14, X12, 2);            // * 4: the entry (12 bytes)
            a.ldr(false, X14, X12, tlb_tag_off);
            a.ubfx(X15, X9, bit_of(msr::PR), 1);
            a.orr(false, X15, X15, X13, 1);
            a.mov_imm32(X10, tlb_valid);
            a.orr(false, X15, X15, X10);              // the tag a hit has
            a.cmp(false, X14, X15);
            a.b_cond(Assembler::NE, out);             // ITLB miss: run_blocks translates
            a.ldr(false, X12, X12, tlb_pa_off);       // w12 = physical page
            a.b(got);
            a.bind(real);
            a.mov_imm32(X12, ~0xFFFu);
            a.and_(false, X12, X8, X12);              // w12 = pc & ~0xFFF
            a.bind(got);
            load_field(false, X13, ram_size_off, 4);  // the page lies in RAM (which starts at 0)
            a.sub_imm(false, X13, X13, 1, true);      // size - 0x1000
            a.cmp(false, X12, X13);
            a.b_cond(Assembler::HI, out);
            load_field(true, X14, ram_off, 8);
            a.add(true, X14, X14, X12);
            {
                u32 off = fetch_host_off;
                const Reg base = far(X19, off, 8);
                a.str(true, X14, base, off);
            }
            a.mov_imm32(X13, ~0xFFFu);
            a.and_(false, X13, X8, X13);              // pc & ~0xFFF
            {
                u32 off = fetch_page_off;
                const Reg base = far(X19, off, 4);
                a.str(false, X13, base, off);
            }
            {
                u32 off = fetch_pa_off;
                const Reg base = far(X19, off, 4);
                a.str(false, X12, base, off);
            }
            {
                u32 off = fetch_msr_off;
                const Reg base = far(X19, off, 4);
                a.str(false, X11, base, off);
            }
        }
        a.bind(have_fetch);
        // w10 = the physical address; its page must be one the cache covers.
        load_field(false, X10, fetch_pa_off, 4);
        a.ubfx(X11, X8, 0, 12);
        a.orr(false, X10, X10, X11);
        // (The page is RAM, checked when it became the fetch page, and the
        // cache covers all of RAM, as run() requires: it has a generation.)
        a.lsr_imm(X15, X10, 12);                      // w15 = page
        // The table slot: pool offset / kAlign + 1, 0 = empty.
        a.lsr_imm(X11, X10, 2);
        a.mov_imm32(X12, layout.table_mask);
        a.and_(false, X11, X11, X12);
        load_field(true, X12, table_off, 8);
        a.add(true, X12, X12, X11, 2);
        a.ldr(false, X13, X12, 0);
        a.cbz(false, X13, out);
        a.sub_imm(false, X13, X13, 1);
        load_field(true, X14, pool_off, 8);
        a.add(true, X14, X14, X13, 4);                // x14 = the block
        static_assert(BlockCache::kAlign == 16, "the generated lookup assumes 16-byte block alignment");
        a.ldr(false, X13, X14, blk_pa);
        a.cmp(false, X13, X10);
        a.b_cond(Assembler::NE, out);
        load_field(true, X12, gen_off, 8);
        a.add(true, X12, X12, X15, 2);
        a.ldr(false, X12, X12, 0);
        a.ldr(false, X13, X14, blk_gen);
        a.cmp(false, X12, X13);
        a.b_cond(Assembler::NE, out);                 // built under an older generation: dropped
        // --- x14: the next block, valid: code, budget, then into it.
        a.bind(enter);
        enter_at = a.size();
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
        // Into its code: code_base + 4 * (code - 1).
        static_assert(kChainEntry == 0, "blocks are entered at their start");
        a.sub_imm(false, X15, X15, 1);
        load_field(true, X12, code_off, 8);
        a.add(true, X12, X12, X15, 2);
        a.br(X12);
        // --- the epilogue: back to run_blocks.
        a.bind(out);
        epilogue = a.size();
        a.ldp(X27, X28, SP, 80);
        a.ldp(X25, X26, SP, 64);
        a.ldp(X23, X24, SP, 48);
        a.ldp(X21, X22, SP, 32);
        a.ldp(X19, X20, SP, 16);
        a.ldp_post(X29, X30, SP, 96);
        a.ret();
    }
    static constexpr u32 blk_pa = __builtin_offsetof(BlockCache::Block, pa);
    static constexpr u32 blk_gen = __builtin_offsetof(BlockCache::Block, gen);
    static constexpr u32 blk_count = __builtin_offsetof(BlockCache::Block, count);
    static constexpr u32 blk_code = __builtin_offsetof(BlockCache::Block, code);

    // The block's possible next PCs that are known now and lie in its page:
    // a final b or bc (taken, and not taken), relative, generated natively.
    static u32 static_targets(const BlockCache::Block& b, Static* out)
    {
        const u32 last = b.ops[b.count - 1].insn;
        const u32 op = field::opcd(last);
        if (!is_branch(last) || op == 19 || field::aa(last))
            return 0;  // bclr/bcctr: computed; AA: absolute (the physical page is not known)
        const s32 here = static_cast<s32>(4 * (b.count - 1));
        s32 deltas[2];
        u32 n = 0;
        if (op == 18) {
            deltas[n++] = here + static_cast<s32>(sign_extend(last & 0x03FFFFFCu, 26));
        } else {
            deltas[n++] = here + static_cast<s32>(sign_extend(last & 0xFFFCu, 16));
            deltas[n++] = here + 4;
        }
        u32 kept = 0;
        for (u32 k = 0; k < n; ++k) {
            const s32 off = static_cast<s32>(b.pa & 0xFFFu) + deltas[k];
            if (off < 0 || off > 0xFFC)
                continue;  // another page
            out[kept++] = Static{deltas[k], (b.pa & ~0xFFFu) + static_cast<u32>(off)};
        }
        return kept;
    }

    static JitCompiler make(Ppc750& c, Assembler& a, Ppc750::JitStats& st)
    {
        auto off = [&c](const void* p) {
            return static_cast<u32>(static_cast<const u8*>(p) - reinterpret_cast<const u8*>(&c));
        };
        JitCompiler jc{c, a, off(&c.instr_count_), off(&c.exit_), off(&c.ram_), off(&c.ram_size_), off(&c.dtlb_[0]),
                       off(&c.tlb_enabled_), off(&c.jit_stats_.memory_slow), &st};
        jc.table_off = off(&c.jit_table_);
        jc.pool_off = off(&c.jit_pool_);
        jc.gen_off = off(&c.jit_gen_);
        jc.code_off = off(&c.jit_code_);
        jc.watch_off = off(&c.watch_cb_);
        jc.itlb_off = off(&c.itlb_[0]);
        jc.profile_off = off(&c.jit_profile_[0]);
        jc.break_off = off(&c.break_run_);
        jc.checkstop_off = off(&c.checkstop_);
        jc.ext_off = off(&c.external_pending_);
        jc.dec_off = off(&c.dec_pending_);
        jc.fetch_host_off = off(&c.fetch_host_);
        jc.fetch_page_off = off(&c.fetch_page_);
        jc.fetch_msr_off = off(&c.fetch_msr_);
        jc.fetch_pa_off = off(&c.fetch_pa_page_);
        jc.limit_off = off(&c.jit_limit_);
        jc.chained_off = off(&c.jit_stats_.chained);
        jc.layout = c.cache_->layout();
        jc.code_base = c.jit_code_;
        jc.stub_dispatch = c.jit_code_ + c.jit_stub_dispatch_;
        jc.stub_enter = c.jit_code_ + c.jit_stub_enter_;
        jc.stub_epilogue = c.jit_code_ + c.jit_stub_epilogue_;
        jc.stub_static = c.jit_code_ + c.jit_stub_static_;
        jc.entry_off = off(&c.jit_entry_);
        jc.code_pages_off = off(&c.jit_code_pages_);
        jc.dtlb_ptr_off = off(&c.jit_dtlb_);
        return jc;
    }

    bool compile(BlockCache::Block& b)
    {
        Assembler::Label out;
        // Entered with x19, x20 set and the frame pushed: by the shared
        // entry stub (from run_blocks) or by a chained jump.
        plan_cache(b, has_memory_ops(b));
        a.ldr(false, X21, X20, pc_off());
        if (has_memory_ops(b)) {
            // x22 = RAM, x23 = the cache's per-page code flags, x24 = DTLB.
            u32 off = ram_off;
            const Reg base = far(X19, off, 8);
            a.ldr(true, X22, base, off);
            load_field(true, X23, code_pages_off, 8);
            load_field(true, X24, dtlb_ptr_off, 8);
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
            flush();   // the handler reads (and may change) the CpuState
            call_handler(i, b.count, op, out);
            forget();
            ++st->helper_ops;
        }
        sync(b.count, pending);
        flush();       // the next block, or run_blocks, reads the CpuState
        Static statics[2];
        const u32 nstatics = static_targets(b, statics);
        chain(out, statics, nstatics, b.gen);
        a.bind(out);
        a.b_abs(stub_epilogue);
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

// A compiler for `a`, with the offsets of everything generated code reads.
static JitCompiler make_compiler(Ppc750& c, Assembler& a, Ppc750::JitStats& st)
{
    return JitCompiler::make(c, a, st);
}

bool Ppc750::jit_emit_stubs()
{
    Assembler a(jit_code_, jit_words_);
    JitStats st;
    JitCompiler jc = make_compiler(*this, a, st);
    jc.emit_stubs(jit_stub_dispatch_, jit_stub_enter_, jit_stub_epilogue_, jit_stub_static_, jit_stub_entry_);
    if (!a.ok())
        return false;
    sync_icache(jit_code_, jit_code_ + a.size());
    jit_stub_words_ = (a.size() + 15) & ~15u;  // blocks start on a 64-byte boundary after it
    return true;
}

bool Ppc750::jit_generate(BlockCache::Block& b, u32* at, u32 room, u32& size, JitStats& st)
{
    Assembler a(at, room);
    JitCompiler jc = make_compiler(*this, a, st);
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
        jit_used_ = jit_stub_words_;  // (the shared code stays)
        ++jit_stats_.resets;
    }
    u32 size = 0;
    if (!jit_generate(*b, jit_code_ + jit_used_, jit_words_ - jit_used_, size, jit_stats_))
        return false;  // out of room: the block runs without code
    b->code = jit_used_ + 1;
    jit_used_ += size;
    jit_stats_.code_words += size;
    ++jit_stats_.compiled;
    return true;
}

// --- background compilation (jit_worker.h) ------------------------------------------

void Ppc750::set_jit_worker(JitWorker* w)
{
    jit_worker_ = jit_code_ ? w : nullptr;
    if (cache_)
        cache_->flush();  // a clean start: the worker owns the code buffer now
    jit_used_ = jit_stub_words_;
}

void Ppc750::jit_request(BlockCache::Block* b)
{
    b->asked = static_cast<u32>(instr_count_);
    JitRequest& r = jit_worker_->req_out_;
    r.block = b;
    r.epoch = cache_->stats().flushes;
    r.pa = b->pa;
    r.gen = b->gen;
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
            used_ = cpu_.jit_stub_words_;  // after the shared code
            ++stats_.resets;
        }
        auto* b = reinterpret_cast<BlockCache::Block*>(block_);
        b->pa = req_.pa;
        b->gen = req_.gen;
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
    jit_entry_ = jit_code_ + (b->code - 1);  // the entry stub jumps there
    void* code = jit_code_ + jit_stub_entry_;
    ++jit_stats_.runs;
    if (jit_runner_)
        jit_runner_(code, this, jit_ctx_);
#if defined(__aarch64__)
    else
        reinterpret_cast<void (*)(Ppc750*)>(code)(this);
#endif
}

} // namespace g3::ppc
