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
    CHECK_EQ(t.find("/cpus/@0"), f.cpu);                     // no name: by unit address
    CHECK_EQ(t.find("/cpus/@1"), kNone);
    CHECK_EQ(t.find("/cpus/@"), kNone);
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
    // quiesce (Apple; Mac OS ROM's loader calls it before handing over): no arguments.
    CHECK_EQ(r.call("test", {r.put_string("quiesce")}, 1)[0], 0u);
    r.call("quiesce", {}, 0);
    CHECK_EQ(r.status, 0u);
    // Too few return cells for a service: rejected as an invalid call.
    r.call("getproplen", {r.f.ata, r.put_string("reg")}, 0);
    CHECK_EQ(r.status, F);
}

// --- reaching the client interface from guest code ---------------------------------

#include "core/firmware/openfirm/firmware.h"
#include "tests/unit/cpu/ppc_asm.h"

TEST_CASE("firmware/client/called_from_guest_code")
{
    using namespace g3::ppc;
    using namespace asm_ppc;
    CiRig r;
    Firmware fw(*r.f.t, r.bus);
    REQUIRE(fw.install(0x3000));
    CHECK_EQ(r.rd(0x3000), Firmware::kTrapClientInterface);
    Ppc750 cpu(r.bus);
    fw.attach(cpu);

    // Argument array for finddevice("/cpus/PowerPC,750"), 1 return.
    const u32 args = 0x8000;
    r.wr(args, r.put_string("finddevice"));
    r.wr(args + 4, 1);
    r.wr(args + 8, 1);
    r.wr(args + 12, r.put_string("/cpus/PowerPC,750"));
    // A second call naming an unknown service.
    const u32 args2 = 0x8100;
    r.wr(args2, r.put_string("no-such-service"));
    r.wr(args2 + 4, 0);
    r.wr(args2 + 8, 0);

    // The client: r3 = args; ctr = entry; bctrl; save r3; same again; loop.
    const u32 prog[] = {
        lis(3, args >> 16), ori(3, 3, args & 0xFFFF),
        lis(9, 0), ori(9, 9, 0x3000), mtspr(spr::CTR, 9),
        bctr() | 1,                      // bctrl
        stw(3, 0x100, 0),                // status of call 1 -> 0x100
        lis(3, 0), ori(3, 3, args2 & 0xFFFF),
        bctr() | 1,
        stw(3, 0x104, 0),                // status of call 2 -> 0x104
        b(0),
    };
    for (u32 i = 0; i < sizeof prog / 4; ++i)
        r.wr(0x2000 + 4 * i, prog[i]);
    r.wr(0x100, 0x11111111u);
    r.wr(0x104, 0x11111111u);
    cpu.reset();
    cpu.state().msr = 0;                 // real mode, vectors at 0
    cpu.state().pc = 0x2000;
    cpu.run(40);
    CHECK_EQ(fw.calls(), 2u);
    CHECK_EQ(r.rd(0x100), 0u);
    CHECK_EQ(r.rd(args + 16), r.f.cpu);  // phandle returned in the array
    CHECK_EQ(r.rd(0x104), ClientInterface::kFail);
    CHECK_EQ(cpu.state().pc, 0x2000u + 4 * (sizeof prog / 4 - 1));  // at the final loop
}

