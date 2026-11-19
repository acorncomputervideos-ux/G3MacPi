// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/cpu/ppc750/ppc750.h — PowerPC 750 (G3) instruction interpreter.
//
// Integer, compare, logical, rotate/shift, condition register, branch,
// load/store, trap, system call, rfi, SPR/MSR/SR access, exceptions
// (Milestone 2) and the floating-point unit (Milestone 2b, software IEEE
// arithmetic in core/fpu) and the MMU (Milestone 3).
//
// Address translation (BAT + segments + hashed page table, R/C updates,
// DSI/ISI) is done for every access (no TLB cache yet).
//
// Not supported yet (documented in docs/hardware/ppc750-cpu.md):
//  * little-endian mode (MSR[LE]); eciwx/ecowx; trace exceptions;
//    alignment exceptions other than lwarx/stwcx.; performance monitor.

#ifndef G3_CORE_CPU_PPC750_PPC750_H
#define G3_CORE_CPU_PPC750_PPC750_H

#include "core/common/endian.h"
#include "core/common/types.h"
#include "core/cpu/ppc750/cpu_state.h"
#include "core/cpu/ppc750/memory_interface.h"
#include "core/fpu/ieee754.h"

namespace g3::ppc {

class Ppc750 {
public:
    explicit Ppc750(MemoryInterface& memory, u32 pvr = kPvrMpc750Placeholder);

    // Hard reset (750 UM 2.1.4): MSR = IP, PC = 0xFFF00100, other state 0.
    void reset();

    // Fetches and executes one instruction at state().pc, then delivers a
    // pending external/decrementer interrupt if enabled.
    void step();

    // Executes 'count' instructions (fewer if the CPU checkstops).
    void run(u64 count);

    // Executes one already-fetched instruction as if it were at state().pc.
    // Used by tests and (later) by the debugger.
    void execute(u32 insn);

    // Emulator traps for high-level firmware (HLE). Primary opcode 0 is
    // illegal on the 750; when such a word is executed, the handler is asked
    // first. It returns true if it claimed the word (it may change state
    // and set next_pc, which defaults to the following instruction);
    // otherwise the usual program exception is taken. The handler must
    // claim only its own trap words: 0x00000000 from zeroed memory, for
    // example, must still fault.
    using HleHandler = bool (*)(void* context, u32 insn, CpuState& state, u32& next_pc);
    void set_hle_handler(HleHandler handler, void* context)
    {
        hle_handler_ = handler;
        hle_ctx_ = context;
    }

    // Interrupt inputs.
    void set_external_interrupt(bool asserted) { external_pending_ = asserted; }
    // Advances the time base and decrementer by 'ticks' time-base ticks.
    // A decrementer exception becomes pending when DEC bit 0 changes 0 -> 1.
    void advance_timebase(u32 ticks);

    // How FP underflow tininess is detected (IEEE 754 allows either). The
    // vectors in tests/vectors/dingusppc decide the default; see
    // docs/hardware/ppc750-cpu.md.
    void set_fp_tininess(fp::Tininess t) { fp_tininess_ = t; }

    // Soft TLB control (on by default). Disabling it gives the reference
    // (uncached) translation path used by the differential tests.
    void set_tlb_enabled(bool enabled)
    {
        tlb_enabled_ = enabled;
        flush_tlb();
    }
    void flush_tlb();

    CpuState& state() { return s_; }
    const CpuState& state() const { return s_; }

    // Checkstop: a machine check occurred with MSR[ME] = 0. The CPU stops.
    bool checkstopped() const { return checkstop_; }
    u64 instructions_executed() const { return instr_count_; }

    // Translates a data address as a load (or store) would right now —
    // MSR[DR], BATs, page table — without raising an exception, updating
    // R/C bits or touching the TLB. For firmware that reads and writes
    // client memory on the client's behalf. False if no translation.
    bool probe_translate(u32 ea, bool write, u32& pa);

