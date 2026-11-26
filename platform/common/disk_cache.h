// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/disk_cache.h — disk-image I/O done by another core.
//
// A DiskCache sits in front of a disk or CD image (the "backing" device)
// and is what the emulated drive reads and writes. The Mac's core calls
// read()/write()/flush(); another core calls service(), which does the
// actual I/O on the backing device (the SD card on the Pi). So:
//  - Reads are served from a cache of 64 KB lines. A miss asks the I/O core
//    to fill the line and waits for it (as long as the read would have
//    taken anyway); when the reads are sequential (CD-ROMs, installers),
//    the following lines are read ahead, and later reads are copies.
//  - Writes are copied into a queue and return at once; the I/O core writes
//    them in order. Lines in the cache are updated too, and every request
//    goes through one FIFO, so a read after a write sees the new data.
//  - flush() waits until everything queued is on the backing device and
//    reports any write that failed since the last flush (a write error can
//    only be reported then, as with a drive's write cache).
//
// One request queue, filled by the Mac's core only and emptied by the I/O
// core only. Line states are published with release/acquire. The Mac's
// core never touches a line's memory while the I/O core is filling it.
//
// Freestanding.

#ifndef G3_PLATFORM_COMMON_DISK_CACHE_H
#define G3_PLATFORM_COMMON_DISK_CACHE_H

#include "core/common/spsc_queue.h"
#include "core/common/types.h"
#include "core/storage/block_device.h"

namespace g3 {

class DiskCache final : public BlockDevice {
public:
    static constexpr u32 kLineSectors = 128;  // 64 KB
    static constexpr u32 kLineBytes = kLineSectors * kSectorSize;
    static constexpr u32 kReadAhead = 4;      // lines read ahead of a sequential read

    // `lines` lines of cache at line_mem (lines * kLineBytes bytes) and a
    // write queue of write_sectors sectors at write_mem.
    DiskCache(BlockDevice& backing, u8* line_mem, u32 lines, u8* write_mem, u32 write_sectors);

    // BlockDevice (the Mac's core).
    u64 sector_count() const override { return sectors_; }
    bool read_only() const override { return read_only_; }
    bool read(u64 lba, u32 count, u8* out) override;
    bool write(u64 lba, u32 count, const u8* in) override;
    bool flush() override;

    // What the Mac's core does while it waits for the I/O core (default:
    // spin). E.g. SEV/WFE on the Pi, yield on a host.
    void set_wait(void (*wait)(void*), void* context)
    {
        wait_ = wait;
        wait_ctx_ = context;
    }

    // The I/O core: does every queued request. Returns how many.
    u32 service();

    struct Stats {
        u64 hits = 0, misses = 0, read_ahead = 0, writes_queued = 0, waits = 0;
        u64 write_errors = 0;  // I/O core
    };
    const Stats& stats() const { return stats_; }

private:
    enum State : u32 { kEmpty, kLoading, kValid, kError };
    enum Kind : u32 { kFill, kWrite, kFlush };
    struct Request {
        u32 kind;
        u32 line;    // kFill
        u64 lba;     // kFill: first sector of the line; kWrite: first sector
        u32 count;   // kWrite: sectors
        u32 slot;    // kWrite: first sector slot in the write queue
        u32 seq;     // kFlush
    };
    struct Line {
        u64 tag = ~0ull;  // first sector; ~0: none
        u32 state = kEmpty;
    };

    void push(const Request& r);
    void wait_once();
    u32 state(u32 line) const { return __atomic_load_n(&lines_[line].state, __ATOMIC_ACQUIRE); }
    void set_state(u32 line, u32 s) { __atomic_store_n(&lines_[line].state, s, __ATOMIC_RELEASE); }
    u32 slot_of(u64 line_lba) const { return static_cast<u32>((line_lba / kLineSectors) % nlines_); }
    // Makes `line_lba`'s line valid in the cache; false on a read error.
    bool load_line(u64 line_lba, u32& slot);
    void read_ahead(u64 line_lba);
    u8* line_data(u32 slot) { return line_mem_ + static_cast<size_t>(slot) * kLineBytes; }

    BlockDevice& backing_;
    u64 sectors_;
    bool read_only_;
    u8* line_mem_;
    u32 nlines_;
    Line lines_[256];  // the first nlines_ (at most 256) are used
    u8* write_mem_;
    u32 write_sectors_;
    u32 write_head_ = 0;   // sectors handed to the queue (Mac's core)
    u32 write_tail_ = 0;   // sectors written (I/O core; atomic)
    u32 flush_seq_ = 0;    // Mac's core
    u32 flushed_seq_ = 0;  // I/O core; atomic
    u32 flush_ok_ = 1;     // I/O core; atomic: the last flush succeeded
    u32 write_failed_ = 0; // I/O core: a write failed since the last flush
    u64 next_lba_ = ~0ull; // where the last read ended (sequential detection)
    SpscQueue<Request, 256> queue_;
    void (*wait_)(void*) = nullptr;
    void* wait_ctx_ = nullptr;
    Stats stats_;
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_DISK_CACHE_H
