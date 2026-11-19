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

BlockCache::Block* Ppc750::build_block(u32 pa, const u8* host_page)
{
    BlockCache::Block* b = cache_->allocate(pa);
    for (u32 off = pa & 0xFFFu; off < 0x1000u && b->count < BlockCache::kMaxOps; off += 4) {
        const u32 insn = load_be32(host_page + off);
        b->ops[b->count++] = BlockOp{&Ppc750::op_generic, insn};
        if (ends_block(insn))
            break;
    }
    cache_->commit(b);
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
            fetch_msr_ = fetch_msr;
        }
        const u32 page_pa = static_cast<u32>(fetch_host_ - ram_) + ram_base_;
        const u32 pa = page_pa | (s_.pc & 0xFFFu);
        if ((pa >> BlockCache::kPageShift) >= cache_->pages()) {
            execute(load_be32(fetch_host_ + (s_.pc & 0xFFFu)));  // RAM the cache does not cover
            ++done;
            continue;
        }
        BlockCache::Block* b = cache_->find(pa);
        if (!b)
            b = build_block(pa, fetch_host_);

        cache_->dirty = false;
        const u32 entry_msr = s_.msr;
        u32 expect = s_.pc;
        const u64 left = count - done;
        const u32 n = b->count < left ? b->count : static_cast<u32>(left);
        u32 i = 0;
        while (i < n) {
            const BlockOp& op = b->ops[i++];
            op.fn(*this, op.insn);
            expect += 4;
            // Leave the block when the next instruction is not its next
            // operation, or when step() would now do something else.
            if (s_.pc != expect || s_.msr != entry_msr || cache_->dirty || checkstop_ || break_run_)
                break;
            if ((s_.msr & msr::EE) && (external_pending_ || dec_pending_))
                break;
        }
        done += i;
    }
}

} // namespace g3::ppc
