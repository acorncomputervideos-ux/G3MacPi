// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/platform/test_disk_cache.cpp — disk-image I/O by another core
// (platform/common/disk_cache): every read must return what a plain device
// would, with the I/O done in the same thread (deterministic) or on another
// thread; read-ahead must work; errors must be reported.

#include "platform/common/disk_cache.h"
#include "tests/framework/g3test.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

using namespace g3;

namespace {

constexpr u32 kSector = BlockDevice::kSectorSize;

// A RAM disk that can be told to fail.
struct TestDisk final : BlockDevice {
    std::vector<u8> data;
    bool ro = false;
    std::atomic<bool> fail_reads{false}, fail_writes{false};
    std::atomic<u32> reads{0}, writes{0}, flushes{0};
    explicit TestDisk(u64 sectors) : data(sectors * kSector)
    {
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<u8>(i * 7 + i / 512);
    }
    u64 sector_count() const override { return data.size() / kSector; }
    bool read_only() const override { return ro; }
    bool read(u64 lba, u32 count, u8* out) override
    {
        ++reads;
        if (fail_reads || lba + count > sector_count())
            return false;
        std::memcpy(out, &data[lba * kSector], static_cast<size_t>(count) * kSector);
        return true;
    }
    bool write(u64 lba, u32 count, const u8* in) override
    {
        ++writes;
        if (fail_writes || lba + count > sector_count())
            return false;
        std::memcpy(&data[lba * kSector], in, static_cast<size_t>(count) * kSector);
        return true;
    }
    bool flush() override
    {
        ++flushes;
        return true;
    }
};

struct Rig {
    TestDisk disk;
    std::vector<u8> lines, wq;
    std::unique_ptr<DiskCache> cache;
    Rig(u64 sectors, u32 nlines = 16, u32 write_sectors = 64)
        : disk(sectors), lines(static_cast<size_t>(nlines) * DiskCache::kLineBytes),
          wq(static_cast<size_t>(write_sectors) * kSector)
    {
        cache = std::make_unique<DiskCache>(disk, lines.data(), nlines, wq.data(), write_sectors);
    }
    // Same thread: waiting means doing the I/O.
    void inline_io()
    {
        cache->set_wait([](void* c) { static_cast<DiskCache*>(c)->service(); }, cache.get());
    }
};

struct IoThread {
    DiskCache& c;
    std::atomic<bool> stop{false};
    std::thread t;
    explicit IoThread(DiskCache& cache) : c(cache), t([this] {
        while (!stop.load())
            if (!c.service())
                std::this_thread::yield();
        c.service();
    }) {}
    ~IoThread()
    {
        stop.store(true);
        t.join();
    }
};

struct Rng {
    u64 s;
    explicit Rng(u64 seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    u32 next()
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<u32>(s >> 16);
    }
    u32 below(u32 n) { return next() % n; }
};

// Random reads and writes against a model; the device size is not a whole
// number of lines and accesses cross line boundaries.
void random_mix(Rig& r, u32 seed, u32 ops)
{
    std::vector<u8> model = r.disk.data;
    const u32 sectors = static_cast<u32>(r.disk.sector_count());
    Rng g(seed);
    std::vector<u8> buf;
    for (u32 i = 0; i < ops; ++i) {
        const u32 len = 1 + g.below(g.below(4) ? 8 : 300);
        const u32 lba = g.below(sectors - (len < sectors ? len : sectors - 1));
        const u32 n = lba + len <= sectors ? len : sectors - lba;
        buf.resize(static_cast<size_t>(n) * kSector);
        const u32 what = g.below(10);
        if (what < 6) {
            REQUIRE(r.cache->read(lba, n, buf.data()));
            REQUIRE(std::memcmp(buf.data(), &model[static_cast<size_t>(lba) * kSector], buf.size()) == 0);
        } else if (what < 9) {
            for (u8& b : buf)
                b = static_cast<u8>(g.next());
            REQUIRE(r.cache->write(lba, n, buf.data()));
            std::memcpy(&model[static_cast<size_t>(lba) * kSector], buf.data(), buf.size());
        } else {
            REQUIRE(r.cache->flush());
            REQUIRE(r.disk.data == model);  // everything written is on the device
        }
    }
    REQUIRE(r.cache->flush());
    CHECK(r.disk.data == model);
}

} // namespace