TEST_CASE("firmware/client/exit_does_not_return")
{
    using namespace g3::ppc;
    using namespace asm_ppc;
    CiRig r;
    Firmware fw(*r.f.t, r.bus);
    REQUIRE(fw.install(0x3000));
    CHECK_EQ(r.rd(0x3004), Firmware::kTrapIdle);
    Ppc750 cpu(r.bus);
    fw.attach(cpu);
    const u32 args = 0x8000;
    r.wr(args, r.put_string("exit"));
    r.wr(args + 4, 0);
    r.wr(args + 8, 0);
    const u32 prog[] = {
        lis(3, args >> 16), ori(3, 3, args & 0xFFFF),
        lis(9, 0), ori(9, 9, 0x3000), mtspr(spr::CTR, 9),
        bctr() | 1,                      // bctrl: exit
        stw(3, 0x100, 0),                // must not run
        b(0),
    };
    for (u32 i = 0; i < sizeof prog / 4; ++i)
        r.wr(0x2000 + 4 * i, prog[i]);
    r.wr(0x100, 0x11111111u);
    cpu.reset();
    cpu.state().msr = 0;
    cpu.state().pc = 0x2000;
    CHECK(!fw.client_exited());
    cpu.run(50);
    CHECK(fw.client_exited());
    CHECK_EQ(cpu.state().pc, 0x3004u);   // parked on the idle trap
    CHECK_EQ(r.rd(0x100), 0x11111111u);  // the code after the call never ran
    CHECK(!cpu.checkstopped());
}

TEST_CASE("firmware/client/other_opcode0_words_still_fault")
{
    using namespace g3::ppc;
    CiRig r;
    Firmware fw(*r.f.t, r.bus);
    Ppc750 cpu(r.bus);
    fw.attach(cpu);
    for (u32 word : {0x00000000u, Firmware::kTrapBase | 0x03, 0x00004F01u | 0x00010000u}) {
        r.wr(0x2000, word);
        cpu.reset();
        cpu.state().msr = 0;
        cpu.state().pc = 0x2000;
        cpu.step();
        CHECK_EQ(cpu.state().pc, 0x700u);                // program exception
        CHECK_EQ(cpu.state().spr[spr::SRR0], 0x2000u);
        CHECK(cpu.state().spr[spr::SRR1] & srr1::ILLEGAL);
    }
    CHECK_EQ(fw.calls(), 0u);
}

// --- memory map ------------------------------------------------------------------------

TEST_CASE("firmware/memory/claim_release")
{
    MemoryMap m;
    REQUIRE(m.add_free(0x00000000, 0x00400000));   // 4 MB
    CHECK_EQ(m.claim(0x00100000, 0x1000, 0), 0x00100000u);      // exact
    CHECK_EQ(m.range_count(), 2u);
    CHECK_EQ(m.claim(0x00100800, 0x100, 0), MemoryMap::kFail);  // already taken
    CHECK_EQ(m.claim(0x003FF000, 0x2000, 0), MemoryMap::kFail);  // runs past the end
    // Aligned claims: lowest fit.
    CHECK_EQ(m.claim(0, 0x10, 0x1000), 0x00000000u);
    CHECK_EQ(m.claim(0, 0x10, 0x1000), 0x00001000u);
    CHECK_EQ(m.claim(0, 0x100000, 0x100000), 0x00200000u);      // 0x100000 is partly taken
    CHECK_EQ(m.claim(0, 0x10, 3), MemoryMap::kFail);            // align not a power of two
    CHECK_EQ(m.claim(0, 0, 0x10), MemoryMap::kFail);            // empty
    CHECK(!m.is_free(0x00200000, 1));
    // Release merges neighbours again.
    CHECK(m.release(0x00200000, 0x100000));
    CHECK(m.release(0x00100000, 0x1000));
    CHECK(m.release(0x00000000, 0x10));
    CHECK(m.release(0x00001000, 0x10));
    CHECK_EQ(m.range_count(), 1u);
    CHECK_EQ(m.free_bytes(), 0x00400000u);
    CHECK(!m.release(0x00001000, 0x10));                        // double free refused
    CHECK(m.is_free(0, 0x00400000));
    // Top of the 32-bit space.
    MemoryMap hi;
    REQUIRE(hi.add_free(0xFFFF0000u, 0x10000));
    CHECK_EQ(hi.claim(0, 0x10000, 0x10000), 0xFFFF0000u);
    CHECK_EQ(hi.range_count(), 0u);
}

// --- instances through the client interface --------------------------------------------

