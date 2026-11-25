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
#include "core/cpu/ppc750/block_cache.h"
#include "core/cpu/ppc750/cpu_state.h"
#include "core/cpu/ppc750/memory_interface.h"
#include "core/fpu/ieee754.h"

namespace g3::ppc {

class JitWorker;

class Ppc750 {
public:
    // The junction temperature the thermal assist unit reports (our choice).
    static constexpr u32 kJunctionCelsius = 40;
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
    void set_external_interrupt(bool asserted)
    {
        external_pending_ = asserted;
        exit_ |= asserted ? 1 : 0;
    }
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
    // Everything cached about translation goes (tlbie, SDR1, reset ...),
    // and with it every translation ID (links and remembered jumps made
    // under one are never taken again).
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
        // Every page change must reach dispatch's page-miss path, which
        // checks the watch: no links to other pages (none are made while
        // it is set, jit_far_link), no remembered jumps.
        if (jit_far_count_)
            jit_unlink_far(~0u);
        jit_jump_drop();
    }
    // Decoded-block cache (JIT stage 1; docs/cpu/block-cache.md). With one
    // set (and ready), run() replays decoded blocks instead of fetching and
    // decoding each instruction; the results are the same instruction for
    // instruction. nullptr: the plain interpreter.
    void set_block_cache(BlockCache* cache)
    {
        cache_ = cache;
        if (cache)
            cache->set_signal(&exit_);
    }
    BlockCache* block_cache() { return cache_; }
    // JIT stage 2 (docs/cpu/jit-a64.md): AArch64 code for each block, in
    // `words` words of executable memory at `code`. `runner` executes a
    // block's code (runner(code, cpu, context)); nullptr calls it directly,
    // which only an AArch64 host can do. False (and no JIT) without a block
    // cache, without room, or with no way to run the code.
    using JitRunner = void (*)(void* code, void* cpu, void* context);
    bool set_jit(u32* code, u32 words, JitRunner runner, void* context);
    struct JitStats {
        u64 compiled = 0, native_ops = 0, helper_ops = 0, resets = 0, runs = 0;
        u64 memory_slow = 0;  // generated loads/stores that took the handler
        u64 queued = 0, queue_full = 0, installed = 0, stale = 0;  // with a worker
        u64 chained = 0;      // blocks entered straight from the previous block's code
        u64 interpreted = 0;  // instructions run_blocks ran without generated code
        u64 code_words = 0;   // AArch64 instructions generated (compiled here)
        u64 retried = 0;      // blocks asked again for code (a request or result was dropped)
        u64 linked = 0;       // direct links patched (block to block, same page)
        u64 linked_far = 0, unlinked_far = 0;  // links to another page made / undone
        u64 xlat_switches = 0, xlat_reused = 0;  // SR/BAT changes; of them, back to a known layout
    };
    // Profile of the JIT's handler calls (docs/cpu/jit-a64.md): generated
    // code counts, per kind of instruction (jit_op_id), the instructions it
    // hands to their handlers, and keeps one instruction word of each kind
    // for naming it. Together with JitStats::interpreted and the
    // instruction count this tells what share runs natively, and which
    // instructions to compile natively next. Costs a few instructions per
    // handler call; counted from set_jit() on.
    static constexpr u32 kJitOpIds = 64 + 4 * 1024;
    // Primary opcode; for opcodes 19, 31, 59 and 63 also the extended
    // opcode (A-form floating point: its 5-bit one).
    static u32 jit_op_id(u32 insn);
    const u64* jit_profile() const { return jit_profile_; }
    u32 jit_profile_sample(u32 id) const { return __atomic_load_n(&jit_sample_[id], __ATOMIC_RELAXED); }
    // Background compilation (jit_worker.h): blocks are compiled by `w`,
    // polled on another core, instead of here. After set_jit(); nullptr
    // compiles here again. Only while the CPU isn't running.
    void set_jit_worker(JitWorker* w);
    const JitStats& jit_stats() const { return jit_stats_; }
    // Diagnostics (g3_host G3_JITHIST): one byte per code-buffer word, set
    // to what each generated instruction is for (JitWordKind); nullptr: off.
    // Compiling here only (no worker). Tags the shared stubs at once.
    enum JitWordKind : u8 {
        kWordSetup = 1, kWordNative, kWordMemFast, kWordMemSlow, kWordHandler, kWordBranch, kWordSync,
        kWordChainHead, kWordStubStatic, kWordStubEntry, kWordStubDispatch, kWordStubEnter, kWordStubEpilogue,
        kWordToDispatchReturn, kWordToDispatchCtr, kWordToDispatchFar, kWordToDispatchOther, kWordKinds
    };
    void set_jit_word_kinds(u8* kinds);
    // Power saving (750 UM 10.2): with MSR[POW] set and a mode selected in
    // HID0 (doze, nap or sleep) the processor stops until an interrupt
    // wakes it. Mac OS's idle loop does this. run() executes nothing while
    // the CPU dozes and no interrupt can be taken (MSR[EE] and an external
    // or decrementer interrupt pending); taking one clears MSR[POW].
    bool dozing() const
    {
        return (s_.msr & msr::POW) && (s_.spr[spr::HID0] & (hid0::DOZE | hid0::NAP | hid0::SLEEP)) &&
               !((s_.msr & msr::EE) && (external_pending_ || dec_pending_));
    }
    // Ends the current run() after the instruction executing now (e.g. after a
    // decrementer write, so the next expiry is planned again).
    void break_run()
    {
        break_run_ = true;
        exit_ = 1;
    }

