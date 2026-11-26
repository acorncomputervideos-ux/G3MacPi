// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the mac-io NVRAM, accessed like Linux's direct_nvram_*_byte:
// in_8/out_8 at nvram_data[(addr & 0x1FFF) * nvram_mult].

#include "core/devices/macio/nvram.h"
#include "core/mem/physical_bus.h"
#include "tests/framework/g3test.h"

using namespace g3;

namespace {
constexpr u32 kBase = 0x80860000u;
// Linux: nvram_mult = (region size + NVRAM_SIZE - 1) / NVRAM_SIZE.
constexpr u32 kMult = (MacioNvram::kWindowSize + 0x2000 - 1) / 0x2000;

struct Rig {
    PhysicalBus bus;
    MacioNvram nv;
    Rig() { bus.map_mmio(kBase, MacioNvram::kWindowSize, &nv); }
    u8 rd(u32 i)
    {
        u8 v = 0;
        bus.read8(kBase + (i & 0x1FFF) * kMult, v);
        return v;
    }
    void wr(u32 i, u8 v) { bus.write8(kBase + (i & 0x1FFF) * kMult, v); }
};
} // namespace

TEST_CASE("devices/nvram/linux_direct_access")
{
    static_assert(kMult == 16, "Linux derives the stride from the region size");
    Rig r;
    CHECK(!r.nv.dirty());
    for (u32 i = 0; i < MacioNvram::kSize; ++i)
        r.wr(i, static_cast<u8>(i * 7 + (i >> 8)));
    CHECK(r.nv.dirty());
    bool all = true;
    for (u32 i = 0; i < MacioNvram::kSize; ++i)
        all &= r.rd(i) == static_cast<u8>(i * 7 + (i >> 8));
    CHECK(all);
    CHECK_EQ(r.nv.byte(0x1FFF), static_cast<u8>(0x1FFF * 7 + 0x1F));
    // Bytes between the stride points alias the same cell.
    u8 v = 0;
    r.bus.read8(kBase + 5 * 16 + 9, v);
    CHECK_EQ(v, r.rd(5));
}

TEST_CASE("devices/nvram/wide_access_and_persistence")
{
    Rig r;
    r.wr(3, 0xAB);
    u32 w = 0;
    r.bus.read32(kBase + 3 * 16, w);
    CHECK_EQ(w, 0xABABABABu);              // 4 byte accesses to one cell
    r.bus.write32(kBase + 4 * 16 + 14, 0x11223344u);  // straddles cells 4 and 5
    CHECK_EQ(r.rd(4), 0x22u);
    CHECK_EQ(r.rd(5), 0x44u);
    r.bus.read32(kBase + 4 * 16 + 14, w);
    CHECK_EQ(w, 0x22224444u);              // read straddles too

    u8 image[MacioNvram::kSize];
    for (u32 i = 0; i < sizeof image; ++i)
        image[i] = static_cast<u8>(~i);
    r.nv.load(image, sizeof image);
    CHECK(!r.nv.dirty());
    CHECK_EQ(r.rd(0x123), static_cast<u8>(~0x123u));
    r.wr(0x123, static_cast<u8>(~0x123u));  // same value: not dirty
    CHECK(!r.nv.dirty());
    r.wr(0x123, 0);
    CHECK(r.nv.dirty());
    CHECK_EQ(r.nv.data()[0x123], 0u);
}
