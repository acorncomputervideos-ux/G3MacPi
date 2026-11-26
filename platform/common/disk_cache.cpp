// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/disk_cache.cpp — see disk_cache.h.

#include "platform/common/disk_cache.h"

namespace g3 {

namespace {
void copy(u8* d, const u8* s, u32 n)
{
    for (u32 i = 0; i < n; ++i)
        d[i] = s[i];
}
} // namespace

DiskCache::DiskCache(BlockDevice& backing, u8* line_mem, u32 lines, u8* write_mem, u32 write_sectors)
    : backing_(backing), sectors_(backing.sector_count()), read_only_(backing.read_only()), line_mem_(line_mem),
      nlines_(lines > 256 ? 256 : (lines ? lines : 1)), write_mem_(write_mem),
      write_sectors_(write_sectors ? write_sectors : 1)
{
}

void DiskCache::wait_once()
{
    ++stats_.waits;
    if (wait_)
        wait_(wait_ctx_);
    else
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void DiskCache::push(const Request& r)
{
    while (!queue_.push(r))
        wait_once();
#if defined(__aarch64__)
    asm volatile("sev" ::: "memory");  // the I/O core may be waiting in WFE
#endif
}

bool DiskCache::load_line(u64 line_lba, u32& slot)
{
    slot = slot_of(line_lba);
    Line& l = lines_[slot];
    bool first = true;
    for (;;) {
        const u32 st = state(slot);
        if (l.tag == line_lba) {
            if (st == kValid) {
                if (first)
                    ++stats_.hits;
                return true;
            }
            if (st == kLoading) {
                first = false;
                wait_once();
                continue;
            }
            if (st == kError) {
                l.tag = ~0ull;  // try again next time
                set_state(slot, kEmpty);
                return false;
            }
        } else if (st == kLoading) {
            wait_once();  // the I/O core is filling this memory for another line
            continue;
        }
        // Claim the slot for this line and have it filled.
        l.tag = line_lba;
        set_state(slot, kLoading);
        push(Request{kFill, slot, line_lba, 0, 0, 0});
        ++stats_.misses;
        first = false;
    }
}

void DiskCache::read_ahead(u64 line_lba)
{
    for (u32 k = 0; k < kReadAhead; ++k, line_lba += kLineSectors) {
        if (line_lba >= sectors_)
            return;
        const u32 slot = slot_of(line_lba);
        Line& l = lines_[slot];
        const u32 st = state(slot);
        if (st == kLoading || (l.tag == line_lba && st == kValid))
            continue;  // busy, or already there
        l.tag = line_lba;
        set_state(slot, kLoading);
        push(Request{kFill, slot, line_lba, 0, 0, 0});
        ++stats_.read_ahead;
    }
}

bool DiskCache::read(u64 lba, u32 count, u8* out)
{
    if (lba > sectors_ || count > sectors_ - lba)
        return false;
    const bool sequential = lba == next_lba_;
    u64 last_line = 0;
    while (count) {
        const u64 line_lba = lba - lba % kLineSectors;
        const u32 off = static_cast<u32>(lba - line_lba);
        const u32 n = count < kLineSectors - off ? count : kLineSectors - off;
        u32 slot;
        if (!load_line(line_lba, slot))
            return false;
        copy(out, line_data(slot) + static_cast<size_t>(off) * kSectorSize, n * kSectorSize);
        out += static_cast<size_t>(n) * kSectorSize;
        lba += n;
        count -= n;
        last_line = line_lba;
    }
    next_lba_ = lba;
    if (sequential)
        read_ahead(last_line + kLineSectors);
    return true;
}

bool DiskCache::write(u64 lba, u32 count, const u8* in)
{
    if (read_only_ || lba > sectors_ || count > sectors_ - lba)
        return false;
    while (count) {
        // Room in the write queue (a request never wraps around its end).
        u32 used;
        while ((used = write_head_ - __atomic_load_n(&write_tail_, __ATOMIC_ACQUIRE)) == write_sectors_)
            wait_once();
        const u32 at = write_head_ % write_sectors_;
        u32 n = write_sectors_ - used;
        if (n > write_sectors_ - at)
            n = write_sectors_ - at;
        if (n > count)
            n = count;
        copy(write_mem_ + static_cast<size_t>(at) * kSectorSize, in, n * kSectorSize);
        push(Request{kWrite, 0, lba, n, at, 0});
        write_head_ += n;
        ++stats_.writes_queued;
        // The same sectors in the cache: wait out a fill queued before this
        // write, then patch the line.
        for (u64 s = lba; s < lba + n;) {
            const u64 line_lba = s - s % kLineSectors;
            const u64 end = line_lba + kLineSectors < lba + n ? line_lba + kLineSectors : lba + n;
            const u32 slot = slot_of(line_lba);
            if (lines_[slot].tag == line_lba) {
                while (state(slot) == kLoading)
                    wait_once();
                if (state(slot) == kValid)
                    copy(line_data(slot) + static_cast<size_t>(s - line_lba) * kSectorSize,
                         in + static_cast<size_t>(s - lba) * kSectorSize, static_cast<u32>(end - s) * kSectorSize);
            }
            s = end;
        }
        in += static_cast<size_t>(n) * kSectorSize;
        lba += n;
        count -= n;
    }
    return true;
}

bool DiskCache::flush()
{
    const u32 seq = ++flush_seq_;
    push(Request{kFlush, 0, 0, 0, 0, seq});
    while (static_cast<s32>(__atomic_load_n(&flushed_seq_, __ATOMIC_ACQUIRE) - seq) < 0)
        wait_once();
    return __atomic_load_n(&flush_ok_, __ATOMIC_RELAXED) != 0;
}

u32 DiskCache::service()
{
    Request r;
    u32 n = 0;
    while (queue_.pop(r)) {
        ++n;
        switch (r.kind) {
        case kFill: {
            const u64 left = sectors_ - r.lba;
            const u32 count = left < kLineSectors ? static_cast<u32>(left) : kLineSectors;
            const bool ok = backing_.read(r.lba, count, line_data(r.line));
            set_state(r.line, ok ? kValid : kError);
            break;
        }
        case kWrite:
            if (!backing_.write(r.lba, r.count, write_mem_ + static_cast<size_t>(r.slot) * kSectorSize)) {
                write_failed_ = 1;
                ++stats_.write_errors;
            }
            __atomic_store_n(&write_tail_, __atomic_load_n(&write_tail_, __ATOMIC_RELAXED) + r.count,
                             __ATOMIC_RELEASE);
            break;
        default: {  // kFlush
            const bool ok = backing_.flush() && !write_failed_;
            write_failed_ = 0;
            __atomic_store_n(&flush_ok_, ok ? 1u : 0u, __ATOMIC_RELAXED);
            __atomic_store_n(&flushed_seq_, r.seq, __ATOMIC_RELEASE);
            break;
        }
        }
    }
    return n;
}

} // namespace g3
