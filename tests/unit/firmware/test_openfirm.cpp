// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the Open Firmware device tree and client interface.
// (The tree built here is a test fixture, not a claim about the B&W.)

#include "core/firmware/openfirm/client_interface.h"
#include "core/firmware/openfirm/device_tree.h"
#include "core/mem/physical_bus.h"
#include "tests/framework/g3test.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace g3;
using namespace g3::of;

namespace {

struct Fixture {
    std::unique_ptr<DeviceTree> t = std::make_unique<DeviceTree>();
    Phandle cpus, cpu, pci, macio, ata, disk, aliases;
    Fixture()
    {
        t->set_string(t->root(), "name", "device-tree");
        cpus = t->add_node(t->root(), "cpus");
        cpu = t->add_node(cpus, "PowerPC,750", "0");
        pci = t->add_node(t->root(), "pci", "80000000");
        macio = t->add_node(pci, "mac-io", "10");
        t->add_node(macio, "interrupt-controller", "0");
        ata = t->add_node(macio, "ata", "20000");
        t->add_node(macio, "ata", "21000");
        disk = t->add_node(ata, "disk", "0");
        aliases = t->add_node(t->root(), "aliases");
        t->set_string(aliases, "hd", "/pci@80000000/mac-io@10/ata@20000/disk@0");
        t->set_string(aliases, "macio", "/pci@80000000/mac-io@10");
        t->set_int(cpu, "clock-frequency", 350000000);
        const u32 reg[] = {0x20000, 0x1000, 0x8B00, 0x100};
        t->set_ints(ata, "reg", reg, 4);
        t->set_string(ata, "device_type", "ata");
    }
};

std::string path(const DeviceTree& t, Phandle h)
{
    char buf[128];
    t.path_of(h, buf, sizeof buf);
    return buf;
}

} // namespace

TEST_CASE("firmware/devtree/build_walk_and_properties")
{
    Fixture f;
    DeviceTree& t = *f.t;
    REQUIRE(t.ok());
    CHECK_EQ(t.peer(kNone), t.root());
    CHECK_EQ(t.parent(t.root()), kNone);
    CHECK_EQ(t.child(t.root()), f.cpus);
    CHECK_EQ(t.peer(f.cpus), f.pci);
    CHECK_EQ(t.peer(f.aliases), kNone);
    CHECK_EQ(t.parent(f.disk), f.ata);
    const u8* d = nullptr;
    u32 len = 0;
    REQUIRE(t.get_property(f.cpu, "name", d, len));          // "name" set by add_node
    CHECK_EQ(len, 12u);                                      // "PowerPC,750" + NUL
    CHECK(std::memcmp(d, "PowerPC,750", 12) == 0);
    REQUIRE(t.get_property(f.cpu, "clock-frequency", d, len));
    CHECK_EQ(len, 4u);
    CHECK_EQ(d[0], 0x14u);                                   // 350000000 = 0x14DC9380, big-endian
    CHECK_EQ(d[3], 0x80u);
    // Replacing a property keeps its position and changes its value.
    t.set_int(f.cpu, "clock-frequency", 400000000);
    REQUIRE(t.get_property(f.cpu, "clock-frequency", d, len));
    CHECK_EQ(d[0], 0x17u);
    // Property order = definition order.
    CHECK(std::strcmp(t.next_property(f.ata, nullptr), "name") == 0);
    CHECK(std::strcmp(t.next_property(f.ata, ""), "name") == 0);
    CHECK(std::strcmp(t.next_property(f.ata, "name"), "reg") == 0);
    CHECK(std::strcmp(t.next_property(f.ata, "reg"), "device_type") == 0);
    CHECK(t.next_property(f.ata, "device_type") == nullptr);
    bool missing = false;
    CHECK(t.next_property(f.ata, "nope", &missing) == nullptr);
    CHECK(missing);
    CHECK(!t.set_int(f.ata, "a-property-name-longer-than-31-chars", 1));
}

