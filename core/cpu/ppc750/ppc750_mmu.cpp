// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/cpu/ppc750/ppc750_mmu.cpp — effective -> physical address translation
// (PEM chapter 7: block address translation, segmented hashed page tables,
// referenced/changed bits, protection) and the DSI/ISI exceptions it raises.
//
// No translation cache yet: every access consults the BATs and, if needed,
// walks the page table. A soft TLB will be added later and verified against
// this path.

#include "core/cpu/ppc750/ppc750.h"

namespace g3::ppc {

namespace {

enum Fault : u32 {
    kFaultNone = 0,
    kFaultNotFound = 1,
    kFaultProtection = 2,
    kFaultNoExecute = 3,
    kFaultDirectStore = 4,
};

// PP access check for pages (PEM Table 7-10). key: Ks or Kp selected by MSR[PR].
bool page_access_allowed(u32 pp, bool key, bool write)
{
    if (!key) {
        // 00, 01, 10: read/write; 11: read-only.
        return !(write && pp == 3);
    }
    // 00: no access; 01: read-only; 10: read/write; 11: read-only.
    if (pp == 0)
        return false;
    return !(write && pp != 2);
}

// PP access check for BATs (PEM Table 7-7): 00 no access, x1 read-only,
// 10 read/write.
bool bat_access_allowed(u32 pp, bool write)
{
    if (pp == 0)
        return false;
    return !(write && pp != 2);
}

} // namespace

bool Ppc750::bat_translate(u32 ea, Access access, u32& pa, bool& protection_fault,
                           bool& writable)
{
    const u32 base = access == Access::Fetch ? spr::IBAT0U : spr::DBAT0U;
    const bool user = in_user_mode();
    for (u32 i = 0; i < 4; ++i) {
        const u32 batu = s_.spr[base + 2 * i];
        const u32 batl = s_.spr[base + 2 * i + 1];
        const bool valid = user ? (batu & mmu::BATU_VP) : (batu & mmu::BATU_VS);
        if (!valid)
            continue;
        // BEPI = BATU[0-14]; BL = BATU[19-29] masks EA[4-14].
        const u32 bl = (batu >> 2) & 0x7FF;
        const u32 ea_hi = ea >> 17;          // EA[0-14]
        const u32 bepi = batu >> 17;         // BATU[0-14]
        if ((ea_hi & ~bl) != (bepi & ~bl))
            continue;
        // Match.
        const bool write = access == Access::Write;
        if (!bat_access_allowed(batl & 3, write)) {
            protection_fault = true;
            return false;
        }
        writable = bat_access_allowed(batl & 3, true);
        const u32 brpn = batl >> 17;
        // PA = BRPN[0-3] || (BRPN[4-14] | (EA[4-14] & BL)) || EA[15-31] (PEM 7.4.3).
        pa = ((brpn | (ea_hi & bl)) << 17) | (ea & 0x1FFFF);
        return true;
    }
    return false;
}

bool Ppc750::page_translate(u32 ea, Access access, u32& pa, u32& fault, bool& writable,
                            bool& changed, bool probe)
{
    const u32 sr = s_.sr[ea >> 28];
    const bool fetch = access == Access::Fetch;
    const bool write = access == Access::Write;

    if (sr & mmu::SR_T) {
        // Direct-store segment. The 750 does not support direct-store
        // accesses (750 UM 7.1). Mac OS does not use them.
        fault = fetch ? kFaultNoExecute : kFaultDirectStore;
        return false;
    }
    if (fetch && (sr & mmu::SR_N)) {
        fault = kFaultNoExecute;
        return false;
    }

    const u32 vsid = sr & mmu::SR_VSID_MASK;
    const u32 page_index = (ea >> 12) & 0xFFFF;
    const u32 api = page_index >> 10;
    const u32 sdr1 = s_.spr[spr::SDR1];
    const u32 htaborg = sdr1 & 0xFFFF0000u;
    const u32 htabmask = sdr1 & 0x1FFu;

    const u32 primary_hash = ((vsid & 0x7FFFF) ^ page_index) & 0x7FFFF;
    for (u32 h = 0; h < 2; ++h) {
        const u32 hash = h == 0 ? primary_hash : (~primary_hash & 0x7FFFF);
        // PTEG address (PEM 7.6.1.2): bits 0-6 from HTABORG, bits 7-15 =
        // HTABORG[7-15] | (hash[0-8] & HTABMASK), bits 16-25 = hash[9-18].
        const u32 pteg = (htaborg & 0xFE000000u) |
                         ((((htaborg >> 16) & 0x1FF) | ((hash >> 10) & htabmask)) << 16) |
                         ((hash & 0x3FF) << 6);
        const u32 want = mmu::PTE0_V | (vsid << 7) | (h ? mmu::PTE0_H : 0) | api;
        for (u32 i = 0; i < 8; ++i) {
            const u32 pte_addr = pteg + 8 * i;
            u32 w0;
            if (!mem_.read32(pte_addr, w0)) {
                if (!probe)
                    machine_check();
                fault = kFaultNone;
                return false;
            }
            if (w0 != want)
                continue;
            u32 w1;
            if (!mem_.read32(pte_addr + 4, w1)) {
                if (!probe)
                    machine_check();
                fault = kFaultNone;
                return false;
            }
            // Protection.
            const bool key = (s_.msr & msr::PR) ? (sr & mmu::SR_KP) : (sr & mmu::SR_KS);
            if (!page_access_allowed(w1 & 3, key, write)) {
                fault = kFaultProtection;
                // R is still set: the page was referenced (PEM 7.5.3).
                if (!(w1 & mmu::PTE1_R) && !probe) {
                    mem_.write32(pte_addr + 4, w1 | mmu::PTE1_R);
                    note_store(pte_addr + 4, 4);
                }
                return false;
            }
            if (fetch && (w1 & mmu::PTE1_G)) {
                fault = kFaultNoExecute;  // instruction fetch from guarded page
                return false;
            }
            // Referenced / changed bits (the 750 updates them in hardware).
            u32 nw1 = w1 | mmu::PTE1_R;
            if (write)
                nw1 |= mmu::PTE1_C;
            if (nw1 != w1 && !probe) {
                mem_.write32(pte_addr + 4, nw1);
                note_store(pte_addr + 4, 4);
            }
            writable = page_access_allowed(w1 & 3, key, true);
            changed = (nw1 & mmu::PTE1_C) != 0;
            pa = (w1 & 0xFFFFF000u) | (ea & 0xFFF);
            return true;
        }
    }
    fault = kFaultNotFound;
    return false;
}

void Ppc750::raise_translation_fault(u32 ea, Access access, u32 cause)
{
    if (access == Access::Fetch) {
        u32 bits = 0;
        switch (cause) {
        case kFaultNotFound:   bits = isi::NOT_FOUND; break;
        case kFaultProtection: bits = isi::PROTECTION; break;
        default:               bits = isi::NO_EXECUTE; break;
        }
        // SRR0 = the instruction that could not be fetched (= ea).
        raise_exception(Exception::Isi, ea, bits);
        return;
    }
    u32 bits = access == Access::Write ? dsisr::STORE : 0;
    switch (cause) {
    case kFaultNotFound:    bits |= dsisr::NOT_FOUND; break;
    case kFaultProtection:  bits |= dsisr::PROTECTION; break;
    case kFaultDirectStore: bits |= dsisr::DIRECT_STORE; break;
    default: break;
    }
    s_.spr[spr::DAR] = ea;
    s_.spr[spr::DSISR] = bits;
    raise_exception(Exception::Dsi, s_.pc);
}

bool Ppc750::translate_slow(u32 ea, Access access, u32& pa, bool& writable, bool& changed)
{
    bool bat_protection = false;
    if (bat_translate(ea, access, pa, bat_protection, writable)) {
        changed = true;  // BATs have no R/C bits
        return true;
    }
    if (bat_protection) {
        raise_translation_fault(ea, access, kFaultProtection);
        return false;
    }
    u32 fault = kFaultNone;
    if (page_translate(ea, access, pa, fault, writable, changed))
        return true;
    if (fault != kFaultNone)  // kFaultNone: a machine check was already raised
        raise_translation_fault(ea, access, fault);
    return false;
}

bool Ppc750::probe_translate(u32 ea, bool write, u32& pa)
{
    if (!(s_.msr & msr::DR)) {
        pa = ea;
        return true;
    }
    const Access access = write ? Access::Write : Access::Read;
    bool protection = false, writable = false, changed = false;
    if (bat_translate(ea, access, pa, protection, writable))
        return true;
    if (protection)
        return false;
    u32 fault = 0;
    return page_translate(ea, access, pa, fault, writable, changed, true);
}

void Ppc750::flush_tlb()
{
    fetch_host_ = nullptr;
    for (u32 i = 0; i < kTlbSize; ++i) {
        itlb_[i].tag = 0;
        dtlb_[i].tag = 0;
    }
}

bool Ppc750::translate_enabled(u32 ea, Access access, u32& pa)
{
    const u32 page = ea >> 12;
    const u32 tag = kTlbValid | (page << 1) | ((s_.msr & msr::PR) ? 1u : 0u);
    TlbEntry& e = (access == Access::Fetch ? itlb_ : dtlb_)[page & (kTlbSize - 1)];

    if (tlb_enabled_ && e.tag == tag) {
        // A store may use the cached entry only if the page is writable and
        // its C bit is already set; otherwise the slow path checks the
        // protection and sets C in the page table (as the 750 does).
        if (access != Access::Write || (e.writable && e.changed)) {
            pa = e.pa_page | (ea & 0xFFF);
            return true;
        }
    }

    bool writable = false, changed = false;
    if (!translate_slow(ea, access, pa, writable, changed))
        return false;
    if (tlb_enabled_) {
        e.tag = tag;
        e.pa_page = pa & ~0xFFFu;
        e.writable = writable;
        e.changed = changed;
    }
    return true;
}

} // namespace g3::ppc
