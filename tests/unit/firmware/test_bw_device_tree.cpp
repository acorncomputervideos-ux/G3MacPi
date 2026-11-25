// SPDX-License-Identifier: GPL-3.0-or-later
// The B&W device tree, checked the way Linux would use it, against the
// emulator's own hardware constants.

#include "core/common/endian.h"
#include "core/devices/macio/dbdma.h"
#include "core/devices/macio/heathrow_pic.h"
#include "core/devices/macio/nvram.h"
#include "core/devices/macio/paddington.h"
#include "core/firmware/openfirm/bw_device_tree.h"
#include "tests/framework/g3test.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace g3;
using namespace g3::of;

namespace {

std::vector<u32> cells(const DeviceTree& t, Phandle h, const char* name)
{
    const u8* d = nullptr;
    u32 len = 0;
    std::vector<u32> v;
    if (!t.get_property(h, name, d, len))
        return v;
    for (u32 i = 0; i + 4 <= len; i += 4)
        v.push_back((static_cast<u32>(d[i]) << 24) | (d[i + 1] << 16) | (d[i + 2] << 8) | d[i + 3]);
    return v;
}

u32 cell_or(const DeviceTree& t, Phandle h, const char* name, u32 dflt)
{
    const auto v = cells(t, h, name);
    return v.empty() ? dflt : v[0];
}

std::string str(const DeviceTree& t, Phandle h, const char* name)
{
    const u8* d = nullptr;
    u32 len = 0;
    if (!t.get_property(h, name, d, len) || len == 0)
        return {};
    return std::string(reinterpret_cast<const char*>(d), len - 1);
}

bool compatible_with(const DeviceTree& t, Phandle h, const std::string& c)
{
    const u8* d = nullptr;
    u32 len = 0;
    if (!t.get_property(h, "compatible", d, len))
        return false;
    std::string all(reinterpret_cast<const char*>(d), len);
    size_t pos = 0;
    while (pos < all.size()) {
        const size_t end = all.find('\0', pos);
        if (all.substr(pos, end - pos) == c)
            return true;
        pos = end + 1;
    }
    return false;
}

// Finds a node by name (and compatible, if given), like for_each_node_by_name.
Phandle find_by_name(const DeviceTree& t, Phandle from, const char* name, const char* compat = nullptr)
{
    for (Phandle c = t.child(from); c != kNone; c = t.peer(c)) {
        if (std::strcmp(t.name(c), name) == 0 && (!compat || compatible_with(t, c, compat)))
            return c;
        const Phandle deeper = find_by_name(t, c, name, compat);
        if (deeper != kNone)
            return deeper;
    }
    return kNone;
}

bool is_pci_bus(const DeviceTree& t, Phandle h) { return str(t, h, "device_type") == "pci"; }

// Translates an address of `node` (in its parent's address format) to a CPU
// address, following Linux's of_translate_address: walk up through the
// parents' "ranges"; a missing "ranges" maps 1:1 on Macs (root compatible
// "MacRISC"), as Linux's of_empty_ranges_quirk does.
bool translate(const DeviceTree& t, Phandle node, std::vector<u32> addr, u32& out)
{
    Phandle bus = t.parent(node);
    while (bus != t.root()) {
        const Phandle up = t.parent(bus);
        const u32 na = cell_or(t, bus, "#address-cells", 2);
        const u32 pna = cell_or(t, up, "#address-cells", 2);
        const u32 ns = cell_or(t, bus, "#size-cells", 1);
        const auto ranges = cells(t, bus, "ranges");
        if (ranges.empty()) {
            // 1:1: keep the low cells.
            std::vector<u32> next(pna, 0);
            for (u32 i = 0; i < pna && i < na; ++i)
                next[pna - 1 - i] = addr[na - 1 - i];
            if (is_pci_bus(t, up))
                next[0] = addr[0];  // keep the PCI space type
            addr = next;
        } else {
            bool found = false;
            const u32 entry = na + pna + ns;
            for (size_t r = 0; r + entry <= ranges.size(); r += entry) {
                const u32 child_lo = ranges[r + na - 1];
                const u32 size = ranges[r + na + pna + ns - 1];
                const bool space_ok = !is_pci_bus(t, bus) || na < 3 ||
                                      ((ranges[r] ^ addr[0]) & 0x03000000u) == 0;
                const u32 a = addr[na - 1];
                if (!space_ok || a < child_lo || a - child_lo >= size)
                    continue;
                std::vector<u32> next(ranges.begin() + static_cast<long>(r + na),
                                      ranges.begin() + static_cast<long>(r + na + pna));
                next[pna - 1] += a - child_lo;
                addr = next;
                found = true;
                break;
            }
            if (!found)
                return false;
        }
        bus = up;
    }
    out = addr.back();
    return true;
}

struct Built {
    std::unique_ptr<DeviceTree> t = std::make_unique<DeviceTree>();
    BwMachine m;
    BwNodes n;
    bool ok;
    Built()
    {
        m.ram_size = 128u << 20;
        m.pvr = 0x00083214u;
        const u32 io[5] = {0x1440, 0x1430, 0x1448, 0x1434, 0x1400};  // as the firmware assigns
        for (u32 i = 0; i < 5; ++i)
            m.cmd646_io[i] = io[i];
        ok = build_bw_device_tree(*t, m, n);
    }
};

} // namespace

