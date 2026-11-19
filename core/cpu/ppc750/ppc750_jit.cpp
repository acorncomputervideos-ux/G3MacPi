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

#include "core/cpu/ppc750/ppc750.h"
#include "core/cpu/ppc750/jit/a64_assembler.h"
#include "core/common/bitops.h"


namespace g3::ppc {

using namespace a64;

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
            switch (field::xo10(insn)) {
            case 0:   // cmp
            case 32:  // cmpl
                load_gpr(X9, rb);
                compare(insn, X9, field::xo10(insn) == 0);
                return true;
            default:
                break;
            }
            // (The OE forms do not match below: OE is bit 9 of the 10-bit
            // extended opcode, so addo, subfo ... have xo10 >= 512.)
            switch (field::xo10(insn)) {
            case 266:  // add rD = rA + rB
            case 40:   // subf rD = rB - rA
                load_gpr(X8, ra);
                load_gpr(X9, rb);
                if (field::xo10(insn) == 266)
                    a.add(false, X8, X8, X9);
                else
                    a.sub(false, X8, X9, X8);
                store_gpr(X8, rd);
                if (field::rc(insn))
                    cr0_from_w8();
                return true;
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

    void call_handler(u32 i, u32 count, const BlockOp& op, Assembler::Label& out)
    {
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
                ++c.jit_stats_.native_ops;
                continue;
            }
            sync(i, pending);
            MemOp m;
            if (memory_op(op.insn, m)) {
                memory(i, b.count, op, m, out);
                ++c.jit_stats_.native_ops;
                continue;
            }
            call_handler(i, b.count, op, out);
            ++c.jit_stats_.helper_ops;
        }
        sync(b.count, pending);
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

bool Ppc750::jit_compile(BlockCache::Block* b)
{
    if (cache_->stats().flushes != jit_epoch_) {
        // The cache was flushed (possibly while allocating this very
        // block): no block refers to the code buffer any more.
        jit_epoch_ = cache_->stats().flushes;
        jit_used_ = 0;
        ++jit_stats_.resets;
    }
    Assembler a(jit_code_ + jit_used_, jit_words_ - jit_used_);
    auto off = [this](const void* p) {
        return static_cast<u32>(static_cast<const u8*>(p) - reinterpret_cast<const u8*>(this));
    };
    JitCompiler jc{*this, a, off(&instr_count_), off(&exit_), off(&ram_), off(&ram_size_), off(&dtlb_[0]),
                   off(&tlb_enabled_), off(&jit_stats_.memory_slow)};
    if (!jc.compile(*b))
        return false;  // out of room: the block runs without code
    sync_icache(jit_code_ + jit_used_, jit_code_ + jit_used_ + a.size());
    b->code = jit_used_ + 1;
    jit_used_ += a.size();
    ++jit_stats_.compiled;
    return true;
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