namespace {

struct InstRig : CiRig {
    std::vector<u8> image = std::vector<u8>(64 * 512);
    RamBlockDevice disk{image.data(), image.size()};
    DiskDriver disk_driver{disk};
    ConsoleDriver console;
    std::string out;
    InstanceTable inst;
    MemoryMap mem;
    Phandle stdout_node;
    InstRig()
    {
        for (size_t i = 0; i < image.size(); ++i)
            image[i] = static_cast<u8>(i * 7 + (i >> 9) * 13);  // no short-period aliasing
        stdout_node = f.t->add_node(f.macio, "escc", "13000");
        inst.bind(f.disk, &disk_driver);
        inst.bind(stdout_node, &console);
        console.set_sink([](void* c, const char* t, u32 n) {
            static_cast<std::string*>(c)->append(t, n);
        }, &out);
        mem.add_free(0x9000, 0x7000);
        ci.set_instances(&inst);
        ci.set_memory(&mem);
    }
};

} // namespace

TEST_CASE("firmware/client/open_read_seek_write_close")
{
    InstRig r;
    // No partition number: the whole disk (this image has no partition map).
    CHECK_EQ(r.call("open", {r.put_string("hd:9,\\\\:tbxi")}, 1)[0], 0u);
    const u32 ih = r.call("open", {r.put_string("hd:,\\\\:tbxi")}, 1)[0];
    REQUIRE(ih != 0);
    CHECK(std::string(r.inst.args(ih)) == ",\\\\:tbxi");
    CHECK_EQ(r.call("instance-to-package", {ih}, 1)[0], r.f.disk);
    const u32 buf = 0x4000;
    CHECK_EQ(r.call("instance-to-path", {ih, buf, 200}, 1)[0], 49u);  // 40 + ":" + 8
    CHECK(std::string(reinterpret_cast<const char*>(&r.ram[buf])) ==
          "/pci@80000000/mac-io@10/ata@20000/disk@0:,\\\\:tbxi");

    // seek to an odd offset, read across a sector boundary.
    CHECK_EQ(r.call("seek", {ih, 0, 1000}, 1)[0], 0u);
    CHECK_EQ(r.call("read", {ih, buf, 700}, 1)[0], 700u);
    CHECK_EQ(r.ram[buf], r.image[1000]);
    CHECK_EQ(r.ram[buf + 699], r.image[1699]);
    // Reading past the end returns what's there.
    CHECK_EQ(r.call("seek", {ih, 0, 64 * 512 - 10}, 1)[0], 0u);
    CHECK_EQ(r.call("read", {ih, buf, 100}, 1)[0], 10u);
    CHECK_EQ(r.call("read", {ih, buf, 100}, 1)[0], 0u);
    CHECK_EQ(r.call("seek", {ih, 0, 64 * 512 + 1}, 1)[0], ClientInterface::kFail);
    CHECK_EQ(r.call("seek", {ih, 1, 0}, 1)[0], ClientInterface::kFail);   // 4 GB: beyond

    // Write a partial sector (read-modify-write keeps the neighbours).
    for (u32 i = 0; i < 4; ++i)
        r.ram[buf + i] = static_cast<u8>(0xF0 + i);
    const u8 before = r.image[2047];
    const u8 after = r.image[2052];                      // same sector as the write
    CHECK_EQ(r.call("seek", {ih, 0, 2048}, 1)[0], 0u);
    CHECK_EQ(r.call("write", {ih, buf, 4}, 1)[0], 4u);
    CHECK_EQ(r.image[2048], 0xF0u);
    CHECK_EQ(r.image[2051], 0xF3u);
    CHECK_EQ(r.image[2047], before);
    CHECK_EQ(r.image[2052], after);

    // A second instance has its own position.
    const u32 ih2 = r.call("open", {r.put_string("/pci/mac-io/ata/disk")}, 1)[0];
    REQUIRE(ih2 != 0);
    CHECK(ih2 != ih);
    CHECK_EQ(r.call("read", {ih2, buf, 4}, 1)[0], 4u);
    CHECK(std::memcmp(&r.ram[buf], &r.image[0], 4) == 0);
    CHECK_EQ(r.call("read", {ih, buf, 4}, 1)[0], 4u);    // ih continues after its write
    CHECK(std::memcmp(&r.ram[buf], &r.image[2052], 4) == 0);
    CHECK_EQ(r.inst.open_count(), 2u);
    r.call("close", {ih}, 0);
    CHECK_EQ(r.inst.open_count(), 1u);
    CHECK_EQ(r.call("read", {ih, buf, 4}, 1)[0], ClientInterface::kFail);    // closed
    CHECK_EQ(r.call("instance-to-package", {ih}, 1)[0], ClientInterface::kFail);
    // No driver / no such node: open fails with 0.
    CHECK_EQ(r.call("open", {r.put_string("/cpus")}, 1)[0], 0u);
    CHECK_EQ(r.call("open", {r.put_string("/nowhere")}, 1)[0], 0u);
}