TEST_CASE("firmware/bw-tree/machine_identity_and_cpu")
{
    Built b;
    REQUIRE(b.ok);
    const DeviceTree& t = *b.t;
    CHECK(str(t, t.root(), "model") == "PowerMac1,1");            // Linux pmac_mb_defs
    CHECK(compatible_with(t, t.root(), "MacRISC"));               // Linux of_empty_ranges_quirk
    // The check Apple's Mac OS ROM loader makes: starts with "Copyright ",
    // contains " Apple Computer, Inc." (the two strings in MacOS.elf).
    const std::string c = str(t, t.root(), "copyright");
    CHECK(c.rfind("Copyright ", 0) == 0);
    CHECK(c.find(" Apple Computer, Inc.") != std::string::npos);
    // NewWorld: some node has an "interrupt-controller" property (Linux setup.c):
    // Paddington's PIC, a child of mac-io.
    CHECK(t.has_property(find_by_name(t, t.root(), "interrupt-controller"), "interrupt-controller"));
    const Phandle cpu = b.n.cpu;
    CHECK(str(t, cpu, "device_type") == "cpu");
    CHECK_EQ(cell_or(t, cpu, "clock-frequency", 0), 350000000u);
    CHECK_EQ(cell_or(t, cpu, "timebase-frequency", 0), 25000000u);  // bus / 4, as emulated
    CHECK_EQ(cell_or(t, cpu, "cpu-version", 0), 0x00083214u);
    CHECK_EQ(cell_or(t, cpu, "d-cache-size", 0), 32768u);
    CHECK(cells(t, b.n.memory, "reg") == std::vector<u32>({0, 128u << 20}));
    CHECK(str(t, b.n.memory, "device_type") == "memory");
    // Grackle: named "pci" with a bus-range (Linux pmac_add_bridge).
    const Phandle pci = find_by_name(t, t.root(), "pci");
    REQUIRE(pci != kNone);
    CHECK(cells(t, pci, "bus-range") == std::vector<u32>({0, 1}));
    CHECK_EQ(t.find("hd"), b.n.disk[0]);
    char path[128];
    t.path_of(b.n.disk[0], path, sizeof path);
    CHECK(std::string(path) == "/pci@80000000/pci-bridge@d/mac-io@5/ata@20000/disk@0");
}

