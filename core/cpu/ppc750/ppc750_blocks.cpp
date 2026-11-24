// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/cpu/ppc750/ppc750_blocks.cpp — running code from the decoded-block
// cache (block_cache.h, docs/cpu/block-cache.md).
//
// The rule: everything observable is exactly what step() would do, one
// instruction at a time. Interrupts are taken at the same instruction
// boundaries, instructions are counted the same way, fetch translation
// happens under the same conditions (the code watch included), and code
// written at any time is never run from a stale decode.

#include "core/cpu/ppc750/ppc750.h"
#include "core/cpu/ppc750/jit_worker.h"
#include "core/common/bitops.h"

namespace g3::ppc {

bool Ppc750::ends_block(u32 insn)
{
    // Instructions after which the next instruction's address, the MSR or
    // the address translation may change, plus everything that is not a
    // plain computation (HLE traps, illegal opcodes). Ordinary exceptions
    // (DSI, alignment, FP unavailable ...) need no entry here: they are
    // caught as a change of PC or MSR after the instruction.
    switch (field::opcd(insn)) {
    case 0: case 3: case 16: case 17: case 18:  // HLE/illegal, twi, bc, sc, b
        return true;
    case 19:
        switch (field::xo10(insn)) {
        case 0:                                   // mcrf
        case 33: case 129: case 193: case 225:    // crnor crandc crxor crnand
        case 257: case 289: case 417: case 449:   // crand creqv crorc cror
            return false;
        default:                                  // bclr bcctr rfi isync, illegal
            return true;
        }
    case 31:
        switch (field::xo10(insn)) {
        case 4:    // tw
        case 146:  // mtmsr
        case 210:  // mtsr
        case 242:  // mtsrin
        case 306:  // tlbie
        case 370:  // tlbia
        case 467:  // mtspr (SDR1, BATs, HID0, DEC ...)
        case 566:  // tlbsync
        case 598:  // sync
        case 982:  // icbi
            return true;
        default:
            return false;
        }
    case 1: case 2: case 4: case 5: case 6: case 9: case 22: case 30:
    case 56: case 57: case 58: case 60: case 61: case 62:  // illegal on the 750
        return true;
    default:
        return false;
    }
}

// Specialised operations for the most common instructions: the same
// semantics as execute() (which the tests compare them with), without its
// decoding switches. Each one does what execute() does around the
// instruction: next_pc_, the instruction count, and the PC afterwards.
struct BlockOps {
    static void begin(Ppc750& c)
    {
        c.next_pc_ = c.s_.pc + 4;
        ++c.instr_count_;
    }
    static void end(Ppc750& c) { c.s_.pc = c.next_pc_; }
    // Instructions that cannot raise an exception.
    static void simple_end(Ppc750& c) { c.s_.pc += 4; }