TEST_CASE("firmware/client/console_claim_and_test")
{
    InstRig r;
    const u32 con = r.call("open", {r.put_string("/pci/mac-io/escc")}, 1)[0];
    REQUIRE(con != 0);
    const u32 msg = r.put_string("Hello from the guest\n");
    CHECK_EQ(r.call("write", {con, msg, 21}, 1)[0], 21u);
    CHECK(r.out == "Hello from the guest\n");
    CHECK_EQ(r.call("read", {con, 0x4000, 10}, 1)[0], 0u);           // no input
    // A large write goes through the bounce buffer in pieces.
    std::vector<u8> big(1500, 'x');
    std::memcpy(&r.ram[0x5000], big.data(), big.size());
    CHECK_EQ(r.call("write", {con, 0x5000, 1500}, 1)[0], 1500u);
    CHECK_EQ(r.out.size(), 21u + 1500u);

    CHECK_EQ(r.call("claim", {0, 0x1000, 0x1000}, 1)[0], 0x9000u);  // lowest aligned fit
    CHECK_EQ(r.call("claim", {0, 0x10, 0x1000}, 1)[0], 0xA000u);
    CHECK_EQ(r.call("claim", {0xB000, 0x100, 0}, 1)[0], 0xB000u);
    CHECK_EQ(r.call("claim", {0xB080, 0x100, 0}, 1)[0], ClientInterface::kFail);
    r.call("release", {0xB000, 0x100}, 0);
    CHECK_EQ(r.call("claim", {0xB080, 0x100, 0}, 1)[0], 0xB080u);

    CHECK_EQ(r.call("test", {r.put_string("claim")}, 1)[0], 0u);
    CHECK_EQ(r.call("test", {r.put_string("instance-to-path")}, 1)[0], 0u);
    CiRig bare;  // without memory map and instances these are missing
    CHECK_EQ(bare.call("test", {bare.put_string("claim")}, 1)[0], ClientInterface::kFail);
    CHECK_EQ(bare.call("test", {bare.put_string("open")}, 1)[0], ClientInterface::kFail);
    bare.call("open", {bare.put_string("/")}, 1);
    CHECK_EQ(bare.status, ClientInterface::kFail);
}

// --- Apple Partition Map ---------------------------------------------------------------

#include "core/storage/apple_partition_map.h"