TEST_CASE("firmware/devtree/paths_units_and_aliases")
{
    Fixture f;
    DeviceTree& t = *f.t;
    CHECK_EQ(t.find("/"), t.root());
    CHECK_EQ(t.find("/cpus/PowerPC,750@0"), f.cpu);
    CHECK_EQ(t.find("/cpus/PowerPC,750"), f.cpu);            // no unit: first by name
    CHECK_EQ(t.find("/pci@80000000/mac-io@10/ata@20000"), f.ata);
    CHECK_EQ(t.find("/pci/mac-io/ata"), f.ata);              // first "ata"
    CHECK(t.find("/pci/mac-io/ata@21000") != f.ata);
    CHECK(t.find("/pci/mac-io/ata@21000") != kNone);
    CHECK_EQ(t.find("/PCI@80000000"), kNone);                // names are case-sensitive
    CHECK_EQ(t.find("/pci@080000000/mac-io@10"), f.macio);   // leading zeros
    CHECK_EQ(t.find("/pci@80000000/mac-io@10/ata@2000A"), kNone);
    CHECK_EQ(t.find("/pci/mac-io/ata@20000:0,\\\\:tbxi"), f.ata);  // arguments ignored
    CHECK_EQ(t.find("//pci///mac-io"), f.macio);
    CHECK_EQ(t.find("/nothing"), kNone);
    CHECK_EQ(t.find(""), kNone);
    // Aliases.
    CHECK_EQ(t.find("hd"), f.disk);
    CHECK_EQ(t.find("hd:9"), f.disk);
    CHECK_EQ(t.find("macio/ata@20000"), f.ata);
    CHECK_EQ(t.find("nosuchalias"), kNone);
    // Paths.
    CHECK(path(t, t.root()) == "/");
    CHECK(path(t, f.disk) == "/pci@80000000/mac-io@10/ata@20000/disk@0");
    CHECK(path(t, f.aliases) == "/aliases");
    char small[8];
    const u32 full = t.path_of(f.disk, small, sizeof small);
    CHECK_EQ(full, 40u);                                     // length even when truncated
    CHECK(std::memcmp(small, "/pci@800", 8) == 0);
}

TEST_CASE("firmware/devtree/capacity_limits")
{
    auto t = std::make_unique<DeviceTree>();
    Phandle last = t->root();
    u32 made = 1;
    while (t->add_node(t->root(), "n") != kNone)
        ++made;
    CHECK_EQ(made, DeviceTree::kMaxNodes);
    CHECK(!t->ok());
    (void)last;
    auto t2 = std::make_unique<DeviceTree>();
    std::vector<u8> big(DeviceTree::kPoolSize);
    CHECK(!t2->set_property(t2->root(), "big", big.data(), static_cast<u32>(big.size())));
    CHECK(!t2->ok());
}

// --- client interface ------------------------------------------------------------------

namespace {

struct CiRig {
    Fixture f;
    std::vector<u8> ram = std::vector<u8>(0x10000);
    PhysicalBus bus;
    ClientInterface ci{*f.t, bus};
    u32 heap = 0x1000;
    CiRig() { bus.map_ram(0, ram.data(), static_cast<u32>(ram.size())); }
    u32 put_string(const char* s)
    {
        const u32 a = heap;
        for (size_t i = 0; i <= std::strlen(s); ++i)
            ram[heap++] = static_cast<u8>(s[i]);
        heap = (heap + 3) & ~3u;
        return a;
    }
    u32 rd(u32 a) const
    {
        return (static_cast<u32>(ram[a]) << 24) | (ram[a + 1] << 16) | (ram[a + 2] << 8) | ram[a + 3];
    }
    void wr(u32 a, u32 v)
    {
        ram[a] = static_cast<u8>(v >> 24);
        ram[a + 1] = static_cast<u8>(v >> 16);
        ram[a + 2] = static_cast<u8>(v >> 8);
        ram[a + 3] = static_cast<u8>(v);
    }
    // Like Linux prom_init's call_prom(service, nargs, nret, ...).
    u32 status = 0;
    std::vector<u32> call(const char* service, std::vector<u32> args, u32 nret)
    {
        const u32 a = 0x8000;
        wr(a, put_string(service));
        wr(a + 4, static_cast<u32>(args.size()));
        wr(a + 8, nret);
        for (size_t i = 0; i < args.size(); ++i)
            wr(a + 12 + 4 * static_cast<u32>(i), args[i]);
        for (u32 i = 0; i < nret; ++i)
            wr(a + 12 + 4 * static_cast<u32>(args.size() + i), 0xDEADBEEFu);
        status = ci.call(a);
        std::vector<u32> r;
        for (u32 i = 0; i < nret; ++i)
            r.push_back(rd(a + 12 + 4 * static_cast<u32>(args.size() + i)));
        return r;
    }
};

constexpr u32 F = ClientInterface::kFail;

} // namespace

TEST_CASE("firmware/client/tree_walk_like_linux_prom_init")
{
    CiRig r;
    // Linux scan_dt_build_strings/struct: child/peer walk from peer(0).
    const u32 root = r.call("peer", {0}, 1)[0];
    CHECK_EQ(r.status, 0u);
    CHECK_EQ(root, r.f.t->root());
    std::vector<std::string> names;
    std::vector<u32> stack{r.call("child", {root}, 1)[0]};
    while (!stack.empty()) {
        const u32 n = stack.back();
        stack.pop_back();
        if (n == 0)
            continue;
        const u32 buf = 0x4000;
        const u32 l = r.call("package-to-path", {n, buf, 256}, 1)[0];
        names.emplace_back(reinterpret_cast<const char*>(&r.ram[buf]), l);
        stack.push_back(r.call("peer", {n}, 1)[0]);
        stack.push_back(r.call("child", {n}, 1)[0]);
    }
    CHECK_EQ(names.size(), r.f.t->node_count() - 1);
    CHECK(names[0] == "/cpus");
    CHECK_EQ(r.call("parent", {root}, 1)[0], 0u);
    CHECK_EQ(r.call("child", {0x12345}, 1)[0], F);           // invalid phandle
    CHECK_EQ(r.call("peer", {0x12345}, 1)[0], F);
}