    // Called just before the time base or the decrementer is read or the
    // decrementer is written, so whoever drives time (the execution loop) can
    // bring them up to the current instruction; null = none.
    using TimeHook = void (*)(void* context);
    void set_time_hook(TimeHook h, void* ctx)
    {
        time_hook_ = h;
        time_ctx_ = ctx;
    }
    // Diagnostics: called when instruction fetch enters a 4 KB page that
    // overlaps the effective range [lo, hi] (pc = the instruction about to
    // run). Checked only when the fetch-page cache misses, so it reports
    // page entries, not every instruction; it costs nothing on cache hits.
    using CodeWatch = void (*)(void* context, u32 pc);
    void set_code_watch(u32 lo, u32 hi, CodeWatch cb, void* ctx)
    {
        watch_lo_ = lo & ~0xFFFu;
        watch_hi_ = hi;
        watch_cb_ = cb;
        watch_ctx_ = ctx;
        fetch_host_ = nullptr;
    }
    // Ends the current run() after the instruction executing now (e.g. after a
    // decrementer write, so the next expiry is planned again).
    void break_run() { break_run_ = true; }

    // Observer told of every exception taken (debugging tools); null = none.
    using ExceptionHook = void (*)(void* context, u32 vector, u32 srr0, u32 srr1);
    void set_exception_hook(ExceptionHook h, void* ctx)
    {
        exception_hook_ = h;
        exception_ctx_ = ctx;
    }

    // Exception entry (public for tests and for device-driven exceptions).
    // 'srr0' is the address saved; 'srr1_cause' are the cause bits
    // OR-ed into SRR1 next to the copied MSR bits.
    void raise_exception(Exception type, u32 srr0, u32 srr1_cause = 0);

private:
    // --- dispatch (ppc750.cpp)
    void execute_op19(u32 insn);
    void execute_op31(u32 insn);
    void illegal(u32 insn);
    void privileged();
    bool in_user_mode() const { return (s_.msr & msr::PR) != 0; }

    // --- integer (ppc750_integer.cpp)
    void update_cr0(u32 result);
    void set_ov(bool overflow);
    void set_ca(bool carry) { s_.xer() = carry ? (s_.xer() | xer::CA) : (s_.xer() & ~xer::CA); }
    bool ca() const { return (s_.xer() & xer::CA) != 0; }
    void op_add_family(u32 insn, u32 xo);
    void op_mul_div(u32 insn, u32 xo);
    void op_logical(u32 insn, u32 xo);
    void op_shift(u32 insn, u32 xo);
    void op_compare(u32 crf, u32 a, u32 b, bool is_signed);
    void op_rotate(u32 insn, u32 opcode);
    void op_trap(u32 to, u32 a, u32 b);

    // --- branch and condition register (ppc750_branch.cpp)
    void op_b(u32 insn);
    void op_bc(u32 insn);
    void op_bclr_bcctr(u32 insn, bool to_ctr);
    void op_cr_logical(u32 insn, u32 xo);
    bool cr_bit(u32 bit) const { return (s_.cr >> (31u - bit)) & 1u; }
    void set_cr_bit(u32 bit, bool value);
    void set_cr_field(u32 field, u32 value);

    // --- load/store (ppc750_loadstore.cpp)
    u32 ea_d(u32 insn) const;   // (rA|0) + EXTS(d)
    u32 ea_x(u32 insn) const;   // (rA|0) + rB
    bool load_store_d(u32 insn, u32 opcode);
    bool load_store_x(u32 insn, u32 xo);
    bool op_lmw_stmw(u32 insn, bool store);
    bool op_string(u32 insn, bool store, u32 nbytes, u32 ea);
    bool op_lwarx(u32 insn);
    bool op_stwcx(u32 insn);
    bool op_dcbz(u32 insn);
    // Memory helpers: translate, then access. On failure they raise DSI or a
    // machine check and return false.
    bool mem_access(u32 ea, u32 size, bool write, u32& value);
    // The common case inline: the access stays in one page, translates
    // through real mode or a soft-TLB hit, and lands in the direct RAM
    // window. Anything else goes to mem_access.
    template <u32 Size, bool Write>
    bool mem_fast(u32 ea, u32& value)
    {
        if ((ea & 0xFFFu) + Size <= 0x1000u) {
            u32 pa = ea;
            if (s_.msr & msr::DR) {
                const u32 page = ea >> 12;
                const u32 tag = kTlbValid | (page << 1) | ((s_.msr & msr::PR) ? 1u : 0u);
                const TlbEntry& e = dtlb_[page & (kTlbSize - 1)];
                if (!tlb_enabled_ || e.tag != tag || (Write && !(e.writable && e.changed)))
                    return mem_access(ea, Size, Write, value);
                pa = e.pa_page | (ea & 0xFFFu);
            }
            if (u8* p = ram_at(pa, Size)) {
                if (Write) {
                    if (Size == 4) store_be32(p, value);
                    else if (Size == 2) store_be16(p, static_cast<u16>(value));
                    else *p = static_cast<u8>(value);
                } else {
                    value = Size == 4 ? load_be32(p) : Size == 2 ? load_be16(p) : *p;
                }
                return true;
            }
        }
        return mem_access(ea, Size, Write, value);
    }
    bool mem_read8(u32 a, u32& v) { return mem_fast<1, false>(a, v); }
    bool mem_read16(u32 a, u32& v) { return mem_fast<2, false>(a, v); }
    bool mem_read32(u32 a, u32& v) { return mem_fast<4, false>(a, v); }
    bool mem_write8(u32 a, u32 v) { return mem_fast<1, true>(a, v); }
    bool mem_write16(u32 a, u32 v) { return mem_fast<2, true>(a, v); }
    bool mem_write32(u32 a, u32 v) { return mem_fast<4, true>(a, v); }
    void machine_check();