namespace {

void be16(std::vector<u8>& d, size_t at, u16 v) { d[at] = static_cast<u8>(v >> 8); d[at + 1] = static_cast<u8>(v); }
void be32(std::vector<u8>& d, size_t at, u32 v)
{
    be16(d, at, static_cast<u16>(v >> 16));
    be16(d, at + 2, static_cast<u16>(v));
}
// A disk image laid out like Inside Macintosh's description: driver
// descriptor map in block 0, map entries from block 1.
std::vector<u8> apm_image(u32 blocks)
{
    std::vector<u8> d(static_cast<size_t>(blocks) * 512);
    for (size_t i = 0; i < d.size(); ++i)
        d[i] = static_cast<u8>(i * 7 + (i >> 9) * 13);
    std::memset(d.data(), 0, 512);
    be16(d, 0, 0x4552);                 // 'ER'
    be16(d, 2, 512);                    // sbBlkSize
    be32(d, 4, blocks);                 // sbBlkCount
    struct E { u32 start, count; const char* name; const char* type; };
    const E e[] = {{1, 63, "Apple", "Apple_partition_map"},
                   {64, 32, "Macintosh", "Apple_HFS"},
                   {96, 1000, "Extra", "Apple_Free"}};  // runs past the image: clipped
    for (u32 i = 0; i < 3; ++i) {
        const size_t b = 512u * (1 + i);
        std::memset(&d[b], 0, 512);
        be16(d, b, 0x504D);             // 'PM'
        be32(d, b + 4, 3);              // pmMapBlkCnt
        be32(d, b + 8, e[i].start);     // pmPyPartStart
        be32(d, b + 12, e[i].count);    // pmPartBlkCnt
        std::memcpy(&d[b + 16], e[i].name, std::strlen(e[i].name));
        std::memcpy(&d[b + 48], e[i].type, std::strlen(e[i].type));
        be32(d, b + 88, 0x37);          // pmPartStatus
    }
    return d;
}

} // namespace

TEST_CASE("storage/apm/read_map")
{
    std::vector<u8> img = apm_image(128);
    RamBlockDevice disk(img.data(), img.size());
    ApplePartitionMap map;
    REQUIRE(map.read(disk));
    CHECK_EQ(map.count(), 3u);
    CHECK(std::strcmp(map.partition(1)->type, "Apple_partition_map") == 0);
    CHECK_EQ(map.partition(2)->start, 64u);
    CHECK_EQ(map.partition(2)->blocks, 32u);
    CHECK(std::strcmp(map.partition(2)->name, "Macintosh") == 0);
    CHECK_EQ(map.partition(2)->status, 0x37u);
    CHECK(map.partition(0) == nullptr);
    CHECK(map.partition(4) == nullptr);
    CHECK_EQ(map.find_type("Apple_HFS"), 2u);
    CHECK_EQ(map.find_type("Apple_UFS"), 0u);
    // Not a map: bad signature, or a block size other than 512 or 2048
    // (2048: a CD, whose map is still in 512-byte entries; read).
    std::vector<u8> bad = img;
    bad[0] = 0;
    RamBlockDevice bd(bad.data(), bad.size());
    CHECK(!map.read(bd));
    std::vector<u8> cd = img;
    be16(cd, 2, 2048);
    RamBlockDevice cdd(cd.data(), cd.size());
    CHECK(map.read(cdd));
    be16(cd, 2, 4096);
    CHECK(!map.read(cdd));
    CHECK(map.read(disk));  // (map is used again below)
    // A partition window.
    BlockWindow w(disk, 64, 32);
    u8 sec[512];
    CHECK(w.read(0, 1, sec));
    CHECK_EQ(sec[0], img[64 * 512]);
    CHECK(!w.read(32, 1, sec));
    CHECK(!w.read(31, 2, sec));
}

