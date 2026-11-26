// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for core/common/spsc_queue.h

#include "core/common/spsc_queue.h"
#include "tests/framework/g3test.h"

#include <thread>

using namespace g3;

TEST_CASE("common/spsc_queue/fifo_order")
{
    SpscQueue<u32, 8> q;
    CHECK(q.empty());
    for (u32 i = 1; i <= 5; ++i)
        CHECK(q.push(i));
    CHECK_EQ(q.size(), 5u);
    u32 v = 0;
    for (u32 i = 1; i <= 5; ++i) {
        CHECK(q.pop(v));
        CHECK_EQ(v, i);
    }
    CHECK(!q.pop(v));
    CHECK(q.empty());
}

TEST_CASE("common/spsc_queue/full_rejects_without_overwriting")
{
    SpscQueue<u32, 4> q;
    for (u32 i = 0; i < 4; ++i)
        CHECK(q.push(i));
    CHECK(!q.push(99));  // full
    CHECK_EQ(q.size(), 4u);
    u32 v = 0;
    CHECK(q.pop(v));
    CHECK_EQ(v, 0u);  // oldest element intact
    CHECK(q.push(4));  // space again
}

TEST_CASE("common/spsc_queue/index_wraparound")
{
    // Push/pop many more items than the capacity so indices wrap the ring
    // many times.
    SpscQueue<u32, 4> q;
    u32 v = 0;
    for (u32 i = 0; i < 100000; ++i) {
        REQUIRE(q.push(i));
        REQUIRE(q.pop(v));
        REQUIRE(v == i);
    }
    CHECK(q.empty());
}

TEST_CASE("common/spsc_queue/two_threads_stress")
{
    // One producer thread, one consumer thread: every value must arrive
    // exactly once and in order.
    static SpscQueue<u32, 64> q;
    constexpr u32 kCount = 2000000;
    std::thread producer([] {
        for (u32 i = 0; i < kCount; ++i)
            while (!q.push(i)) {
            }
    });
    u32 expected = 0;
    bool in_order = true;
    while (expected < kCount) {
        u32 v;
        if (q.pop(v)) {
            if (v != expected)
                in_order = false;
            ++expected;
        }
    }
    producer.join();
    CHECK(in_order);
    CHECK_EQ(expected, kCount);
    CHECK(q.empty());
}
