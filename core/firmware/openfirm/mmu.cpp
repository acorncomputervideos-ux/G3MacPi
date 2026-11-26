// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/mmu.cpp — see mmu.h.

#include "core/firmware/openfirm/mmu.h"

#include "core/common/cstring.h"

namespace g3::of {

namespace {
constexpr u64 kPageMask = Translations::kPage - 1;
}

// --- Translations ------------------------------------------------------------------

bool Translations::insert(const Translation& t)
{
    if (n_ >= kMax)
        return false;
    u32 i = n_;
    while (i > 0 && t_[i - 1].virt > t.virt) {
        t_[i] = t_[i - 1];
        --i;
    }
    t_[i] = t;
    ++n_;
    return true;
}

void Translations::unmap(u32 virt, u32 size)
{
    const u64 lo = virt & ~kPageMask;
    const u64 hi = (static_cast<u64>(virt) + size + kPageMask) & ~kPageMask;
    if (size == 0)
        return;
    Translation keep[kMax];
    u32 k = 0;
    Translation extra[2];
    u32 e = 0;
    for (u32 i = 0; i < n_; ++i) {
        const Translation& t = t_[i];
        const u64 a = t.virt, b = static_cast<u64>(t.virt) + t.size;
        if (b <= lo || a >= hi) {
            keep[k++] = t;
            continue;
        }
        if (a < lo)  // the part below the hole
            keep[k++] = {t.virt, static_cast<u32>(lo - a), t.phys, t.mode};
        if (b > hi)  // the part above it (at most one entry can need both pieces)
            extra[e++ % 2] = {static_cast<u32>(hi), static_cast<u32>(b - hi), static_cast<u32>(t.phys + (hi - a)),
                              t.mode};
    }
    n_ = 0;
    for (u32 i = 0; i < k; ++i)
        t_[n_++] = keep[i];
    for (u32 i = 0; i < e && i < 2; ++i)
        insert(extra[i]);
}

bool Translations::map(u32 phys, u32 virt, u32 size, u32 mode)
{
    if (size == 0)
        return true;
    if ((phys & kPageMask) != (virt & kPageMask))
        return false;
    const u64 lo = virt & ~kPageMask;
    const u64 hi = (static_cast<u64>(virt) + size + kPageMask) & ~kPageMask;
    if (hi > (1ull << 32) || static_cast<u64>(phys & ~kPageMask) + (hi - lo) > (1ull << 32))
        return false;
    // Room check before changing anything: an unmap can split one entry in two.
    if (n_ + 2 > kMax)
        return false;
    unmap(static_cast<u32>(lo), static_cast<u32>(hi - lo));
    return insert({static_cast<u32>(lo), static_cast<u32>(hi - lo), phys & ~static_cast<u32>(kPageMask), mode});
}

bool Translations::modify(u32 virt, u32 size, u32 mode)
{
    bool any = false;
    const u64 lo = virt & ~kPageMask;
    const u64 hi = (static_cast<u64>(virt) + size + kPageMask) & ~kPageMask;
    // Split at the range ends, then change the entries inside.
    Translation copy[kMax];
    u32 n = 0;
    for (u32 i = 0; i < n_; ++i)
        copy[n++] = t_[i];
    for (u32 i = 0; i < n; ++i) {
        const Translation& t = copy[i];
        const u64 a = t.virt, b = static_cast<u64>(t.virt) + t.size;
        if (b <= lo || a >= hi)
            continue;
        const u64 s = a > lo ? a : lo, e = b < hi ? b : hi;
        if (!map(static_cast<u32>(t.phys + (s - a)), static_cast<u32>(s), static_cast<u32>(e - s), mode))
            return false;
        any = true;
    }
    return any;
}

bool Translations::translate(u32 virt, u32& phys, u32& mode) const
{
    for (u32 i = 0; i < n_; ++i) {
        const Translation& t = t_[i];
        if (virt >= t.virt && virt - t.virt < t.size) {
            phys = t.phys + (virt - t.virt);
            mode = t.mode;
            return true;
        }
    }
    return false;
}

// --- PageTable -----------------------------------------------------------------------

u32 PageTable::pteg(u32 hash) const
{
    const u32 htabmask = (size_ >> 16) - 1;
    return (base_ & 0xFE000000u) | ((((base_ >> 16) & 0x1FF) | ((hash >> 10) & htabmask)) << 16) |
           ((hash & 0x3FF) << 6);
}

bool PageTable::clear()
{
    entries_ = 0;
    for (u32 a = 0; a < size_; a += 4)
        if (!mem_.write32(base_ + a, 0))
            return false;
    return true;
}

bool PageTable::insert(u32 vsid, u32 ea, u32 pa, u32 mode)
{
    const u32 page_index = (ea >> 12) & 0xFFFF;
    const u32 api = page_index >> 10;
    const u32 primary = ((vsid & 0x7FFFF) ^ page_index) & 0x7FFFF;
    for (u32 h = 0; h < 2; ++h) {
        const u32 group = pteg(h == 0 ? primary : (~primary & 0x7FFFF));
        for (u32 i = 0; i < 8; ++i) {
            u32 w0 = 0;
            if (!mem_.read32(group + 8 * i, w0))
                return false;
            if (w0 & 0x80000000u)
                continue;
            if (!mem_.write32(group + 8 * i + 4, (pa & 0xFFFFF000u) | (mode & 0x7B)) ||
                !mem_.write32(group + 8 * i, 0x80000000u | ((vsid & 0xFFFFFF) << 7) | (h ? 0x40u : 0) | api))
                return false;
            ++entries_;
            return true;
        }
    }
    return false;
}

// --- MmuDriver -------------------------------------------------------------------------

bool MmuDriver::publish()
{
    if (node_ == kNone)
        return true;
    u32 cells[Translations::kMax * 4];
    for (u32 i = 0; i < t_.count(); ++i) {
        cells[4 * i + 0] = t_[i].virt;
        cells[4 * i + 1] = t_[i].size;
        cells[4 * i + 2] = t_[i].phys;
        cells[4 * i + 3] = t_[i].mode;
    }
    return tree_.set_ints(node_, "translations", cells, 4 * t_.count());
}

bool MmuDriver::resync()
{
    const bool ok = !sync_ || sync_(sync_ctx_);
    return publish() && ok;
}

bool MmuDriver::map(u32 phys, u32 virt, u32 size, u32 mode)
{
    if (mode == 0xFFFFFFFFu)
        mode = (is_io_ && is_io_(io_ctx_, phys)) ? wimg::kIoDefault : wimg::kMemoryDefault;
    return t_.map(phys, virt, size, mode) && resync();
}

void MmuDriver::unmap(u32 virt, u32 size)
{
    t_.unmap(virt, size);
    resync();
}

bool MmuDriver::method(u32, const char* name, forth::Forth& f)
{
    if (cstr::equal(name, "claim")) {  // ( [virt] size align -- base )
        const u32 align = f.pop(), size = f.pop();
        const u32 virt = align == 0 ? f.pop() : 0;
        if (f.failed())
            return true;
        const u32 base = virt_.claim(virt, size, align);
        if (base == MemoryMap::kFail)
            f.fail(-1, "mmu claim: no free virtual space");
        else
            f.push(base);
        return true;
    }
    if (cstr::equal(name, "release")) {  // ( virt size -- )
        const u32 size = f.pop(), virt = f.pop();
        if (!f.failed())
            virt_.release(virt, size);
        return true;
    }
    if (cstr::equal(name, "map")) {  // ( phys virt size mode -- )
        const u32 mode = f.pop(), size = f.pop(), virt = f.pop(), phys = f.pop();
        if (!f.failed() && !map(phys, virt, size, mode))
            f.fail(-1, "mmu map: cannot install the translation");
        return true;
    }
    if (cstr::equal(name, "unmap")) {  // ( virt size -- )
        const u32 size = f.pop(), virt = f.pop();
        if (!f.failed())
            unmap(virt, size);
        return true;
    }
    if (cstr::equal(name, "modify")) {  // ( virt size mode -- )
        u32 mode = f.pop();
        const u32 size = f.pop(), virt = f.pop();
        if (f.failed())
            return true;
        u32 phys = 0, old = 0;
        if (mode == 0xFFFFFFFFu && t_.translate(virt, phys, old))
            mode = (is_io_ && is_io_(io_ctx_, phys)) ? wimg::kIoDefault : wimg::kMemoryDefault;
        if (!t_.modify(virt, size, mode) || !resync())
            f.fail(-1, "mmu modify: nothing mapped there");
        return true;
    }
    if (cstr::equal(name, "translate")) {  // ( virt -- false | phys mode true )
        const u32 virt = f.pop();
        if (f.failed())
            return true;
        u32 phys = 0, mode = 0;
        if (t_.translate(virt, phys, mode)) {
            f.push(phys);
            f.push(mode);
            f.push(0xFFFFFFFFu);
        } else {
            f.push(0);
        }
        return true;
    }
    return false;
}

// --- MemoryDriver ------------------------------------------------------------------------

bool MemoryDriver::method(u32, const char* name, forth::Forth& f)
{
    if (cstr::equal(name, "claim")) {  // ( [phys] size align -- base )
        const u32 align = f.pop(), size = f.pop();
        const u32 phys = align == 0 ? f.pop() : 0;
        if (f.failed())
            return true;
        const u32 base = phys_.claim(phys, size, align);
        if (base == MemoryMap::kFail)
            f.fail(-1, "memory claim: not available");
        else
            f.push(base);
        return true;
    }
    if (cstr::equal(name, "release")) {  // ( phys size -- )
        const u32 size = f.pop(), phys = f.pop();
        if (!f.failed())
            phys_.release(phys, size);
        return true;
    }
    return false;
}

} // namespace g3::of