TEST_CASE("firmware/client/open_partition")
{
    InstRig r;
    r.image = apm_image(128);
    RamBlockDevice disk(r.image.data(), r.image.size());
    DiskDriver drv(disk);
    r.inst.bind(r.f.disk, &drv);
    // A file part needs a file system: this partition has none.
    CHECK_EQ(r.call("open", {r.put_string("hd:2,\\\\:tbxi")}, 1)[0], 0u);
    const u32 ih = r.call("open", {r.put_string("hd:2")}, 1)[0];
    REQUIRE(ih != 0);
    CHECK_EQ(drv.partition(r.inst.slot(ih)), 2u);
    CHECK_EQ(drv.window_size(r.inst.slot(ih)), 32u * 512);
    const u32 buf = 0x4000;
    CHECK_EQ(r.call("read", {ih, buf, 16}, 1)[0], 16u);
    CHECK(std::memcmp(&r.ram[buf], &r.image[64 * 512], 16) == 0);   // offset 0 = partition start
    CHECK_EQ(r.call("seek", {ih, 0, 32 * 512}, 1)[0], 0u);           // end of partition
    CHECK_EQ(r.call("read", {ih, buf, 16}, 1)[0], 0u);
    CHECK_EQ(r.call("seek", {ih, 0, 32 * 512 + 1}, 1)[0], ClientInterface::kFail);
    // Writes stay inside the partition.
    r.ram[buf] = 0xAB;
    CHECK_EQ(r.call("seek", {ih, 0, 32 * 512 - 1}, 1)[0], 0u);
    CHECK_EQ(r.call("write", {ih, buf, 4}, 1)[0], 1u);
    CHECK_EQ(r.image[96 * 512 - 1], 0xABu);
    // Partition 3 is clipped to the image; 4 doesn't exist; whole disk without a number.
    const u32 ih3 = r.call("open", {r.put_string("hd:3")}, 1)[0];
    REQUIRE(ih3 != 0);
    CHECK_EQ(drv.window_size(r.inst.slot(ih3)), 32u * 512);
    CHECK_EQ(r.call("open", {r.put_string("hd:4")}, 1)[0], 0u);
    CHECK_EQ(r.call("open", {r.put_string("hd:99999")}, 1)[0], 0u);
    const u32 whole = r.call("open", {r.put_string("hd")}, 1)[0];
    REQUIRE(whole != 0);
    CHECK_EQ(drv.window_size(r.inst.slot(whole)), 128u * 512);
}

// --- files on an HFS Plus partition --------------------------------------------------

#include "tests/unit/storage/hfs_image.h"

namespace {

// Disk: partition map (2 entries) + the test HFS Plus volume as partition 2.
std::vector<u8> bootable_disk(const std::vector<u8>& volume)
{
    const u32 hfs_start = 64, hfs_blocks = static_cast<u32>((volume.size() + 511) / 512);
    std::vector<u8> d(static_cast<size_t>(hfs_start + hfs_blocks) * 512);
    be16(d, 0, 0x4552);
    be16(d, 2, 512);
    be32(d, 4, hfs_start + hfs_blocks);
    const char* types[] = {"Apple_partition_map", "Apple_HFS"};
    const u32 starts[] = {1, hfs_start}, counts[] = {63, hfs_blocks};
    for (u32 i = 0; i < 2; ++i) {
        const size_t b = 512u * (1 + i);
        be16(d, b, 0x504D);
        be32(d, b + 4, 2);
        be32(d, b + 8, starts[i]);
        be32(d, b + 12, counts[i]);
        std::memcpy(&d[b + 48], types[i], std::strlen(types[i]));
    }
    std::memcpy(&d[hfs_start * 512], volume.data(), volume.size());
    return d;
}

} // namespace

