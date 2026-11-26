// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/platform/test_machine_state.cpp — the saved clock/NVRAM/PRAM
// file and the choice of start-up time.

#include "platform/common/machine_state.h"
#include "tests/framework/g3test.h"

#include <cstring>
#include <vector>

using namespace g3;

TEST_CASE("platform/state/crc32_known_value")
{
    // The standard check value of CRC-32 (IEEE): "123456789" -> CBF43926.
    const char* s = "123456789";
    CHECK_EQ(crc32_ieee(reinterpret_cast<const u8*>(s), 9), 0xCBF43926u);
    CHECK_EQ(crc32_ieee(nullptr, 0), 0u);
}

TEST_CASE("platform/state/round_trip")
{
    MachineState a;
    a.rtc_1904 = 0xDEADBEEFu;
    for (u32 i = 0; i < MachineState::kNvramSize; ++i)
        a.nvram[i] = static_cast<u8>(i * 7);
    for (u32 i = 0; i < MachineState::kPramSize; ++i)
        a.pram[i] = static_cast<u8>(255 - i);
    std::vector<u8> f(MachineState::kFileSize);
    a.encode(f.data());
    CHECK(std::memcmp(f.data(), "G3ST", 4) == 0);
    MachineState b;
    REQUIRE(b.decode(f.data(), static_cast<u32>(f.size())));
    CHECK_EQ(b.rtc_1904, a.rtc_1904);
    CHECK(std::memcmp(a.nvram, b.nvram, sizeof a.nvram) == 0);
    CHECK(std::memcmp(a.pram, b.pram, sizeof a.pram) == 0);
}

TEST_CASE("platform/state/rejects_bad_files")
{
    MachineState a;
    a.rtc_1904 = 1234;
    std::vector<u8> f(MachineState::kFileSize);
    a.encode(f.data());
    MachineState b;
    b.rtc_1904 = 99;
    CHECK(!b.decode(f.data(), static_cast<u32>(f.size()) - 1));  // short
    CHECK(!b.decode(nullptr, 0));
    std::vector<u8> g = f;
    g[100] ^= 1;  // a flipped NVRAM bit: checksum mismatch
    CHECK(!b.decode(g.data(), static_cast<u32>(g.size())));
    g = f;
    g[0] = 'X';  // magic
    CHECK(!b.decode(g.data(), static_cast<u32>(g.size())));
    g = f;
    g[7] = 2;  // version
    CHECK(!b.decode(g.data(), static_cast<u32>(g.size())));
    CHECK_EQ(b.rtc_1904, 99u);  // unchanged by the failures
}

TEST_CASE("platform/state/start_time_choice")
{
    const u64 trusted = 1577836800ull;  // 2020-01-01
    MachineState saved;
    saved.rtc_1904 = static_cast<u32>(1700000000ull + kUnixTo1904);
    // Host time known: it wins.
    CHECK_EQ(choose_start_time(1800000000ull, trusted, &saved, 978307200ull), 1800000000ull);
    // Host time unknown (a Pi with no network): the saved clock.
    CHECK_EQ(choose_start_time(5, trusted, &saved, 978307200ull), 1700000000ull);
    // Nothing saved: the fallback.
    CHECK_EQ(choose_start_time(5, trusted, nullptr, 978307200ull), 978307200ull);
    MachineState empty;
    CHECK_EQ(choose_start_time(5, trusted, &empty, 978307200ull), 978307200ull);
}