TEST_CASE("platform/diskcache/same_thread_random")
{
    for (u32 seed = 1; seed <= 4; ++seed) {
        Rig r(1000 + seed * 37, 8, 40);  // small: lines get evicted, the write queue wraps
        r.inline_io();
        random_mix(r, seed, 3000);
    }
}

TEST_CASE("platform/diskcache/io_thread_random")
{
    for (u32 seed = 11; seed <= 16; ++seed) {
        Rig r(3000 + seed * 101, 8, 40);
        r.cache->set_wait([](void*) { std::this_thread::yield(); }, nullptr);
        IoThread io(*r.cache);
        random_mix(r, seed, 4000);
    }
}

TEST_CASE("platform/diskcache/sequential_reads_are_read_ahead")
{
    Rig r(16384);  // 8 MB
    r.cache->set_wait([](void*) { std::this_thread::yield(); }, nullptr);
    IoThread io(*r.cache);
    std::vector<u8> buf(4 * 2048);
    for (u32 lba = 0; lba < 8192; lba += 16) {  // 4 MB in 8 KB reads, as a CD-ROM driver does
        REQUIRE(r.cache->read(lba, 16, buf.data()));
        REQUIRE(std::memcmp(buf.data(), &r.disk.data[static_cast<size_t>(lba) * kSector], buf.size()) == 0);
    }
    const DiskCache::Stats& st = r.cache->stats();
    std::printf("    sequential 4 MB: %llu hits, %llu misses, %llu lines read ahead\n",
                static_cast<unsigned long long>(st.hits), static_cast<unsigned long long>(st.misses),
                static_cast<unsigned long long>(st.read_ahead));
    CHECK(st.read_ahead >= 60);         // 64 lines in 4 MB; almost all read ahead
    CHECK(st.misses <= 2);
    CHECK(st.hits > 400);
}

TEST_CASE("platform/diskcache/a_read_sees_the_write_before_it")
{
    // A line is being filled (read ahead) while the same sectors are
    // written: the cache must end up with the new data, and so must the disk.
    Rig r(4096);
    r.inline_io();
    std::vector<u8> buf(kSector);
    REQUIRE(r.cache->read(0, 1, buf.data()));
    r.cache->set_wait(nullptr, nullptr);  // from now on nobody does I/O until we say
    REQUIRE(r.cache->read(1, 1, buf.data()));  // sequential: lines 1-4 queued for read-ahead, not done
    std::vector<u8> mine(kSector, 0x5A);
    r.inline_io();
    REQUIRE(r.cache->write(DiskCache::kLineSectors + 3, 1, mine.data()));  // inside line 1
    REQUIRE(r.cache->read(DiskCache::kLineSectors + 3, 1, buf.data()));
    CHECK(buf == mine);
    REQUIRE(r.cache->flush());
    CHECK(std::memcmp(&r.disk.data[(DiskCache::kLineSectors + 3) * kSector], mine.data(), kSector) == 0);
}

TEST_CASE("platform/diskcache/errors")
{
    Rig r(2048);
    r.inline_io();
    std::vector<u8> buf(kSector);
    // A read error fails that read; the next try reads again.
    r.disk.fail_reads = true;
    CHECK(!r.cache->read(5, 1, buf.data()));
    r.disk.fail_reads = false;
    CHECK(r.cache->read(5, 1, buf.data()));
    // A write error is reported by the next flush, once.
    r.disk.fail_writes = true;
    CHECK(r.cache->write(7, 1, buf.data()));  // queued: can't fail yet
    CHECK(!r.cache->flush());
    r.disk.fail_writes = false;
    CHECK(r.cache->flush());
    // Out of range and read-only.
    CHECK(!r.cache->read(2047, 2, buf.data()));
    CHECK(!r.cache->write(2048, 1, buf.data()));
    Rig ro(2048);
    ro.disk.ro = true;
    ro.cache = std::make_unique<DiskCache>(ro.disk, ro.lines.data(), 16, ro.wq.data(), 64);
    CHECK(ro.cache->read_only());
    CHECK(!ro.cache->write(0, 1, buf.data()));
}