    // Debugging: called after every instruction run() executes (the block
    // cache is not used while one is set). Checked once per run(): free
    // when unset.
    using StepHook = void (*)(void* context, const CpuState& state);
    void set_step_hook(StepHook h, void* ctx)
    {
        step_hook_ = h;
        step_ctx_ = ctx;
    }

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
                    note_store(pa, Size);
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
    bool tlb_enabled_ = true;
    // The entries filled since the TLBs were last emptied (bit 8: the ITLB),
    // so that emptying them on an SR or BAT switch (about once per 5000
    // instructions in Mac OS) clears those, not all 512; past kTouched,
    // everything is cleared.
    static constexpr u32 kTouched = 48;
    u16 tlb_touched_[kTouched] = {};
    u32 tlb_touched_n_ = 0;

    // --- system (ppc750_system.cpp)
    void op_mfspr(u32 insn);
    u32 thermal_read(u32 thrm) const;  // THRM1/THRM2 as read (thermal assist unit)
    void op_mtspr(u32 insn);
    void op_mftb(u32 insn);
    static bool spr_implemented(u32 n);
    void op_rfi();
    void op_sc();

    // --- block cache (ppc750_blocks.cpp)
    void run_blocks(u64 count);

    BlockCache::Block* build_block(u32 pa, const u8* host_page);
    static bool ends_block(u32 insn);
    static BlockOp::Fn op_for(u32 insn);
    static void op_generic(Ppc750& cpu, u32 insn) { cpu.execute(insn); }
    friend struct BlockOps;  // the specialised operations (ppc750_blocks.cpp)
    // Set by everything after which a block must not go on with its next
    // operation: exceptions, a checkstop, break_run(), an interrupt input
    // becoming pending, blocks being dropped, MSR[POW] set. Cleared at each
    // block start.
    // With the interrupt inputs after it, in one aligned word that a
    // block's end reads at once (zero: none of them; JitCompiler::chain).
    alignas(4) u8 exit_ = 0;
    bool external_pending_ = false;
    bool dec_pending_ = false;
    u8 stop_pad_ = 0;  // always 0
    u32 fetch_pa_page_ = 0;  // physical page of fetch_host_
    void note_store(u32 pa, u32 len)
    {
        if (cache_)
            cache_->note_write(pa, len);
    }

