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
    // A block's end reads exit_ and the interrupt inputs as one word.
    if (reinterpret_cast<uintptr_t>(&exit_) % 4 != 0 || reinterpret_cast<u8*>(&external_pending_) != &exit_ + 1 ||
        reinterpret_cast<u8*>(&dec_pending_) != &exit_ + 2 || &stop_pad_ != &exit_ + 3)
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
    jit_far_count_ = 0;
    jit_fast_ptr_ = jit_fast_;
    fast_used_ = 0xF;
    jit_fast_drop(false);
    jit_jump_ptr_ = jit_jump_;
    jump_used_ = 1;
    jit_jump_drop();
    cache_->set_hooks(&Ppc750::jit_cache_invalidated, &Ppc750::jit_cache_flushed, &Ppc750::jit_cache_marked,
                      this);
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
    static constexpr u32 fast_read_off = __builtin_offsetof(Ppc750::FastEntry, read);
    static constexpr u32 fast_write_off = __builtin_offsetof(Ppc750::FastEntry, write);
    static constexpr u32 fast_addend_off = __builtin_offsetof(Ppc750::FastEntry, addend);
    static_assert(sizeof(Ppc750::FastEntry) == 16 && Ppc750::kFastSize == 256,
                  "the generated lookup assumes 16-byte entries, 4096-byte tables");
    static_assert(sizeof(Ppc750::JumpEntry) == 16 && Ppc750::kJumpSize == 1024,
                  "dispatch's jump cache lookup assumes 16-byte entries, 10 index bits");
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
    const u32* stub_dispatch_link = nullptr;
    u32 far_slot_off = 0;
    u32 entry_off = 0, code_pages_off = 0, dtlb_ptr_off = 0, link_block_off = 0, link_slot_off = 0;
    u32 fast_ptr_off = 0, jump_ptr_off = 0, jump_used_off = 0;
    u8* kinds = nullptr;  // Ppc750::jit_word_kinds_ (diagnostics)
    void tag_one(u32 w, u8 kind)
    {
        if (kinds && a.ok())
            kinds[static_cast<u64>(a.start() - code_base) + w] = kind;
    }
    // Tags the words emitted since `from` (diagnostics only).
    void tag(u32 from, u8 kind)
    {
        if (!kinds || !a.ok())
            return;
        const u64 base = static_cast<u64>(a.start() - code_base);
        for (u32 i = from; i < a.size(); ++i)
            kinds[base + i] = kind;
    }
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
        const u32 slots = memory ? 6 : 7;  // memory: x24 holds the fast table
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
        enum In : u8 { RA, NOT_RA, RB, ZERO, ONES, CA, ONE, IMM };
        In x, y, cin;
        bool writes_ca = true;
        bool rc = field::rc(insn), oe = field::oe(insn);
        const u32 op = field::opcd(insn);
        if (op != 31) {  // D-form: addic, addic. (CR0), subfic: as op_add_family with xo 0
            oe = false;
            switch (op) {
            case 12: x = RA; y = IMM; cin = ZERO; rc = false; break;
            case 13: x = RA; y = IMM; cin = ZERO; rc = true; break;
            case 8:  x = NOT_RA; y = IMM; cin = ONE; rc = false; break;
            default: return false;
            }
        } else switch (field::xo9(insn)) {
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
            case IMM: a.mov_imm32(w, field::simm(insn)); break;
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
        if (rc) {
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
        case 8: case 12: case 13:  // subfic addic addic.
            return add_family(insn);
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
            case 144: {  // mtcrf: CR fields picked by FXM from rS
                if (field::rc(insn) || (insn & 0x00100800u))
                    return false;  // bit 11 and bit 20 must be 0
                const u32 fxm = (insn >> 12) & 0xFF;
                u32 mask = 0;
                for (u32 f = 0; f < 8; ++f)
                    if (fxm & (0x80u >> f))
                        mask |= 0xF0000000u >> (4 * f);
                const Reg vs = use(rd, X8);
                a.ldr(false, X9, X20, cr_off());
                a.mov_imm32(X10, ~mask);
                a.and_(false, X9, X9, X10);
                a.mov_imm32(X10, mask);
                a.and_(false, X10, vs, X10);
                a.orr(false, X9, X9, X10);
                a.str(false, X9, X20, cr_off());
                return true;
            }
            case 19: {  // mfcr
                if (field::rc(insn) || (insn & 0x001FF800u))
                    return false;
                const Reg d = def(rd, X8);
                a.ldr(false, d, X20, cr_off());
                defined(rd, d);
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
            case 339: {  // mfspr XER / LR / CTR (user-readable: no exception)
                const u32 n = ((insn >> 16) & 31) | ((insn >> 6) & 0x3E0);
                if (field::rc(insn) || (n != spr::LR && n != spr::CTR && n != spr::XER))
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

    // --- D- and X-form integer loads and stores ------------------------------------
    // The fast path is one lookup in the fast table (Ppc750::FastEntry):
    // a hit has made mem_fast's checks already (one page; real mode or a
    // data-TLB hit, for stores writable with C set; the RAM window; stores:
    // no blocks in the page). Then the access itself, big-endian. A miss
    // fills the entry from the TLB (jit_fill) and runs the instruction's
    // handler, which does it all over (nothing has been changed before).
    struct MemOp {
        u32 size;
        bool store, update;
        bool sign = false;  // lha, lhau: sign-extend the halfword
        bool indexed = false;  // X-form: EA = (rA|0) + rB
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
        case 31:
            if (field::rc(insn))
                return false;
            switch (field::xo10(insn)) {
            case 23:  m = {4, false, false}; break;        // lwzx
            case 55:  m = {4, false, true}; break;         // lwzux
            case 87:  m = {1, false, false}; break;        // lbzx
            case 119: m = {1, false, true}; break;         // lbzux
            case 151: m = {4, true, false}; break;         // stwx
            case 183: m = {4, true, true}; break;          // stwux
            case 215: m = {1, true, false}; break;         // stbx
            case 247: m = {1, true, true}; break;          // stbux
            case 279: m = {2, false, false}; break;        // lhzx
            case 311: m = {2, false, true}; break;         // lhzux
            case 343: m = {2, false, false, true}; break;  // lhax
            case 375: m = {2, false, true, true}; break;   // lhaux
            case 407: m = {2, true, false}; break;         // sthx
            case 439: m = {2, true, true}; break;          // sthux
            default: return false;
            }
            m.indexed = true;
            return true;
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
        Assembler::Label slow, done;
        const u32 tag_from = a.size();
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
        if (m.indexed) {
            const Reg vb = use(field::rb(insn), X9);
            a.add(false, X8, base_reg, vb);
        } else {
            add_const(X8, base_reg, field::simm(insn));
        }
        // The fast table (x24: the one for this block's MSR[DR, PR]). The
        // key is the page of the access's last byte, the index that of its
        // first: an access crossing into the next page never hits (that
        // page's entry sits at the next index).
        if (m.size > 1) {
            a.add_imm(false, X13, X8, m.size - 1);
            a.lsr_imm(X13, X13, 12);
        } else {
            a.lsr_imm(X13, X8, 12);
        }
        a.ubfx(X14, X8, 12, 8);
        a.add(true, X14, X24, X14, 4);                  // * 16: the entry
        a.ldr(false, X15, X14, m.store ? fast_write_off : fast_read_off);
        a.cmp(false, X13, X15);
        a.b_cond(Assembler::NE, slow);
        a.ldr(true, X15, X14, fast_addend_off);         // host = addend + EA (x8: zero-extended)
        if (m.store) {
            Reg v = use(rd, X10);
            if (m.size == 4) { a.rev(X10, v); v = X10; }
            else if (m.size == 2) { a.rev16(X10, v); v = X10; }
            a.str_reg(m.size, v, X15, X8);
        } else {
            a.ldr_reg(m.size, X10, X15, X8);
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
        tag(tag_from, Ppc750::kWordMemFast);
        const u32 slow_from = a.size();
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
        a.mov(true, X0, X19);                           // jit_fill(cpu, EA): for the next time
        a.mov(false, X1, X8);
        a.mov_imm64(X16, reinterpret_cast<u64>(&Ppc750::jit_fill));
        a.blr(X16);
        call_handler(i, count, op, out);
        for (u32 r = 0; r < 32; ++r) {
            if (cached(r) && valid1[r])
                a.ldr(false, host(r), X20, gpr_off(r));
            valid_[r] = valid1[r];
            dirty_[r] = dirty1[r];  // (at most stores one value again)
        }
        tag(slow_from, Ppc750::kWordMemSlow);
        a.bind(done);
    }

    // --- mfspr of a supervisor SPR with no side effects on read: the
    // value in supervisor mode; in user mode (MSR[PR]) the handler, which
    // raises the program exception (op_mfspr). As a memory access: the
    // register cache written back before, reloaded after the handler.
    static bool supervisor_spr_read(u32 insn)
    {
        if (field::opcd(insn) != 31 || field::xo10(insn) != 339 || field::rc(insn))
            return false;
        switch (((insn >> 16) & 31) | ((insn >> 6) & 0x3E0)) {
        case spr::SRR0: case spr::SRR1: case spr::SPRG0: case spr::SPRG1: case spr::SPRG2: case spr::SPRG3:
        case spr::DAR: case spr::DSISR: case spr::SDR1: case spr::HID0: case spr::HID1:
            return true;
        default:
            return false;
        }
    }
    void spr_read(u32 i, u32 count, const BlockOp& op, Assembler::Label& out)
    {
        const u32 insn = op.insn, rd = field::rd(insn);
        const u32 n = ((insn >> 16) & 31) | ((insn >> 6) & 0x3E0);
        Assembler::Label slow, done;
        bool dirty0[32];
        for (u32 r = 0; r < 32; ++r)
            dirty0[r] = dirty_[r];
        a.ldr(false, X9, X20, msr_off());
        a.tbnz(X9, bit_of(msr::PR), slow);
        a.ldr(false, X10, X20, spr_off(n));
        store_gpr(X10, rd);
        u32 one = 1;
        sync(i + 1, one);
        bool valid1[32], dirty1[32];
        for (u32 r = 0; r < 32; ++r) {
            valid1[r] = valid_[r];
            dirty1[r] = dirty_[r];
        }
        a.b(done);
        a.bind(slow);
        for (u32 r = 0; r < 32; ++r)
            dirty_[r] = dirty0[r];
        flush();
        a.mov(true, X0, X19);                           // jit_fill(cpu, EA): for the next time
        a.mov(false, X1, X8);
        a.mov_imm64(X16, reinterpret_cast<u64>(&Ppc750::jit_fill));
        a.blr(X16);
        call_handler(i, count, op, out);
        for (u32 r = 0; r < 32; ++r) {
            if (cached(r) && valid1[r])
                a.ldr(false, host(r), X20, gpr_off(r));
            valid_[r] = valid1[r];
            dirty_[r] = dirty1[r];
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
        u32 pa;     // (in the block's page; unknown for a far target)
        bool far;   // in another page: linked at run time (Ppc750::jit_far_link)
    };
    u8 dispatch_kind = 0;  // diagnostics: what kind of block end jumps to dispatch
    void chain(Assembler::Label& out, const Static* statics, u32 nstatics, u32 own_gen, bool computed,
               bool fresh)
    {
        Assembler::Label stop_word, stop_checked;
        // exit_ (an exception, break_run_ (mtspr DEC), a checkstop, blocks
        // dropped, MSR[POW] set ...: everything that sets those sets exit_
        // too) and the interrupt inputs, in one word: all zero is the
        // common case; otherwise stop_word, after the rest.
        load_field(false, X10, exit_off, 4);
        a.cbnz(false, X10, stop_word);
        a.bind(stop_checked);
        if (!fresh)
            a.ldr(false, X8, X20, pc_off());          // w8 = the next PC
        // Targets known now (b, bc and its fall-through within the page):
        // the fetch page, MSR[IR|PR] and the page's generation are this
        // block's (it changes none of them; anything that does sets exit_),
        // so the block is found at a fixed table slot and checked against
        // fixed values.
        // Each has a link slot: a NOP until the "static" stub has found its
        // block with code, then a direct branch into that block (patched by
        // Ppc750::jit_link). Same page, so anything that drops the target
        // drops this block too, and sets exit_ (checked above). Before a
        // linked jump, the budget as if the next block had kMaxOps
        // instructions; run_blocks and the "enter" stub do it exactly.
        Assembler::Label generic;
        if (nstatics || computed) {
            if (fresh) {
                a.add_imm(true, X11, X9, BlockCache::kMaxOps);  // just written
            } else {
                load_field(true, X11, cnt_off, 8);
                a.add_imm(true, X11, X11, BlockCache::kMaxOps);
            }
            load_field(true, X12, limit_off, 8);
            a.cmp(true, X11, X12);
            a.b_cond(Assembler::HI, generic);
        }
        bool any_far = computed;
        for (u32 k = 0; k < nstatics; ++k)
            any_far = any_far || statics[k].far;
        if (any_far)
            a.ldr(false, X9, X20, msr_off());         // w9 = the MSR (the links' mode)
        for (u32 k = 0; k < nstatics; ++k) {
            Assembler::Label other;
            add_const(X10, X21, static_cast<u32>(statics[k].delta));
            a.cmp(false, X8, X10);
            a.b_cond(Assembler::NE, other);
            if (statics[k].far) {
                // Another page: a link made under one MSR[IR|PR] (the movz,
                // patched with it; 0xFFFF matches none) and undone when the
                // translation or the target's page changes (jit_far_link).
                Assembler::Label unlinked;
                a.mov_imm32(X12, msr::IR | msr::PR);
                a.and_(false, X11, X9, X12);
                const u32* mode = a.start() + a.size();
                a.movz(false, X12, 0xFFFF, 0);        // patched: the link's MSR[IR|PR]
                a.cmp(false, X11, X12);
                a.b_cond(Assembler::NE, unlinked);
                a.nop();                              // patched: b to the target (mode + 3)
                a.bind(unlinked);
                a.mov_imm64(X15, reinterpret_cast<u64>(mode));
                a.b_abs(stub_dispatch_link);
                a.bind(other);
                continue;
            }
            const u32* slot = a.start() + a.size();
            a.nop();                                  // the link slot
            a.mov_imm32(X12, (statics[k].pa >> 2) & layout.table_mask);  // its table slot
            a.mov_imm32(X10, statics[k].pa);
            a.mov_imm32(X11, own_gen);
            a.mov_imm64(X15, reinterpret_cast<u64>(slot));
            a.b_abs(stub_static);
            a.bind(other);
        }
        if (computed) {
            // bclr/bcctr: the first target seen (its address and MSR[IR|PR],
            // patched into the movz/movk) is linked like a far target;
            // another target takes dispatch without relinking.
            Assembler::Label link_path;
            a.mov_imm32(X12, msr::IR | msr::PR);
            a.and_(false, X11, X9, X12);
            const u32* mode = a.start() + a.size();
            a.movz(false, X12, 0xFFFF, 0);            // 0: patched: the link's MSR[IR|PR]
            a.cmp(false, X11, X12);                   // 1
            a.b_cond(Assembler::NE, link_path);       // 2: unlinked, or another mode
            a.movz(false, X12, 0, 0);                 // 3: patched: target address, low half
            a.movk(false, X12, 0, 16);                // 4: ... high half
            a.cmp(false, X8, X12);                    // 5
            a.b_cond(Assembler::NE, generic);         // 6: another target
            a.nop();                                  // 7: patched: b to the target
            a.bind(link_path);
            a.mov_imm64(X15, reinterpret_cast<u64>(mode));
            a.b_abs(stub_dispatch_link);
        }
        a.bind(generic);
        a.ldr(false, X9, X20, msr_off());
        const u32 to_dispatch = a.size();             // (one word: G3_JITHIST counts hops by it)
        a.b_abs(stub_dispatch);                       // w8: the PC, w9: the MSR
        tag(to_dispatch, dispatch_kind);
        // Something in the stop word: exit_ stops; an interrupt input only
        // if MSR[EE] lets it in (run_blocks takes it), else go on. (w8 and
        // x9 may hold the next PC and the count: kept.)
        a.bind(stop_word);
        a.ubfx(X11, X10, 0, 8);
        a.cbnz(false, X11, out);
        a.ldr(false, X11, X20, msr_off());
        a.tbz(X11, bit_of(msr::EE), stop_checked);
        a.b(out);
    }

    // --- the shared code (once, at the start of the code buffer; see
    // Ppc750::jit_emit_stubs): the general lookup ("dispatch", from w8 =
    // the next PC and w9 = the MSR), entering a found block ("enter", x14
    // = the block) and the common epilogue. They run in the frame the
    // first block of the chain pushed.
    void emit_stubs(u32& dispatch, u32& enter_at, u32& epilogue, u32& static_at, u32& entry, u32& dispatch_link)
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
        // It has code: link the caller's slot (x15) to it, once.
        a.ldr(false, X13, X14, blk_code);
        a.cbz(false, X13, enter);                     // no code yet: enter sends it back
        {
            u32 off = link_block_off;
            const Reg base = far(X19, off, 8);
            a.str(true, X14, base, off);
        }
        {
            u32 off = link_slot_off;
            const Reg base = far(X19, off, 8);
            a.str(true, X15, base, off);
        }
        a.mov(true, X0, X19);
        a.mov_imm64(X16, reinterpret_cast<u64>(&Ppc750::jit_link));
        a.blr(X16);                                   // (x0-x18 not kept)
        load_field(true, X14, link_block_off, 8);
        a.b(enter);
        // "dispatch_link": as dispatch, then link the far target slot at x15
        // to what it finds ("enter" does it).
        Assembler::Label body;
        dispatch_link = a.size();
        {
            u32 off = far_slot_off;
            const Reg base = far(X19, off, 8);
            a.str(true, X15, base, off);
        }
        a.b(body);
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
        {
            u32 off = far_slot_off;
            const Reg base = far(X19, off, 8);
            a.str(true, ZR, base, off);               // no far link to make
        }
        a.bind(body);
        Assembler::Label miss, have_fetch, lookup;
        a.mov_imm32(X12, msr::IR | msr::PR);
        a.and_(false, X11, X9, X12);                  // w11 = MSR[IR|PR]
        // The jump cache (Ppc750::JumpEntry): this PC in this mode found
        // before, and its block's page not dropped since.
        a.orr(true, X12, X8, X11, 32);                // the key (x8: zero-extended)
        a.ubfx(X13, X8, 2, 10);
        load_field(true, X10, jump_ptr_off, 8);
        a.add(true, X13, X10, X13, 4);                // * 16: the entry
        a.ldr(true, X10, X13, 0);
        a.cmp(true, X10, X12);
        a.b_cond(Assembler::NE, lookup);
        a.ldr(true, X14, X13, 8);                     // x14 = the block
        a.ldr(false, X15, X14, blk_pa);
        a.lsr_imm(X15, X15, 12);
        load_field(true, X12, gen_off, 8);
        a.add(true, X12, X12, X15, 2);
        a.ldr(false, X12, X12, 0);
        a.ldr(false, X13, X14, blk_gen);
        a.cmp(false, X12, X13);
        a.b_cond(Assembler::EQ, enter);
        a.bind(lookup);                               // (w11 still the mode)
        // The fetch page: still valid, same effective page, same MSR[IR|PR].
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
        {   // Into the jump cache (w8, w9 as dispatch got them), unless a
            // code watch is set.
            Assembler::Label no_fill;
            load_field(true, X10, watch_off, 8);
            a.cbnz(true, X10, no_fill);
            a.mov_imm32(X12, msr::IR | msr::PR);
            a.and_(false, X11, X9, X12);
            a.orr(true, X12, X8, X11, 32);
            a.ubfx(X13, X8, 2, 10);
            load_field(true, X10, jump_ptr_off, 8);
            a.add(true, X13, X10, X13, 4);
            a.str(true, X12, X13, 0);
            a.str(true, X14, X13, 8);
            a.movz(false, X10, 1, 0);
            u32 off = jump_used_off;
            const Reg base = far(X19, off, 1);
            a.strb(X10, base, off);
            a.bind(no_fill);
        }
        // --- x14: the next block, valid: code, budget, then into it.
        a.bind(enter);
        enter_at = a.size();
        a.ldr(false, X15, X14, blk_code);
        a.cbz(false, X15, out);                       // no code (yet)
        {   // a far link asked for (dispatch_link): make it
            Assembler::Label nolink;
            load_field(true, X13, far_slot_off, 8);
            a.cbz(true, X13, nolink);
            u32 off = link_block_off;
            const Reg base = far(X19, off, 8);
            a.str(true, X14, base, off);
            a.mov(true, X0, X19);
            a.mov_imm64(X16, reinterpret_cast<u64>(&Ppc750::jit_far_link));
            a.blr(X16);                               // (x0-x18 not kept)
            load_field(true, X14, link_block_off, 8);
            a.ldr(false, X15, X14, blk_code);
            a.bind(nolink);
        }
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
        {
            u32 off = far_slot_off;
            const Reg base = far(X19, off, 8);
            a.str(true, ZR, base, off);               // a far link not made stays unmade
        }
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
                out[kept++] = Static{deltas[k], 0, true};  // another page
            else
                out[kept++] = Static{deltas[k], (b.pa & ~0xFFFu) + static_cast<u32>(off), false};
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
        jc.stub_dispatch_link = c.jit_code_ + c.jit_stub_dispatch_link_;
        jc.far_slot_off = off(&c.jit_far_slot_);
        jc.entry_off = off(&c.jit_entry_);
        jc.code_pages_off = off(&c.jit_code_pages_);
        jc.dtlb_ptr_off = off(&c.jit_dtlb_);
        jc.fast_ptr_off = off(&c.jit_fast_ptr_);
        jc.jump_ptr_off = off(&c.jit_jump_ptr_);
        jc.jump_used_off = off(&c.jump_used_);
        jc.kinds = c.jit_word_kinds_;
        jc.link_block_off = off(&c.jit_link_block_);
        jc.link_slot_off = off(&c.jit_link_slot_);
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
            // x24 = the fast table for MSR[DR, PR] (constant in a block: what
            // changes them ends it or leaves it, ends_block).
            a.ldr(false, X9, X20, msr_off());
            a.ubfx(X10, X9, bit_of(msr::DR), 1);
            a.ubfx(X11, X9, bit_of(msr::PR), 1);
            a.add(false, X10, X10, X11, 1);
            load_field(true, X24, fast_ptr_off, 8);
            a.add(true, X24, X24, X10, 12);             // * 4096: a table
        }

        tag(0, Ppc750::kWordSetup);
        u32 pending = 0;
        bool ended_with_branch = false;  // branch() leaves w8 = the next PC, x9 = the count
        for (u32 i = 0; i < b.count; ++i) {
            const BlockOp& op = b.ops[i];
            u32 at = a.size();
            if (native(op.insn)) {
                ++pending;
                ++st->native_ops;
                tag(at, Ppc750::kWordNative);
                continue;
            }
            if (i + 1 == b.count && is_branch(op.insn)) {
                branch(i, op.insn, pending);
                ended_with_branch = true;
                ++st->native_ops;
                tag(at, Ppc750::kWordBranch);
                continue;
            }
            sync(i, pending);
            tag(at, Ppc750::kWordSync);
            at = a.size();
            MemOp m;
            if (memory_op(op.insn, m)) {
                memory(i, b.count, op, m, out);  // tags its own words
                ++st->native_ops;
                continue;
            }
            if (supervisor_spr_read(op.insn)) {
                spr_read(i, b.count, op, out);
                ++st->native_ops;
                tag(at, Ppc750::kWordNative);
                continue;
            }
            flush();   // the handler reads (and may change) the CpuState
            call_handler(i, b.count, op, out);
            forget();
            ++st->helper_ops;
            tag(at, Ppc750::kWordHandler);
        }
        u32 at = a.size();
        // w8 = the next PC and x9 = the count, as the block's last code
        // left them: branch(), or sync() after a native last instruction
        // (the next PC then is the one after it). flush only stores.
        const bool fresh = pending != 0 || ended_with_branch;
        sync(b.count, pending);
        flush();       // the next block, or run_blocks, reads the CpuState
        tag(at, Ppc750::kWordSync);
        at = a.size();
        Static statics[2];
        const u32 nstatics = static_targets(b, statics);
        {
            const u32 last = b.ops[b.count - 1].insn;
            dispatch_kind = !is_branch(last) ? Ppc750::kWordToDispatchOther
                            : field::opcd(last) != 19 ? Ppc750::kWordToDispatchFar
                            : field::xo10(last) == 16 ? Ppc750::kWordToDispatchReturn : Ppc750::kWordToDispatchCtr;
        }
        const u32 last = b.ops[b.count - 1].insn;
        chain(out, statics, nstatics, b.gen, field::opcd(last) == 19 && is_branch(last), fresh);
        a.bind(out);
        a.b_abs(stub_epilogue);
        // (the branch to dispatch keeps its own tag)
        for (u32 w = at; w < a.size(); ++w)
            if (!kinds || kinds[static_cast<u64>(a.start() - code_base) + w] < Ppc750::kWordToDispatchReturn)
                tag_one(w, Ppc750::kWordChainHead);
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
    jc.emit_stubs(jit_stub_dispatch_, jit_stub_enter_, jit_stub_epilogue_, jit_stub_static_, jit_stub_entry_,
                  jit_stub_dispatch_link_);
    if (!a.ok())
        return false;
    sync_icache(jit_code_, jit_code_ + a.size());
    jit_stub_words_ = (a.size() + 15) & ~15u;  // blocks start on a 64-byte boundary after it
    return true;
}

// Called from the "static" stub: turns the NOP at jit_link_slot_ into a
// branch to jit_link_block_'s code (B, imm26: the buffer is far smaller
// than +-128 MB), and makes it visible to instruction fetch.
void Ppc750::jit_link(void* cpu, u32)
{
    auto& c = *static_cast<Ppc750*>(cpu);
    u32* slot = c.jit_link_slot_;
    const u32* target = c.jit_code_ + (c.jit_link_block_->code - 1);
    const s64 words = target - slot;
    *slot = 0x14000000u | (static_cast<u32>(words) & 0x03FFFFFFu);
    sync_icache(slot, slot + 1);
    ++c.jit_stats_.linked;
}

// Called from the "enter" stub after dispatch_link found a block: the far
// target slot at jit_far_slot_ (its movz) gets this MSR[IR|PR] and a branch
// to the block's code, and the link is recorded, to be undone by
// jit_unlink_far (translation changes: flush_tlb; the target's page
// dropped: the block cache's invalidation hook).
void Ppc750::jit_far_link(void* cpu, u32)
{
    auto& c = *static_cast<Ppc750*>(cpu);
    u32* mode = c.jit_far_slot_;
    c.jit_far_slot_ = nullptr;
    if (!mode || c.jit_far_count_ >= kMaxFarLinks || c.watch_cb_)
        return;  // (a code watch: page changes go through dispatch)
    BlockCache::Block* b = c.jit_link_block_;
    const u32 bits = c.s_.msr & (msr::IR | msr::PR);
    // Two layouts: a far b/bc (the branch at mode[3]); a computed bclr/bcctr
    // (mode[3] is a movz w12 for the target address, the branch at mode[7]).
    const bool computed = (mode[3] & 0xFFE0001Fu) == (0x52800000u | 12);
    if (computed && (mode[0] >> 5 & 0xFFFF) != 0xFFFF)
        return;  // already linked (to another mode): keep it
    const u32 at = computed ? 7 : 3;
    u32* slot = mode + at;
    const u32* target = c.jit_code_ + (b->code - 1);
    if (computed) {
        const u32 ea = c.s_.pc;  // the target this time
        mode[3] = 0x52800000u | ((ea & 0xFFFF) << 5) | 12;  // movz w12, #lo
        mode[4] = 0x72A00000u | ((ea >> 16) << 5) | 12;     // movk w12, #hi, lsl 16
    }
    mode[0] = 0x52800000u | (bits << 5) | 12;  // movz w12, #bits
    slot[0] = 0x14000000u | (static_cast<u32>(target - slot) & 0x03FFFFFFu);
    sync_icache(mode, slot + 1);
    c.jit_far_links_[c.jit_far_count_++] = FarLink{mode, b->pa >> BlockCache::kPageShift, at};
    ++c.jit_stats_.linked_far;
}

// Undoes the far links into `page` (all of them with page = ~0u).
void Ppc750::jit_unlink_far(u32 page)
{
    u32 kept = 0;
    for (u32 i = 0; i < jit_far_count_; ++i) {
        FarLink& l = jit_far_links_[i];
        if (page != ~0u && l.page != page) {
            jit_far_links_[kept++] = l;
            continue;
        }
        l.mode[0] = 0x52800000u | (0xFFFFu << 5) | 12;  // movz w12, #0xFFFF: matches no mode
        l.mode[l.branch] = 0xD503201Fu;                  // nop
        sync_icache(l.mode, l.mode + 4);
        ++jit_stats_.unlinked_far;
    }
    jit_far_count_ = kept;
}

void Ppc750::jit_cache_invalidated(void* cpu, u32 page) { static_cast<Ppc750*>(cpu)->jit_unlink_far(page); }
void Ppc750::jit_cache_flushed(void* cpu)
{
    Ppc750& c = *static_cast<Ppc750*>(cpu);
    c.jit_far_count_ = 0;  // the code is dead
    c.jit_jump_drop();     // and the blocks
}

void Ppc750::jit_jump_drop()
{
    if (!jump_used_)
        return;
    for (JumpEntry& e : jit_jump_)
        e = JumpEntry{};
    jump_used_ = 0;
}
void Ppc750::jit_cache_marked(void* cpu, u32 page) { static_cast<Ppc750*>(cpu)->jit_fast_drop_writes(page); }

void Ppc750::jit_fast_drop(bool translated_only)
{
    for (u32 t = 0; t < 4; ++t) {
        if ((translated_only && !(t & 1)) || !(fast_used_ & (1u << t)))
            continue;  // real mode: pa = ea, whatever the MMU holds; or nothing filled
        for (u32 i = 0; i < kFastSize; ++i)
            jit_fast_[t * kFastSize + i] = FastEntry{};
        fast_used_ &= ~(1u << t);
    }
}

// Stores to the physical page `page` must take the handler again (it
// tells the block cache). The tables are indexed by effective page, so
// every entry that may write is checked.
void Ppc750::jit_fast_drop_writes(u32 page)
{
    if (!ram_)
        return;
    for (FastEntry& e : jit_fast_) {
        if (e.write == kFastNone)
            continue;
        const u64 host = e.addend + (static_cast<u64>(e.write) << 12);
        if (host - reinterpret_cast<u64>(ram_) == static_cast<u64>(page - (ram_base_ >> 12)) << 12)
            e.write = kFastNone;
    }
}

// Called by a generated load or store's slow path, before the handler,
// with the effective address: copies what the soft TLB already says about
// the page into the fast table (nothing is translated or changed here; a
// page the TLB does not hold yet is filled on a later miss, after the
// handler has translated it).
void Ppc750::jit_fill(void* cpu, u32 ea)
{
    Ppc750& c = *static_cast<Ppc750*>(cpu);
    const u32 page = ea >> 12;
    u32 pa_page = ea & ~0xFFFu;
    bool may_write = true;
    if (c.s_.msr & msr::DR) {
        const u32 tag = kTlbValid | (page << 1) | ((c.s_.msr & msr::PR) ? 1u : 0u);
        const TlbEntry& t = c.dtlb_[page & (kTlbSize - 1)];
        if (!c.tlb_enabled_ || t.tag != tag)
            return;
        pa_page = t.pa_page;
        may_write = t.writable && t.changed;
    }
    u8* host = c.ram_at(pa_page, 0x1000);
    if (!host)
        return;
    if (c.cache_ && (pa_page >> 12) < c.cache_->pages() && c.cache_->code_pages()[pa_page >> 12])
        may_write = false;
    const u32 table = ((c.s_.msr & msr::PR) ? 2u : 0u) | ((c.s_.msr & msr::DR) ? 1u : 0u);
    FastEntry& e = c.jit_fast_[table * kFastSize + (page & (kFastSize - 1))];
    e.read = page;
    e.write = may_write ? page : kFastNone;
    e.addend = reinterpret_cast<u64>(host) - (static_cast<u64>(page) << 12);
    c.fast_used_ |= 1u << table;
}

void Ppc750::set_jit_word_kinds(u8* kinds)
{
    jit_word_kinds_ = kinds;
    if (!kinds || !jit_code_)
        return;
    // The stubs, in the order emit_stubs writes them.
    const u32 edges[] = {jit_stub_static_, jit_stub_entry_, jit_stub_dispatch_, jit_stub_enter_,
                         jit_stub_epilogue_, jit_stub_words_};
    const u8 what[] = {kWordStubStatic, kWordStubEntry, kWordStubDispatch, kWordStubEnter, kWordStubEpilogue};
    for (u32 k = 0; k < 5; ++k)
        for (u32 w = edges[k]; w < edges[k + 1]; ++w)
            kinds[w] = what[k];
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