TEST_CASE("firmware/client/open_boot_file_on_hfsplus")
{
    InstRig r;
    hfs_image::Volume vol;
    std::vector<u8> img = bootable_disk(vol.img.d);
    RamBlockDevice disk(img.data(), img.size());
    DiskDriver drv(disk);
    r.inst.bind(r.f.disk, &drv);

    // The Mac OS boot path: blessed folder, file of type 'tbxi'.
    const u32 ih = r.call("open", {r.put_string("hd:2,\\\\:tbxi")}, 1)[0];
    REQUIRE(ih != 0);
    CHECK(drv.is_file(r.inst.slot(ih)));
    std::vector<u8> got(12000, 0xEE);
    const u32 buf = 0x2000;  // RAM is 64 KB; the file is 10000 bytes
    CHECK_EQ(r.call("read", {ih, buf, 12000}, 1)[0], static_cast<u32>(hfs_image::Volume::kRomSize));
    CHECK(std::memcmp(&r.ram[buf], vol.rom.data(), vol.rom.size()) == 0);
    CHECK_EQ(r.call("read", {ih, buf, 10}, 1)[0], 0u);             // end of file
    CHECK_EQ(r.call("seek", {ih, 0, 5000}, 1)[0], 0u);
    CHECK_EQ(r.call("read", {ih, buf, 4}, 1)[0], 4u);
    CHECK(std::memcmp(&r.ram[buf], &vol.rom[5000], 4) == 0);
    CHECK_EQ(r.call("seek", {ih, 0, 10001}, 1)[0], ClientInterface::kFail);  // beyond the file
    CHECK_EQ(r.call("write", {ih, buf, 4}, 1)[0], ClientInterface::kFail);   // read-only

    // A path from the root, case-insensitive; a fragmented file.
    const u32 ih2 = r.call("open", {r.put_string("hd:2,\\system folder\\Fragmented")}, 1)[0];
    REQUIRE(ih2 != 0);
    CHECK_EQ(r.call("seek", {ih2, 0, 9 * 4096 + 7}, 1)[0], 0u);    // in the overflow extents
    CHECK_EQ(r.call("read", {ih2, buf, 16}, 1)[0], 16u);
    CHECK(std::memcmp(&r.ram[buf], &vol.frag[9 * 4096 + 7], 16) == 0);
    // Reading the first instance again after the second moved the volume's caches.
    CHECK_EQ(r.call("seek", {ih, 0, 0}, 1)[0], 0u);
    CHECK_EQ(r.call("read", {ih, buf, 8}, 1)[0], 8u);
    CHECK(std::memcmp(&r.ram[buf], vol.rom.data(), 8) == 0);

    // Failures: no such file or type, a folder, a file mid-path, bad type length,
    // a non-HFS partition.
    for (const char* bad : {"hd:2,\\\\:zzzz", "hd:2,\\System Folder\\Nope", "hd:2,\\System Folder",
                            "hd:2,\\ReadMe\\x", "hd:2,\\ReadMe\\:TEXT", "hd:2,\\\\:tbx",
                            "hd:2,\\\\:tbxix", "hd:1,\\\\:tbxi"})
        CHECK_EQ(r.call("open", {r.put_string(bad)}, 1)[0], 0u);
    // The raw partition is still available without a file part.
    const u32 raw = r.call("open", {r.put_string("hd:2")}, 1)[0];
    REQUIRE(raw != 0);
    CHECK(!drv.is_file(r.inst.slot(raw)));
    CHECK_EQ(r.call("seek", {raw, 0, 1024}, 1)[0], 0u);
    CHECK_EQ(r.call("read", {raw, buf, 2}, 1)[0], 2u);
    CHECK_EQ(r.ram[buf], 0x48u);                                    // 'H+' volume header
}

TEST_CASE("firmware/client/open_boot_file_on_classic_hfs")
{
    InstRig r;
    hfs_image::ClassicVolume vol;
    std::vector<u8> img = bootable_disk(vol.img.d);
    RamBlockDevice disk(img.data(), img.size());
    DiskDriver drv(disk);
    r.inst.bind(r.f.disk, &drv);
    const u32 ih = r.call("open", {r.put_string("hd:2,\\\\:tbxi")}, 1)[0];
    REQUIRE(ih != 0);
    const u32 buf = 0x2000;
    CHECK_EQ(r.call("read", {ih, buf, 8000}, 1)[0], static_cast<u32>(hfs_image::ClassicVolume::kRomSize));
    CHECK(std::memcmp(&r.ram[buf], vol.rom.data(), vol.rom.size()) == 0);
}