    MemoryInterface& mem_;
    BlockCache* cache_ = nullptr;
    StepHook step_hook_ = nullptr;
    void* step_ctx_ = nullptr;
    // --- JIT (ppc750_jit.cpp)
    bool jit_compile(BlockCache::Block* b);
    // The code shared by all blocks (between-blocks lookup, entry, epilogue)
    // at the start of the code buffer; blocks follow from jit_stub_words_.
    bool jit_emit_stubs();
    u32 jit_stub_words_ = 0, jit_stub_dispatch_ = 0, jit_stub_enter_ = 0, jit_stub_epilogue_ = 0,
        jit_stub_static_ = 0, jit_stub_entry_ = 0;
    const u32* jit_entry_ = nullptr;     // the block the entry stub jumps to (jit_run)
    const u8* jit_code_pages_ = nullptr; // the cache's per-page code flags (fixed after set_jit)
    const void* jit_dtlb_ = nullptr;     // &dtlb_[0]
    u8* jit_word_kinds_ = nullptr;
    // Direct links (ppc750_jit.cpp): the slot to patch and the block it
    // jumps to, handed from generated code to jit_link.
    u32* jit_link_slot_ = nullptr;
    BlockCache::Block* jit_link_block_ = nullptr;
    static void jit_link(void* cpu, u32 unused);
    // Links to blocks in other pages (ppc750_jit.cpp).
    static constexpr u32 kMaxFarLinks = 16384;
    struct FarLink {
        u32* mode;   // the slot's movz (nullptr: a free record)
        u32 page;    // the target's physical page
        u32 branch;  // the branch is mode[branch] (4: far b/bc; 8: bclr/bcctr)
        u32 prev, next;  // the chain of the page's bucket; next: also the free list
    };
    // Translation IDs (docs/cpu/jit-a64.md): one per layout of the segment
    // registers and BATs seen since the last flush_tlb, so that switching
    // back to a layout finds its far links and remembered jumps still
    // good, as the 750's VSID-tagged TLB keeps its entries. Generated code
    // compares xlat_id_ << 16 | MSR[IR|PR] with the key a link was made
    // under.
    u32 xlat_id_ = 1, xlat_next_ = 2;
    struct XlatLayout {
        u32 hash = 0, id = 0;  // id 0: empty
        u32 regs[32] = {};     // SR0-15, IBAT0U-DBAT3L
    };
    static constexpr u32 kXlatLayouts = 64;
    u32 xlat_victim_ = 0;
    void drop_translations();   // the TLBs, the fetch page, translated fast entries
    void switch_translation();  // an SR or BAT changed: the layout's ID
    void new_translation_id();
    u32 current_layout(u32 (&regs)[32]) const;
    void remember_layout();
    u32* jit_far_slot_ = nullptr;  // asked for by dispatch_link, made by enter
    u32 jit_far_count_ = 0;
    u32 jit_stub_dispatch_link_ = 0;
    static void jit_far_link(void* cpu, u32 unused);
    void jit_unlink_far(u32 page);
    // The records stay where they are (free ones on a list) and are
    // chained per bucket of target pages, so undoing the links into one
    // page (a store into code) walks that page's few, not all of them.
    // Which record holds a slot: open addressing on the slot's address
    // (value record + 1, 0 empty), rebuilt when three quarters full; an
    // entry is good only if its record still holds that slot. A slot that
    // is linked has exactly one record: a relink under another key
    // updates it.
    static constexpr u32 kFarNone = ~0u;
    static constexpr u32 kFarBuckets = 4096;
    static constexpr u32 kFarIndex = 2 * kMaxFarLinks;
    u32 far_top_ = 0, far_free_ = kFarNone, far_index_used_ = 0;
    u32 far_find(const u32* mode) const;
    void far_index_add(u32 record);
    void far_index_rebuild();
    u32 far_alloc();
    void far_attach(u32 record);
    void far_detach(u32 record);
    void far_release(u32 record);  // undoes its link, frees it
    void far_reset();              // no records, no index
    static void jit_cache_invalidated(void* cpu, u32 page);
    static void jit_cache_flushed(void* cpu);
    // The fast data table (ppc750_jit.cpp, docs/cpu/jit-a64.md): what
    // generated loads and stores look up. One table of kFastSize entries per
    // MSR[DR, PR] combination, indexed by the effective page's low bits. A
    // hit means the whole access lies in that page and in the RAM window;
    // for stores also that the page may be written (real mode, or a TLB
    // entry with write permission and its C bit set) and holds no blocks.
    // Filled by jit_fill from what the soft TLB already knows; emptied with
    // it (flush_tlb), when a page gets blocks (writes only) and when the RAM
    // window moves.
    struct FastEntry {
        u32 read = kFastNone;   // the effective page number, or kFastNone
        u32 write = kFastNone;
        u64 addend = 0;         // host address = addend + effective address
    };
    static constexpr u32 kFastNone = 0xFFFFFFFFu;  // no page number (they have 20 bits)
    static constexpr u32 kFastSize = 256;
    void* jit_fast_ptr_ = nullptr;  // &jit_fast_[0]
    static void jit_fill(void* cpu, u32 ea);
    void jit_fast_drop(bool translated_only);
    void jit_fast_drop_writes(u32 page);
    static void jit_cache_marked(void* cpu, u32 page);
    // The jump cache: dispatch's memory of where a jump landed. Entry
    // (EA >> 2) & (kJumpSize - 1) holds key = EA | MSR[IR|PR] << 32 and
    // the block found for it then. Taken again while that block's page
    // keeps its generation; emptied when the translation changes
    // (flush_tlb), the cache is flushed and a code watch is set. Not
    // filled while a code watch is set (dispatch's page change checks it).
    struct JumpEntry {
        u64 key = 1;    // 1: no PC (they are word aligned)
        u64 block = 0;  // BlockCache::Block*
    };
    static constexpr u32 kJumpSize = 1024;
    void* jit_jump_ptr_ = nullptr;  // &jit_jump_[0]
    u8 jump_used_ = 0;              // set by dispatch when it fills one
    void jit_jump_drop();
    // Generates b's code at `at` (room words); words used in `size`. Reads
    // nothing of the CPU that changes while it runs (callable from the worker).
    bool jit_generate(BlockCache::Block& b, u32* at, u32 room, u32& size, JitStats& st);
    void jit_request(BlockCache::Block* b);
    void jit_collect();
    JitWorker* jit_worker_ = nullptr;
    friend class JitWorker;
    void jit_run(BlockCache::Block* b);
    u32* jit_code_ = nullptr;
    u32 jit_words_ = 0, jit_used_ = 0;
    u64 jit_epoch_ = 0;          // cache flush count the code buffer belongs to
    bool jit_full_ = false;      // out of code space: flush everything at the next build
    JitRunner jit_runner_ = nullptr;
    void* jit_ctx_ = nullptr;
    JitStats jit_stats_;
    // run_blocks' budget as an instruction count: generated code goes on
    // to the next block only if it fits (as run_blocks itself decides).
    u64 jit_limit_ = 0;
    // A block still without code this many instructions after it was asked
    // for is asked again (with a worker; queues may drop requests).
    static constexpr u32 kJitRetry = 1u << 20;
    // The block cache's lookup data, for generated code (fixed after set_jit).
    const u32* jit_table_ = nullptr;
    const u8* jit_pool_ = nullptr;
    const u32* jit_gen_ = nullptr;
    friend struct JitCompiler;
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
    u32 pvr_;
    u32 next_pc_ = 0;        // set by execute(); exceptions/branches overwrite
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
    // The large members last, so that the small ones generated code reads
    // between blocks (exit_, the fetch page, the interrupt inputs, the
    // instruction count ...) sit within a single load's reach of the start
    // of the object. Generated code reaches these through base registers.
    CpuState s_;
    TlbEntry itlb_[kTlbSize];
    TlbEntry dtlb_[kTlbSize];
    FarLink jit_far_links_[kMaxFarLinks] = {};
    u16 jit_far_index_[kFarIndex] = {};
    u32 far_bucket_[kFarBuckets] = {};
    XlatLayout xlat_layouts_[kXlatLayouts];
    JumpEntry jit_jump_[kJumpSize];
    FastEntry jit_fast_[4 * kFastSize];  // [MSR[PR] * 2 + MSR[DR]][page & 255]
    u8* fast_ram_ = nullptr;             // the RAM window jit_fast_ was filled for
    u32 fast_ram_size_ = 0;
    u32 fast_used_ = 0;                  // bit t: table t may hold entries
    u16 fast_touched_[kTouched] = {};    // translated entries filled (table * 256 + index)
    u32 fast_touched_n_ = 0;             // past kTouched: clear the tables whole
    // The JIT profile.
    u64 jit_profile_[kJitOpIds] = {};
    u32 jit_sample_[kJitOpIds] = {};  // written by whoever compiles (relaxed atomics)
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