TEST_CASE("firmware/bw-tree/macio_addresses_match_the_emulated_hardware")
{
    Built b;
    REQUIRE(b.ok);
    const DeviceTree& t = *b.t;
    // Linux probe_one_macio: name "mac-io", compatible "paddington", base
    // from assigned-addresses, translated through the parents.
    const Phandle mio = find_by_name(t, t.root(), "mac-io", "paddington");
    REQUIRE(mio == b.n.macio);
    const auto aa = cells(t, mio, "assigned-addresses");
    REQUIRE(aa.size() == 5);
    CHECK_EQ(aa[0] & 0x03000000u, 0x02000000u);                   // 32-bit memory space
    CHECK_EQ((aa[0] >> 16) & 0xFF, 1u);                           // bus 1
    CHECK_EQ((aa[0] >> 11) & 0x1F, 5u);                           // device 5
    CHECK_EQ(aa[0] & 0xFF, 0x10u);                                // BAR 0
    CHECK_EQ(aa[4], Paddington::kBarSize);
    u32 base = 0;
    REQUIRE(translate(t, mio, {aa[0], aa[1], aa[2]}, base));
    CHECK_EQ(base, b.m.macio_base);

    auto cell_addr = [&](Phandle h, u32 index) {
        const auto reg = cells(t, h, "reg");
        u32 a = 0;
        if (2 * index + 1 >= reg.size() || !translate(t, h, {reg[2 * index]}, a))
            return 0xFFFFFFFFu;
        return a;
    };
    // via-cuda (Linux find_via_cuda: by name, reg 0) and its interrupt.
    REQUIRE(find_by_name(t, t.root(), "via-cuda") == b.n.cuda);
    CHECK_EQ(cell_addr(b.n.cuda, 0), base + Paddington::kViaOffset);
    CHECK(cells(t, b.n.cuda, "interrupts") == std::vector<u32>({Paddington::kViaIrq}));
    CHECK(cells(t, b.n.cuda, "interrupt-parent") == std::vector<u32>({b.n.pic}));
    // Its ADB child: what the Mac OS ROM loader takes for "Cuda ADB".
    REQUIRE(find_by_name(t, b.n.cuda, "adb") == b.n.adb);
    CHECK(t.parent(b.n.adb) == b.n.cuda);
    CHECK(str(t, b.n.adb, "device_type") == "adb");
    CHECK(str(t, b.n.adb, "compatible") == "adb");
    // ATA: registers, then the DBDMA channel; interrupts IDE then DMA.
    for (u32 i = 0; i < 2; ++i) {
        const Phandle ata = b.n.ata[i];
        CHECK(compatible_with(t, ata, "heathrow-ata"));            // Linux pata_macio kind
        const auto reg = cells(t, ata, "reg");
        REQUIRE(reg.size() == 4);
        CHECK(reg[1] <= 0x1000u);                                  // macio resource quirks
        CHECK(reg[3] <= 0x100u);
        CHECK_EQ(cell_addr(ata, 0), base + Paddington::kIdeOffset[i]);
        CHECK_EQ(cell_addr(ata, 1), base + Paddington::kDbdmaOffset +
                                        Paddington::kIdeDmaChannel[i] * DbdmaController::kChannelStride);
        CHECK(cells(t, ata, "interrupts") ==
              std::vector<u32>({Paddington::kIdeIrq[i], Paddington::kIdeDmaIrq[i]}));
        CHECK(cells(t, ata, "interrupt-parent") == std::vector<u32>({b.n.pic}));
        // What Mac OS's heathrow-ata driver requires (>= 5 timing words) and
        // the ATA Manager reads.
        CHECK(cells(t, ata, "AAPL,pio-timing").size() >= 5u);
        CHECK(cells(t, ata, "AAPL,bus-id") == std::vector<u32>({i}));
    }
    // ... and the driver finds the chip's base through mac-io's device_type.
    CHECK(str(t, b.n.macio, "device_type") == "mac-io");
    // MESH SCSI: named "scsi" (Apple's loader), type + compatible as Linux matches.
    REQUIRE(b.n.scsi != kNone);
    CHECK(std::string(t.name(b.n.scsi)) == "scsi");
    CHECK(str(t, b.n.scsi, "device_type") == "scsi");
    CHECK(compatible_with(t, b.n.scsi, "chrp,mesh0"));
    CHECK_EQ(cell_addr(b.n.scsi, 0), base + 0x10000u);
    CHECK_EQ(cell_addr(b.n.scsi, 1), base + Paddington::kDbdmaOffset + 0 * DbdmaController::kChannelStride);
    CHECK(cells(t, b.n.scsi, "interrupts") == std::vector<u32>({12, 0}));
    CHECK(cells(t, b.n.scsi, "interrupt-parent") == std::vector<u32>({b.n.pic}));
    // ESCC: MacRISC channels where Linux found them on a real B&W (ch-a at
    // + 0x13020 on interrupt 15, ch-b at + 0x13000 on 16), plus escc-legacy.
    REQUIRE(b.n.escc != kNone);
    CHECK_EQ(t.parent(b.n.escc_a), b.n.escc);
    CHECK_EQ(cell_addr(b.n.escc_a, 0), base + 0x13020u);
    CHECK_EQ(cell_addr(b.n.escc_b, 0), base + 0x13000u);
    // Then data, the enhancement register and the transmit/receive DBDMA
    // blocks; interrupts: the SCC's, then the two DMA ones (OpenBIOS
    // drivers/escc.c; Mac OS's SerialDMA reads them all).
    CHECK_EQ(cell_addr(b.n.escc_a, 1), base + 0x13030u);
    CHECK_EQ(cell_addr(b.n.escc_a, 3), base + 0x8400u);
    CHECK_EQ(cell_addr(b.n.escc_a, 4), base + 0x8500u);
    CHECK_EQ(cell_addr(b.n.escc_b, 3), base + 0x8600u);
    CHECK_EQ(cell_addr(b.n.escc_b, 4), base + 0x8700u);
    CHECK_EQ(base + Paddington::kDbdmaOffset + Paddington::kEsccDmaChannel[0][0] * DbdmaController::kChannelStride,
             base + 0x8400u);
    CHECK_EQ(base + Paddington::kDbdmaOffset + Paddington::kEsccDmaChannel[1][1] * DbdmaController::kChannelStride,
             base + 0x8700u);
    CHECK(cells(t, b.n.escc_a, "interrupts") == std::vector<u32>({15, 4, 5}));
    CHECK(cells(t, b.n.escc_b, "interrupts") == std::vector<u32>({16, 6, 7}));
    CHECK(compatible_with(t, b.n.escc_a, "chrp,es4") && compatible_with(t, b.n.escc_b, "chrp,es5"));
    CHECK(str(t, b.n.escc_a, "device_type") == "serial");
    const Phandle legacy = find_by_name(t, mio, "escc-legacy");
    REQUIRE(legacy != kNone);
    CHECK(compatible_with(t, legacy, "chrp,es1"));
    CHECK_EQ(cells(t, legacy, "reg")[0], 0x12000u);
    // NVRAM: Linux direct mode uses mult = size / 8 KB.
    const auto nv = cells(t, b.n.nvram, "reg");
    REQUIRE(nv.size() == 2);
    CHECK_EQ(cell_addr(b.n.nvram, 0), base + Paddington::kNvramOffset);
    CHECK_EQ(nv[1] / 0x2000u, MacioNvram::kStride);
    CHECK_EQ(nv[1], MacioNvram::kWindowSize);
    // The interrupt controller: Paddington's PIC registers at mac-io + 0, one
    // cell per interrupt; recognisable as Apple's Mac OS ROM loader expects
    // (device_type "interrupt-controller", compatible "paddington").
    REQUIRE(b.n.pic != kNone);
    CHECK_EQ(t.parent(b.n.pic), mio);
    // Mac OS's "Heathrow base": the IRQ 0-31 bank (mac-io + 0x20) at +0x10,
    // IRQs 32-63 (mac-io + 0x10) at +0x00, inside the emulated window.
    CHECK_EQ(cell_addr(b.n.pic, 0), base + Paddington::kPicOffset + 0x10);
    CHECK(cells(t, b.n.pic, "reg") == std::vector<u32>({Paddington::kPicOffset + 0x10, 0x20}));
    CHECK(of::kPicNodeOffset + of::kPicNodeSize <= HeathrowPic::kWindowSize);
    CHECK_EQ(cell_or(t, b.n.pic, "#interrupt-cells", 0), 1u);
    CHECK(str(t, b.n.pic, "device_type") == "interrupt-controller");
    CHECK(compatible_with(t, b.n.pic, "paddington"));
    CHECK(t.has_property(b.n.pic, "interrupt-controller"));
    CHECK_EQ(t.find("/pci/pci-bridge/mac-io/interrupt-controller"), b.n.pic);
}

