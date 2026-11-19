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
// effective address that maps to it). w8-w12 are scratch; x16 holds call
// targets, x17 far addresses.

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

    // Generated code for `insn` if it is one of the simple instructions
    // (no exception possible, no CR/XER update); false otherwise.
    bool native(u32 insn)
    {
        const u32 rd = field::rd(insn), ra = field::ra(insn), rb = field::rb(insn);
        switch (field::opcd(insn)) {
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
        case 21: {  // rlwinm without Rc
            if (field::rc(insn))
                return false;
            const u32 mask = ppc_mask(field::mb(insn), field::me(insn));
            load_gpr(X8, rd);
            if (rb & 31)
                a.ror_imm(X8, X8, (32 - rb) & 31);  // rotate left by SH
            if (mask != 0xFFFFFFFFu) {
                a.mov_imm32(X9, mask);
                a.and_(false, X8, X8, X9);
            }
            store_gpr(X8, ra);
            return true;
        }
        case 31: {
            if (insn & 0x00000401u)  // OE or Rc
                return false;
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
                return true;
            case 339: {  // mfspr LR / CTR (user-readable: no exception)
                const u32 n = ((insn >> 16) & 31) | ((insn >> 6) & 0x3E0);
                if (n != spr::LR && n != spr::CTR)
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

    bool compile(BlockCache::Block& b)
    {
        Assembler::Label out;
        a.stp_pre(X29, X30, SP, -48);
        a.mov_from_sp(X29);
        a.stp(X19, X20, SP, 16);
        a.stp(X21, X22, SP, 32);
        a.mov(true, X19, X0);
        const u32 s_off = static_cast<u32>(reinterpret_cast<const u8*>(&c.s_) - reinterpret_cast<const u8*>(&c));
        if (s_off < 4096) {
            a.add_imm(true, X20, X19, s_off);
        } else {
            a.mov_imm64(X20, s_off);
            a.add(true, X20, X19, X20);
        }
        a.ldr(false, X21, X20, pc_off());

        u32 pending = 0;
        for (u32 i = 0; i < b.count; ++i) {
            const BlockOp& op = b.ops[i];
            if (native(op.insn)) {
                ++pending;
                ++c.jit_stats_.native_ops;
                continue;
            }
            sync(i, pending);
            a.mov(true, X0, X19);
            a.mov_imm32(X1, op.insn);
            a.mov_imm64(X16, reinterpret_cast<u64>(op.fn));
            a.blr(X16);
            ++c.jit_stats_.helper_ops;
            if (i + 1 < b.count) {
                u32 off = exit_off;
                const Reg base = far(X19, off, 1);
                a.ldrb(X8, base, off);
                a.cbnz(false, X8, out);
            }
        }
        sync(b.count, pending);
        a.bind(out);
        a.ldp(X21, X22, SP, 32);
        a.ldp(X19, X20, SP, 16);
        a.ldp_post(X29, X30, SP, 48);
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
    JitCompiler jc{*this, a,
                   static_cast<u32>(reinterpret_cast<const u8*>(&instr_count_) - reinterpret_cast<const u8*>(this)),
                   static_cast<u32>(reinterpret_cast<const u8*>(&exit_) - reinterpret_cast<const u8*>(this))};
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