    // --- floating point (ppc750_fpu.cpp)
    bool fp_available();                 // raises FP-unavailable if MSR[FP] = 0
    void execute_op59(u32 insn);
    void execute_op63(u32 insn);
    void fp_arith(u32 insn, bool single);
    void fp_compare(u32 insn, bool ordered);
    void fp_round_to_single(u32 insn);
    void fp_convert_to_int(u32 insn, bool toward_zero);
    void fp_estimate(u32 insn, bool reciprocal_sqrt);
    void fp_move(u32 insn, u32 xo);
    void fp_fpscr_op(u32 insn, u32 xo);
    // Applies an arithmetic result to frD and the FPSCR (PEM 3.3.6).
    void fp_commit(u32 insn, u64 result, bool is_nan_result, bool vxsnan, u32 other_vx,
                   const fp::Flags& flags, bool single, bool negate);
    void fp_update_summary(u32 old_fpscr);   // FX, VX, FEX
    void fp_finish(u32 insn);                // CR1 (Rc) + enabled-exception check
    bool fp_load_store_d(u32 insn, u32 opcode);
    bool fp_load_store_x(u32 insn, u32 xo);
    bool mem_read64(u32 a, u64& v);
    bool mem_write64(u32 a, u64 v);

    // --- MMU (ppc750_mmu.cpp)
    enum class Access { Read, Write, Fetch };
    // Translates an effective address. On failure raises DSI/ISI and
    // returns false. Real mode (MSR[IR]/[DR] = 0) returns pa = ea; that
    // check is inline because it is on every fetch and data access.
    bool translate(u32 ea, Access access, u32& pa)
    {
        const u32 bit = access == Access::Fetch ? msr::IR : msr::DR;
        if (!(s_.msr & bit)) {
            pa = ea;
            return true;
        }
        // Soft-TLB hit, inline (the common case); everything else goes to
        // translate_enabled, which repeats the lookup.
        if (tlb_enabled_) {
            const u32 page = ea >> 12;
            const u32 tag = kTlbValid | (page << 1) | ((s_.msr & msr::PR) ? 1u : 0u);
            const TlbEntry& e = (access == Access::Fetch ? itlb_ : dtlb_)[page & (kTlbSize - 1)];
            if (e.tag == tag && (access != Access::Write || (e.writable && e.changed))) {
                pa = e.pa_page | (ea & 0xFFFu);
                return true;
            }
        }
        return translate_enabled(ea, access, pa);
    }
    // Host pointer to [pa, pa + len) if it lies in the bus's direct RAM
    // window (MemoryInterface::direct_window), else nullptr.
    u8* ram_at(u32 pa, u32 len) const
    {
        const u32 off = pa - ram_base_;
        return (ram_ && off < ram_size_ && ram_size_ - off >= len) ? ram_ + off : nullptr;
    }
    bool translate_enabled(u32 ea, Access access, u32& pa);
    // Uncached translation. On success also reports whether the page/block
    // may be written and whether its C bit is already set (BATs: always).
    bool translate_slow(u32 ea, Access access, u32& pa, bool& writable, bool& changed);
    bool bat_translate(u32 ea, Access access, u32& pa, bool& protection_fault, bool& writable);
    // probe: no R/C updates and no machine check (for probe_translate).
    bool page_translate(u32 ea, Access access, u32& pa, u32& fault, bool& writable,
                        bool& changed, bool probe = false);
    void raise_translation_fault(u32 ea, Access access, u32 cause);