TEST_CASE("firmware/bw-tree/pci_nodes_follow_the_pci_binding")
{
    Built b;
    const DeviceTree& t = *b.t;
    // reg of a PCI function: its configuration address (bus, device, function).
    const auto br = cells(t, b.n.bridge, "reg");
    REQUIRE(br.size() == 5);
    CHECK_EQ(br[0], (0u << 16) | (0x0Du << 11));
    CHECK_EQ(cell_or(t, b.n.bridge, "vendor-id", 0), 0x1011u);
    CHECK_EQ(cell_or(t, b.n.bridge, "device-id", 0), 0x26u);
    CHECK(cells(t, b.n.bridge, "bus-range") == std::vector<u32>({1, 1}));
    const auto mr = cells(t, b.n.macio, "reg");
    CHECK_EQ(mr[0], (1u << 16) | (5u << 11));
    // Then BAR 0 and its size (PCI binding). The Mac OS ROM loader takes the
    // mac-io base from this second entry; without it it computed 0x10000.
    REQUIRE(mr.size() == 10u);
    CHECK_EQ(mr[5], 0x02000010u | (1u << 16) | (5u << 11));
    CHECK_EQ(mr[9], 0x80000u);
    CHECK_EQ(cell_or(t, b.n.macio, "vendor-id", 0), 0x106Bu);
    CHECK_EQ(cell_or(t, b.n.macio, "device-id", 0), 0x17u);
    CHECK(std::strcmp(t.unit(b.n.bridge), "d") == 0);
    CHECK(std::strcmp(t.unit(b.n.macio), "5") == 0);
    // A different BAR address flows through to every translated address.
    Built moved;
    moved.m.macio_base = 0x81000000u;
    auto t2 = std::make_unique<DeviceTree>();
    BwNodes n2;
    REQUIRE(build_bw_device_tree(*t2, moved.m, n2));
    const auto reg = cells(*t2, n2.cuda, "reg");
    u32 a = 0;
    REQUIRE(translate(*t2, n2.cuda, {reg[0]}, a));
    CHECK_EQ(a, 0x81000000u + Paddington::kViaOffset);
    const auto aa = cells(*t2, n2.macio, "assigned-addresses");
    REQUIRE(aa.size() == 5);
    REQUIRE(translate(*t2, n2.macio, {aa[0], aa[1], aa[2]}, a));
    CHECK_EQ(a, 0x81000000u);
}