    static void addi(Ppc750& c, u32 insn)
    {
        ++c.instr_count_;
        const u32 ra = field::ra(insn);
        c.s_.gpr[field::rd(insn)] = (ra ? c.s_.gpr[ra] : 0) + field::simm(insn);
        simple_end(c);
    }
    static void addis(Ppc750& c, u32 insn)
    {
        ++c.instr_count_;
        const u32 ra = field::ra(insn);
        c.s_.gpr[field::rd(insn)] = (ra ? c.s_.gpr[ra] : 0) + (field::simm(insn) << 16);
        simple_end(c);
    }
    static void ori(Ppc750& c, u32 insn)
    {
        ++c.instr_count_;
        c.s_.gpr[field::ra(insn)] = c.s_.gpr[field::rd(insn)] | field::uimm(insn);
        simple_end(c);
    }
    static void oris(Ppc750& c, u32 insn)
    {
        ++c.instr_count_;
        c.s_.gpr[field::ra(insn)] = c.s_.gpr[field::rd(insn)] | (field::uimm(insn) << 16);
        simple_end(c);
    }
    static void andi_rc(Ppc750& c, u32 insn)
    {
        ++c.instr_count_;
        const u32 r = c.s_.gpr[field::rd(insn)] & field::uimm(insn);
        c.s_.gpr[field::ra(insn)] = r;
        c.update_cr0(r);
        simple_end(c);
    }
    static void rlwinm(Ppc750& c, u32 insn)
    {
        ++c.instr_count_;
        const u32 r = rotl32(c.s_.gpr[field::rd(insn)], field::rb(insn)) & ppc_mask(field::mb(insn), field::me(insn));
        c.s_.gpr[field::ra(insn)] = r;
        if (field::rc(insn))
            c.update_cr0(r);
        simple_end(c);
    }
    static void cmpi(Ppc750& c, u32 insn)
    {
        ++c.instr_count_;
        c.op_compare(field::crfd(insn), c.s_.gpr[field::ra(insn)], field::simm(insn), true);
        simple_end(c);
    }
    static void cmpli(Ppc750& c, u32 insn)
    {
        ++c.instr_count_;
        c.op_compare(field::crfd(insn), c.s_.gpr[field::ra(insn)], field::uimm(insn), false);
        simple_end(c);
    }
    // or rA,rS,rB (mr when rS = rB), add, subf, and: no OE, no Rc.
    static void or_(Ppc750& c, u32 insn)
    {
        ++c.instr_count_;
        c.s_.gpr[field::ra(insn)] = c.s_.gpr[field::rd(insn)] | c.s_.gpr[field::rb(insn)];
        simple_end(c);
    }
    static void and_(Ppc750& c, u32 insn)
    {
        ++c.instr_count_;
        c.s_.gpr[field::ra(insn)] = c.s_.gpr[field::rd(insn)] & c.s_.gpr[field::rb(insn)];
        simple_end(c);
    }
    static void add(Ppc750& c, u32 insn)
    {
        ++c.instr_count_;
        c.s_.gpr[field::rd(insn)] = c.s_.gpr[field::ra(insn)] + c.s_.gpr[field::rb(insn)];
        simple_end(c);
    }
    static void subf(Ppc750& c, u32 insn)
    {
        ++c.instr_count_;
        c.s_.gpr[field::rd(insn)] = c.s_.gpr[field::rb(insn)] - c.s_.gpr[field::ra(insn)];
        simple_end(c);
    }
    static void cmp(Ppc750& c, u32 insn)
    {
        ++c.instr_count_;
        c.op_compare(field::crfd(insn), c.s_.gpr[field::ra(insn)], c.s_.gpr[field::rb(insn)], true);
        simple_end(c);
    }
    static void cmpl(Ppc750& c, u32 insn)
    {
        ++c.instr_count_;
        c.op_compare(field::crfd(insn), c.s_.gpr[field::ra(insn)], c.s_.gpr[field::rb(insn)], false);
        simple_end(c);
    }
    static void mflr(Ppc750& c, u32 insn)
    {
        ++c.instr_count_;
        c.s_.gpr[field::rd(insn)] = c.s_.lr();
        simple_end(c);
    }
    // D-form loads and stores (may raise DSI, alignment or machine check).
    static u32 ea(const Ppc750& c, u32 insn)
    {
        const u32 ra = field::ra(insn);
        return (ra ? c.s_.gpr[ra] : 0) + field::simm(insn);
    }
    template <u32 Size, bool Update>
    static void load(Ppc750& c, u32 insn)
    {
        begin(c);
        const u32 a = ea(c, insn);
        u32 v = 0;
        if (c.mem_fast<Size, false>(a, v)) {
            c.s_.gpr[field::rd(insn)] = v;
            if (Update)
                c.s_.gpr[field::ra(insn)] = a;
        }
        end(c);
    }
    static void lha(Ppc750& c, u32 insn)
    {
        begin(c);
        u32 v = 0;
        if (c.mem_fast<2, false>(ea(c, insn), v))
            c.s_.gpr[field::rd(insn)] = static_cast<u32>(sign_extend16(v));
        end(c);
    }
    template <u32 Size, bool Update>
    static void store(Ppc750& c, u32 insn)
    {
        begin(c);
        const u32 a = ea(c, insn);
        u32 v = c.s_.gpr[field::rd(insn)];
        if (c.mem_fast<Size, true>(a, v) && Update)
            c.s_.gpr[field::ra(insn)] = a;
        end(c);
    }
};

BlockOp::Fn Ppc750::op_for(u32 insn)
{
    switch (field::opcd(insn)) {
    case 10: return &BlockOps::cmpli;
    case 11: return &BlockOps::cmpi;
    case 14: return &BlockOps::addi;
    case 15: return &BlockOps::addis;
    case 21: return &BlockOps::rlwinm;
    case 24: return &BlockOps::ori;
    case 25: return &BlockOps::oris;
    case 28: return &BlockOps::andi_rc;
    case 32: return &BlockOps::load<4, false>;   // lwz
    case 33: return &BlockOps::load<4, true>;    // lwzu
    case 34: return &BlockOps::load<1, false>;   // lbz
    case 35: return &BlockOps::load<1, true>;    // lbzu
    case 36: return &BlockOps::store<4, false>;  // stw
    case 37: return &BlockOps::store<4, true>;   // stwu
    case 38: return &BlockOps::store<1, false>;  // stb
    case 39: return &BlockOps::store<1, true>;   // stbu
    case 40: return &BlockOps::load<2, false>;   // lhz
    case 41: return &BlockOps::load<2, true>;    // lhzu
    case 42: return &BlockOps::lha;              // lha
    case 44: return &BlockOps::store<2, false>;  // sth
    case 45: return &BlockOps::store<2, true>;   // sthu
    case 31:
        // Only the forms without OE and Rc (bit 21, bit 31), bit 0 clear.
        if (insn & 0x00000401u)
            return &Ppc750::op_generic;
        switch (field::xo10(insn)) {
        case 0:   return (insn & 0x00600000u) ? &Ppc750::op_generic : &BlockOps::cmp;   // L = 0, bit 9 = 0
        case 32:  return (insn & 0x00600000u) ? &Ppc750::op_generic : &BlockOps::cmpl;
        case 28:  return &BlockOps::and_;
        case 40:  return &BlockOps::subf;
        case 266: return &BlockOps::add;
        case 444: return &BlockOps::or_;
        case 339: return (((insn >> 16) & 31) | ((insn >> 6) & 0x3E0)) == spr::LR ? &BlockOps::mflr : &Ppc750::op_generic;
        default:  return &Ppc750::op_generic;
        }
    default:
        return &Ppc750::op_generic;
    }
}

BlockCache::Block* Ppc750::build_block(u32 pa, const u8* host_page)
{
    if (jit_full_) {
        // Out of code space: start again with an empty cache (the code
        // buffer follows the cache's flushes, see jit_compile).
        jit_full_ = false;
        cache_->flush();
    }
    // Length first (up to a block-ending instruction, the page end or
    // kMaxOps), then the block, exactly that long.
    u32 n = 0;
    for (u32 off = pa & 0xFFFu; off < 0x1000u && n < BlockCache::kMaxOps; off += 4) {
        ++n;
        if (ends_block(load_be32(host_page + off)))
            break;
    }
    BlockCache::Block* b = cache_->allocate(pa, n);
    for (u32 i = 0; i < n; ++i) {
        const u32 insn = load_be32(host_page + (pa & 0xFFFu) + 4 * i);
        b->ops[i] = BlockOp{op_for(insn), insn};
    }
    b->count = n;
    cache_->commit(b);
    if (jit_worker_)
        jit_request(b);  // compiled on another core; runs through its handlers until then
    else if (jit_code_ && !jit_compile(b))
        jit_full_ = true;
    return b;
}

void Ppc750::run_blocks(u64 count)
{
    u64 done = 0;
    while (done < count && !checkstop_ && !break_run_) {
        // Pending interrupts, the fetch from outside RAM and the fetch
        // faults: one step() each, exactly as the interpreter.
        if ((s_.msr & msr::EE) && (external_pending_ || dec_pending_)) {
            step();
            ++done;
            continue;
        }
        const u32 fetch_msr = s_.msr & (msr::IR | msr::PR);
        if (!(fetch_host_ && (s_.pc & ~0xFFFu) == fetch_page_ && fetch_msr == fetch_msr_)) {
            // step()'s fetch-page miss: the code watch and translation.
            if (watch_cb_ && s_.pc >= watch_lo_ && s_.pc <= watch_hi_)
                watch_cb_(watch_ctx_, s_.pc);
            u32 pa;
            if (!translate(s_.pc, Access::Fetch, pa)) {  // raises ISI
                s_.pc = next_pc_;
                ++done;
                continue;
            }
            const u8* page = ram_at(pa & ~0xFFFu, 0x1000);
            if (!page) {
                u32 insn;
                if (!mem_.fetch32(pa, insn)) {
                    machine_check();
                    s_.pc = next_pc_;
                } else {
                    execute(insn);
                }
                ++done;
                continue;
            }
            fetch_host_ = page;
            fetch_page_ = s_.pc & ~0xFFFu;
            fetch_pa_page_ = pa & ~0xFFFu;
            fetch_msr_ = fetch_msr;
        }
        if (jit_worker_)
            jit_collect();  // code the worker finished
        const u32 pa = fetch_pa_page_ | (s_.pc & 0xFFFu);
        BlockCache::Block* b = cache_->find(pa);
        if (!b)
            b = build_block(pa, fetch_host_);

        // Run the operations until the block ends or something happens
        // that step() would react to before the next instruction (exit_).
        exit_ = 0;
        const u64 left = count - done;
        if (b->code && b->count <= left) {
            // Generated code: the same operations; it keeps the
            // instruction count, so what ran is the difference.
            const u64 before = instr_count_;
            jit_limit_ = before + left;
            jit_run(b);
            done += instr_count_ - before;
            continue;
        }
        const u32 n = b->count < left ? b->count : static_cast<u32>(left);
        const BlockOp* op = b->ops;
        const BlockOp* const stop = op + n;
        do {
            op->fn(*this, op->insn);
            ++op;
        } while (op != stop && !exit_);
        done += static_cast<u64>(op - b->ops);
    }
}

} // namespace g3::ppc