TEST_CASE("firmware/client/properties")
{
    CiRig r;
    const u32 ata = r.call("finddevice", {r.put_string("/pci/mac-io/ata@20000")}, 1)[0];
    CHECK_EQ(ata, r.f.ata);
    CHECK_EQ(r.call("finddevice", {r.put_string("/nope")}, 1)[0], F);
    CHECK_EQ(r.call("finddevice", {r.put_string("hd")}, 1)[0], r.f.disk);

    const u32 reg = r.put_string("reg");
    CHECK_EQ(r.call("getproplen", {ata, reg}, 1)[0], 16u);
    CHECK_EQ(r.call("getproplen", {ata, r.put_string("missing")}, 1)[0], F);
    // getprop copies at most buflen bytes but returns the full size.
    const u32 buf = 0x5000;
    CHECK_EQ(r.call("getprop", {ata, reg, buf, 8}, 1)[0], 16u);
    CHECK_EQ(r.rd(buf), 0x20000u);
    CHECK_EQ(r.rd(buf + 4), 0x1000u);
    CHECK_EQ(r.rd(buf + 8), 0u);                             // not copied
    CHECK_EQ(r.call("getprop", {ata, r.put_string("missing"), buf, 8}, 1)[0], F);

    // nextprop walks all names, then returns 0.
    std::vector<std::string> props;
    u32 prev = 0;  // null previous
    const u32 nb = 0x6000;
    for (;;) {
        const u32 flag = r.call("nextprop", {ata, prev, nb}, 1)[0];
        if (flag != 1)
            break;
        props.emplace_back(reinterpret_cast<const char*>(&r.ram[nb]));
        prev = r.put_string(props.back().c_str());
    }
    CHECK_EQ(props.size(), 3u);
    CHECK(props[2] == "device_type");
    CHECK_EQ(r.call("nextprop", {ata, r.put_string("bogus"), nb}, 1)[0], F);

    // setprop then read it back.
    const u32 val = r.put_string("hello");
    CHECK_EQ(r.call("setprop", {ata, r.put_string("greeting"), val, 6}, 1)[0], 6u);
    CHECK_EQ(r.call("getproplen", {ata, r.put_string("greeting")}, 1)[0], 6u);
    CHECK_EQ(r.call("setprop", {0x42, r.put_string("x"), val, 6}, 1)[0], F);
}

TEST_CASE("firmware/client/paths_test_clock_and_unknown")
{
    CiRig r;
    const u32 buf = 0x4000;
    CHECK_EQ(r.call("canon", {r.put_string("hd"), buf, 100}, 1)[0], 40u);
    CHECK(std::string(reinterpret_cast<const char*>(&r.ram[buf])) ==
          "/pci@80000000/mac-io@10/ata@20000/disk@0");
    r.ram[buf + 1] = 0xEE;
    CHECK_EQ(r.call("package-to-path", {r.f.t->root(), buf, 1}, 1)[0], 1u);
    CHECK_EQ(r.ram[buf], '/');
    CHECK_EQ(r.ram[buf + 1], 0xEEu);                         // no NUL beyond buflen

    CHECK_EQ(r.call("test", {r.put_string("getprop")}, 1)[0], 0u);
    CHECK_EQ(r.call("test", {r.put_string("frobnicate")}, 1)[0], F);
    r.call("frobnicate", {}, 0);
    CHECK_EQ(r.status, F);
    CHECK_EQ(r.ci.unknown_calls(), 1u);
    CHECK(std::strcmp(r.ci.last_service(), "frobnicate") == 0);

    u32 ms = 1234;
    r.ci.set_clock([](void* c) { return *static_cast<u32*>(c); }, &ms);
    CHECK_EQ(r.call("milliseconds", {}, 1)[0], 1234u);
    int exits = 0;
    r.ci.set_exit([](void* c) { ++*static_cast<int*>(c); }, &exits);
    r.call("exit", {}, 0);
    CHECK_EQ(exits, 1);
    CHECK_EQ(r.status, 0u);
    // Too few return cells for a service: rejected as an invalid call.
    r.call("getproplen", {r.f.ata, r.put_string("reg")}, 0);
    CHECK_EQ(r.status, F);
}