TEST_CASE("firmware/bw-tree/cmd646_node_and_io_translation")
{
    Built b;
    REQUIRE(b.ok);
    const DeviceTree& t = *b.t;
    REQUIRE(b.n.cmd646 != kNone);
    CHECK_EQ(t.parent(b.n.cmd646), b.n.bridge);
    CHECK(std::string(t.name(b.n.cmd646)) == "pci-ata");
    CHECK(std::strcmp(t.unit(b.n.cmd646), "1") == 0);
    CHECK_EQ(cell_or(t, b.n.cmd646, "vendor-id", 0), 0x1095u);
    CHECK_EQ(cell_or(t, b.n.cmd646, "device-id", 0), 0x646u);
    CHECK(cells(t, b.n.cmd646, "interrupts") == std::vector<u32>({26}));
    CHECK(cells(t, b.n.cmd646, "interrupt-parent") == std::vector<u32>({b.n.pic}));
    // The channel node carries the driver's compatible (Mac OS binds there).
    REQUIRE(b.n.cmd646_channel != kNone);
    CHECK(std::string(t.name(b.n.cmd646_channel)) == "ata-4");
    CHECK(compatible_with(t, b.n.cmd646_channel, "cmd646-ata"));
    CHECK(str(t, b.n.cmd646_disk, "device_type") == "block");
    // Every I/O BAR translates through the bridge's and the host's ranges
    // to PCI I/O space at 0xFE000000 (what Mac OS uses as AAPL,address).
    const auto aa = cells(t, b.n.cmd646, "assigned-addresses");
    REQUIRE(aa.size() == 25u);
    const u32 want[5] = {0x1440, 0x1430, 0x1448, 0x1434, 0x1400};
    for (u32 i = 0; i < 5; ++i) {
        CHECK_EQ(aa[5 * i] & 0x03000000u, 0x01000000u);      // I/O space
        u32 a = 0;
        REQUIRE(translate(t, b.n.cmd646, {aa[5 * i], aa[5 * i + 1], aa[5 * i + 2]}, a));
        CHECK_EQ(a, 0xFE000000u + want[i]);
    }
    // The Grackle node names its model; the bridge forwards the windows.
    CHECK(str(t, b.n.pci, "model") == "MOT,MPC106");
    // The bridge's ranges must cover what is behind it: Mac OS does not
    // translate through a bridge without them (the CMD646 driver then
    // wrote to RAM). Entry: child (3), parent (3), size (2).
    const auto br = cells(t, b.n.bridge, "ranges");
    REQUIRE(br.size() == 16u);
    CHECK_EQ(br[0] & 0x03000000u, 0x01000000u);              // I/O window
    for (u32 port : want)
        CHECK(port >= br[2] && port - br[2] < br[7]);
    CHECK_EQ(br[8] & 0x03000000u, 0x02000000u);              // memory window
    CHECK(b.m.macio_base >= br[10] && b.m.macio_base + 0x80000u - br[10] <= br[15]);
    // Without a CMD646 there is no node.
    Built none;
    none.m.cmd646_io[0] = 0;
    auto t2 = std::make_unique<DeviceTree>();
    BwNodes n2;
    REQUIRE(build_bw_device_tree(*t2, none.m, n2));
    CHECK(n2.cmd646 == kNone);
}