    // Soft TLB: direct-mapped caches of successful translations, one for
    // instruction fetches and one for data. Same invalidation rules as the
    // hardware TLB (tlbie) plus flushes on SR/SDR1/BAT writes.
    struct TlbEntry {
        u32 tag = 0;        // kTlbValid | page << 1 | MSR[PR]
        u32 pa_page = 0;
        bool writable = false;
        bool changed = false;
    };
    static constexpr u32 kTlbSize = 256;
    static constexpr u32 kTlbValid = 0x80000000u;
    TlbEntry itlb_[kTlbSize];
    TlbEntry dtlb_[kTlbSize];
    bool tlb_enabled_ = true;

    // --- system (ppc750_system.cpp)
    void op_mfspr(u32 insn);
    void op_mtspr(u32 insn);
    void op_mftb(u32 insn);
    static bool spr_implemented(u32 n);
    void op_rfi();
    void op_sc();

    MemoryInterface& mem_;
    // Direct RAM window, refreshed at the start of run().
    u8* ram_ = nullptr;
    u32 ram_base_ = 0, ram_size_ = 0;
    // Fetch-page cache: host pointer to the RAM page holding the current
    // instruction stream, valid for fetch_page_ (effective) under
    // fetch_msr_ (MSR[IR|PR]). Dropped by flush_tlb() and at run() start.
    const u8* fetch_host_ = nullptr;
    u32 fetch_page_ = 0, fetch_msr_ = 0;
    u32 watch_lo_ = 0, watch_hi_ = 0;
    CodeWatch watch_cb_ = nullptr;
    void* watch_ctx_ = nullptr;
    CpuState s_;
    u32 pvr_;
    u32 next_pc_ = 0;        // set by execute(); exceptions/branches overwrite
    bool external_pending_ = false;
    bool dec_pending_ = false;
    bool checkstop_ = false;
    fp::Tininess fp_tininess_ = fp::Tininess::BeforeRounding;
    u64 instr_count_ = 0;
    HleHandler hle_handler_ = nullptr;
    ExceptionHook exception_hook_ = nullptr;
    TimeHook time_hook_ = nullptr;
    void* time_ctx_ = nullptr;
    bool break_run_ = false;
    void* exception_ctx_ = nullptr;
    void* hle_ctx_ = nullptr;
};

// Instruction field helpers (PowerPC bit numbering, PEM Appendix A).
namespace field {
constexpr u32 opcd(u32 i) { return i >> 26; }
constexpr u32 rd(u32 i) { return (i >> 21) & 31; }    // also rS, TO, BO, crbD
constexpr u32 ra(u32 i) { return (i >> 16) & 31; }    // also BI, crbA
constexpr u32 rb(u32 i) { return (i >> 11) & 31; }    // also SH, crbB, NB
constexpr u32 mb(u32 i) { return (i >> 6) & 31; }
constexpr u32 me(u32 i) { return (i >> 1) & 31; }
constexpr u32 xo10(u32 i) { return (i >> 1) & 0x3FF; }  // X/XL/XFX-form extended opcode
constexpr u32 xo9(u32 i) { return (i >> 1) & 0x1FF; }   // XO-form (without OE)
constexpr bool rc(u32 i) { return (i & 1u) != 0; }
constexpr bool oe(u32 i) { return (i & 0x400u) != 0; }
constexpr bool lk(u32 i) { return (i & 1u) != 0; }
constexpr bool aa(u32 i) { return (i & 2u) != 0; }
constexpr u32 simm(u32 i) { return static_cast<u32>(static_cast<s32>(static_cast<s16>(i & 0xFFFF))); }
constexpr u32 uimm(u32 i) { return i & 0xFFFF; }
constexpr u32 crfd(u32 i) { return (i >> 23) & 7; }
constexpr u32 crfs(u32 i) { return (i >> 18) & 7; }
} // namespace field

} // namespace g3::ppc

#endif // G3_CORE_CPU_PPC750_PPC750_H