TEST_CASE("firmware/bw-tree/usb_placeholder_and_cuda_compatible")
{
    Built b;
    REQUIRE(b.ok);
    const DeviceTree& t = *b.t;
    // The Mac OS ROM loader's special-node table matches name "usb" with
    // device_type "usb" (sets extValid1's "has USB" bit). No controller is
    // emulated, so the node has no addresses a driver could bind to.
    REQUIRE(b.n.usb != kNone);
    CHECK_EQ(t.parent(b.n.usb), b.n.pci);
    CHECK(std::strcmp(t.name(b.n.usb), "usb") == 0);
    const u8* p = nullptr;
    u32 len = 0;
    REQUIRE(t.get_property(b.n.usb, "device_type", p, len));
    CHECK(std::strcmp(reinterpret_cast<const char*>(p), "usb") == 0);
    CHECK(!t.get_property(b.n.usb, "reg", p, len));
    CHECK(!t.get_property(b.n.usb, "assigned-addresses", p, len));
    CHECK(!t.get_property(b.n.usb, "compatible", p, len));
    // via-cuda's compatible "cuda": the ROM's PowerMgrPlugin parcel matches it.
    REQUIRE(t.get_property(b.n.cuda, "compatible", p, len));
    CHECK(std::strcmp(reinterpret_cast<const char*>(p), "cuda") == 0);
}

TEST_CASE("firmware/bw-tree/rtc_under_cuda")
{
    Built b;
    REQUIRE(b.ok);
    const DeviceTree& t = *b.t;
    // As OpenBIOS: via-cuda/rtc with device_type and compatible "rtc"
    // (Mac OS's UTCUtils searches for device_type "rtc").
    REQUIRE(b.n.rtc != kNone);
    CHECK_EQ(t.parent(b.n.rtc), b.n.cuda);
    CHECK(std::strcmp(t.name(b.n.rtc), "rtc") == 0);
    const u8* p = nullptr;
    u32 len = 0;
    REQUIRE(t.get_property(b.n.rtc, "device_type", p, len));
    CHECK(std::strcmp(reinterpret_cast<const char*>(p), "rtc") == 0);
    REQUIRE(t.get_property(b.n.rtc, "compatible", p, len));
    CHECK(std::strcmp(reinterpret_cast<const char*>(p), "rtc") == 0);
}

TEST_CASE("firmware/bw-tree/serial_channels_have_slot_names")
{
    Built b;
    REQUIRE(b.ok);
    const DeviceTree& t = *b.t;
    // As OpenBIOS: slot-names = 0 on the channels (the Mac OS ROM loader's
    // "slot-name detected"; without it Mac OS 9.2.1 started LocalTalk).
    CHECK_EQ(cell_or(t, b.n.escc_a, "slot-names", 0xFFFFFFFFu), 0u);
    CHECK_EQ(cell_or(t, b.n.escc_b, "slot-names", 0xFFFFFFFFu), 0u);
}

TEST_CASE("firmware/bw-tree/sound_and_ethernet")
{
    Built b;
    REQUIRE(b.ok);
    const DeviceTree& t = *b.t;
    const u8* p = nullptr;
    u32 len = 0;
    // davbus with a "sound" child of compatible "screamer" (Linux pmac.c).
    REQUIRE(b.n.davbus != kNone);
    REQUIRE(b.n.sound != kNone);
    CHECK_EQ(t.parent(b.n.davbus), b.n.macio);
    CHECK_EQ(t.parent(b.n.sound), b.n.davbus);
    CHECK(std::strcmp(t.name(b.n.davbus), "davbus") == 0);
    CHECK(std::strcmp(t.name(b.n.sound), "sound") == 0);
    REQUIRE(t.get_property(b.n.sound, "compatible", p, len));
    CHECK(std::strcmp(reinterpret_cast<const char*>(p), "screamer") == 0);
    CHECK(!t.get_property(b.n.sound, "layout-id", p, len));  // snd-powermac refuses nodes with one
    // reg: codec, out DMA, in DMA; interrupts 17, 8, 9.
    REQUIRE(t.get_property(b.n.davbus, "reg", p, len));
    REQUIRE(len == 24u);
    CHECK_EQ(load_be32(p), 0x14000u);
    CHECK_EQ(load_be32(p + 8), 0x8800u);
    CHECK_EQ(load_be32(p + 16), 0x8900u);
    REQUIRE(t.get_property(b.n.davbus, "interrupts", p, len));
    REQUIRE(len == 12u);
    CHECK_EQ(load_be32(p), 17u);
    CHECK_EQ(load_be32(p + 4), 8u);
    CHECK_EQ(load_be32(p + 8), 9u);
    // ethernet: type network, compatible bmac+, three regs and interrupts, the address.
    REQUIRE(b.n.ethernet != kNone);
    CHECK_EQ(t.parent(b.n.ethernet), b.n.macio);
    REQUIRE(t.get_property(b.n.ethernet, "device_type", p, len));
    CHECK(std::strcmp(reinterpret_cast<const char*>(p), "network") == 0);
    REQUIRE(t.get_property(b.n.ethernet, "compatible", p, len));
    CHECK(std::strcmp(reinterpret_cast<const char*>(p), "bmac+") == 0);
    REQUIRE(t.get_property(b.n.ethernet, "reg", p, len));
    REQUIRE(len == 24u);
    CHECK_EQ(load_be32(p), 0x11000u);
    CHECK_EQ(load_be32(p + 8), 0x8200u);
    CHECK_EQ(load_be32(p + 16), 0x8300u);
    REQUIRE(t.get_property(b.n.ethernet, "interrupts", p, len));
    REQUIRE(len == 12u);
    CHECK_EQ(load_be32(p), 42u);
    CHECK_EQ(load_be32(p + 4), 32u);
    CHECK_EQ(load_be32(p + 8), 33u);
    REQUIRE(t.get_property(b.n.ethernet, "cell-id", p, len));  // Mac OS's lanLib needs it
    REQUIRE(len == 4u);
    CHECK_EQ(load_be32(p), 0xC7u);
    const BwMachine m;
    for (const char* prop : {"local-mac-address", "mac-address"}) {
        REQUIRE(t.get_property(b.n.ethernet, prop, p, len));
        REQUIRE(len == 6u);
        CHECK(std::memcmp(p, m.mac_address, 6) == 0);
    }
}
