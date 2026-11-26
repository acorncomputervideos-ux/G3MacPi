// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/guest/run_ndrv.cpp — runs our Mac OS display driver (guest/ndrv)
// on the emulated 750 against the real display device model, with this
// program playing Mac OS: the PEF is prepared like the Code Fragment
// Manager would (tests/unit/tools/pef_prepare.h), its imports
// (IOCommandIsComplete, RegistryPropertyGetSize, RegistryPropertyGet, the
// interrupt-management routines and the Video Services Library's interrupt
// services) are
// stand-ins reached through transition vectors whose code is an emulator
// trap word, and DoDriverIO is called the way the Device Manager and the
// Display Manager call it.
//
// Expected values are written here from Apple's documentation (the
// command and csCode numbers, record offsets under 68K alignment), not
// taken from the driver's source, so that a wrong constant there fails.
//
// usage: g3_guest_ndrv g3_ndrv.pef

#include "core/cpu/ppc750/ppc750.h"
#include "core/devices/video/framebuffer.h"
#include "core/mem/physical_bus.h"
#include "core/sched/scheduler.h"
#include "tests/unit/tools/pef_prepare.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace g3;

namespace {

constexpr u32 kRam = 4u << 20;
constexpr u32 kVramSize = 4u << 20;
constexpr u32 kVram = 0x90000000u, kRegs = 0x91000000u;
constexpr u32 kBase = 0x100000;                 // where the fragment is placed
constexpr u32 kStubTv = 0x6000, kStubCode = 0x6100, kReturn = 0x6F00, kStack = 0x3F0000;
constexpr u32 kTrapComplete = 0x00004F10u, kTrapGetSize = 0x00004F11u, kTrapGet = 0x00004F12u;
constexpr u32 kTrapGetIntFns = 0x00004F13u, kTrapInstallIntFns = 0x00004F14u, kTrapEnabler = 0x00004F15u,
              kTrapVslNew = 0x00004F17u, kTrapVslDo = 0x00004F18u, kTrapVslDispose = 0x00004F19u;
constexpr u32 kNumStubs = 9;
// The device's interrupt as Mac OS would describe it: driver-ist member 0
// (the chip's source) = {set ID, member}, and the functions installed on it
// before the driver's (GetInterruptFunctions returns these). The enabler is
// a real transition vector (stub 8); the others are just values.
constexpr u32 kIstSet = 0x1234AB00u, kIstMember = 7;
constexpr u32 kOldRefCon = 0x7777u, kOldHandler = 0x6800u, kOldDisabler = 0x6810u;
enum : u32 { kIsrIsComplete = 0, kIsrIsNotComplete = 0xFFFFFFFFu };  // ch. 9
constexpr u32 kVslId = 0x5A5A0001u;            // the service ID our VSLNewInterruptService gives
constexpr u32 kStopWord = 0x00004F99u;
constexpr u32 kScratch = 0x200000;              // records passed to the driver
constexpr u8 kEntryMagic = 0xA5;                // our RegEntryID: 16 bytes of this

// Command codes and kinds (Designing PCI Cards and Drivers, chapter 8).
enum : u32 { kOpen = 0, kClose = 1, kRead = 2, kWrite = 3, kControl = 4, kStatus = 5, kKillIO = 6,
             kInitialize = 7, kFinalize = 8 };
enum : u32 { kSync = 1, kAsync = 2, kImmediate = 4 };
// Result codes (Inside Macintosh: Devices; chapter 7 for noHardwareErr).
enum : s16 { noErr = 0, controlErr = -17, statusErr = -18, readErr = -19, writErr = -20, paramErr = -50,
             noHardwareErr = -200 };

struct Host {
    PhysicalBus* bus = nullptr;
    std::map<std::string, std::vector<u32>> props;
    std::vector<std::pair<u32, s32>> completions;  // IOCommandIsComplete(ID, result)
    bool stopped = false;
    u32 registry_calls = 0;
    // Interrupt management and VBL services (Designing PCI Cards and Drivers,
    // ch. 9 and 13).
    u32 get_fns = 0, installs = 0, enables = 0;
    bool get_fns_ok = false, enable_ok = false;
    u32 installed_refcon = 0, installed_handler = 0, installed_enabler = 0, installed_disabler = 0;
    u32 vsl_new = 0, vsl_new_type = 0, vsl_do = 0, vsl_disposed = 0;
    bool vsl_entry_ok = false;

    std::string cstr(u32 a) const
    {
        std::string s;
        u32 c = 0;
        for (; s.size() < 256; ++a) {
            bus->read32(a & ~3u, c);
            const char ch = static_cast<char>(c >> (24 - 8 * (a & 3)));
            if (!ch)
                break;
            s.push_back(ch);
        }
        return s;
    }
    bool entry_ok(u32 a) const
    {
        for (u32 i = 0; i < 16; i += 4) {
            u32 w = 0;
            bus->read32(a + i, w);
            if (w != 0xA5A5A5A5u)
                return false;
        }
        return true;
    }
};

u8 rd8(PhysicalBus& b, u32 a) { u32 w = 0; b.read32(a & ~3u, w); return static_cast<u8>(w >> (24 - 8 * (a & 3))); }
u32 rd16(PhysicalBus& b, u32 a) { return (rd8(b, a) << 8) | rd8(b, a + 1); }
u32 rd32(PhysicalBus& b, u32 a) { return (rd16(b, a) << 16) | rd16(b, a + 2); }
void wr8(PhysicalBus& b, u32 a, u32 v)
{
    u32 w = 0;
    b.read32(a & ~3u, w);
    const u32 sh = 24 - 8 * (a & 3);
    w = (w & ~(0xFFu << sh)) | ((v & 0xFF) << sh);
    b.write32(a & ~3u, w);
}
void wr16(PhysicalBus& b, u32 a, u32 v) { wr8(b, a, v >> 8); wr8(b, a + 1, v); }
void wr32(PhysicalBus& b, u32 a, u32 v) { wr16(b, a, v >> 16); wr16(b, a + 2, v); }
void zero(PhysicalBus& b, u32 a, u32 n) { for (u32 i = 0; i < n; ++i) wr8(b, a + i, 0); }

bool trap(void* ctx, u32 insn, ppc::CpuState& s, u32& next_pc)
{
    Host& h = *static_cast<Host*>(ctx);
    PhysicalBus& b = *h.bus;
    if (insn == kStopWord) {
        h.stopped = true;
        return true;
    }
    if (insn == kTrapComplete) {
        h.completions.push_back({s.gpr[3], static_cast<s32>(s.gpr[4])});
        s.gpr[3] = 0;
    } else if (insn == kTrapGetSize || insn == kTrapGet) {
        ++h.registry_calls;
        const std::string name = h.cstr(s.gpr[4]);
        auto it = h.props.find(name);
        if (!h.entry_ok(s.gpr[3]) || it == h.props.end()) {
            s.gpr[3] = static_cast<u32>(-2541);    // any non-zero status: not found
        } else if (insn == kTrapGetSize) {
            wr32(b, s.gpr[5], static_cast<u32>(4 * it->second.size()));
            s.gpr[3] = 0;
        } else {
            const u32 size_ptr = s.gpr[6], want = rd32(b, size_ptr);
            const u32 have = static_cast<u32>(4 * it->second.size());
            const u32 n = want < have ? want : have;
            for (u32 i = 0; i < n; i += 4)
                wr32(b, s.gpr[5] + i, it->second[i / 4]);
            wr32(b, size_ptr, n);
            s.gpr[3] = 0;
        }
    } else if (insn == kTrapGetIntFns) {       // GetInterruptFunctions(set, member, &refCon, &h, &en, &dis)
        ++h.get_fns;
        h.get_fns_ok = s.gpr[3] == kIstSet && s.gpr[4] == kIstMember;
        wr32(b, s.gpr[5], kOldRefCon);
        wr32(b, s.gpr[6], kOldHandler);
        wr32(b, s.gpr[7], kStubTv + 16 * 8);
        wr32(b, s.gpr[8], kOldDisabler);
        s.gpr[3] = 0;
    } else if (insn == kTrapInstallIntFns) {   // InstallInterruptFunctions(set, member, refCon, h, en, dis)
        if (s.gpr[3] == kIstSet && s.gpr[4] == kIstMember) {
            ++h.installs;
            h.installed_refcon = s.gpr[5];
            h.installed_handler = s.gpr[6];
            h.installed_enabler = s.gpr[7];
            h.installed_disabler = s.gpr[8];
        }
        s.gpr[3] = 0;
    } else if (insn == kTrapEnabler) {         // InterruptEnabler(member {set, member}, refCon)
        ++h.enables;
        h.enable_ok = s.gpr[3] == kIstSet && s.gpr[4] == kIstMember && s.gpr[5] == kOldRefCon;
    } else if (insn == kTrapVslNew) {          // VSLNewInterruptService(owner, type, &id)
        ++h.vsl_new;
        h.vsl_entry_ok = h.entry_ok(s.gpr[3]);
        h.vsl_new_type = s.gpr[4];
        wr32(b, s.gpr[5], kVslId);
        s.gpr[3] = 0;
    } else if (insn == kTrapVslDo) {           // VSLDoInterruptService(id)
        if (s.gpr[3] == kVslId)
            ++h.vsl_do;
        s.gpr[3] = 0;
    } else if (insn == kTrapVslDispose) {      // VSLDisposeInterruptService(id)
        if (s.gpr[3] == kVslId)
            ++h.vsl_disposed;
        s.gpr[3] = 0;
    } else {
        return false;
    }
    next_pc = s.lr();
    return true;
}

bool read_file(const char* path, std::vector<u8>& out)
{
    FILE* f = std::fopen(path, "rb");
    if (!f)
        return false;
    for (int c; (c = std::fgetc(f)) != EOF;)
        out.push_back(static_cast<u8>(c));
    std::fclose(f);
    return true;
}

int failures = 0;
#define EXPECT(cond, ...)                                              \
    do {                                                               \
        if (!(cond)) {                                                 \
            ++failures;                                                \
            std::printf("FAIL line %d: %s — ", __LINE__, #cond);       \
            std::printf(__VA_ARGS__);                                  \
            std::printf("\n");                                         \
        }                                                              \
    } while (0)

} // namespace

int main(int argc, char** argv)
{
    std::vector<u8> pef;
    if (argc < 2 || !read_file(argv[1], pef)) {
        std::fprintf(stderr, "usage: %s g3_ndrv.pef\n", argv[0]);
        return 2;
    }
    const std::map<std::string, u32> imports = {
        {"IOCommandIsComplete", kStubTv}, {"RegistryPropertyGetSize", kStubTv + 16},
        {"RegistryPropertyGet", kStubTv + 32}, {"GetInterruptFunctions", kStubTv + 48},
        {"InstallInterruptFunctions", kStubTv + 64}, {"VSLNewInterruptService", kStubTv + 80},
        {"VSLDoInterruptService", kStubTv + 96}, {"VSLDisposeInterruptService", kStubTv + 112}};
    const pef_test::Prepared p = pef_test::prepare(pef, {kBase}, imports);
    if (!p.ok) {
        std::printf("FAIL prepare: %s\n", p.why.c_str());
        return 1;
    }
    std::vector<u8> ram(kRam), vram(kVramSize);
    PhysicalBus bus;
    bus.map_ram(0, ram.data(), kRam);
    Framebuffer fb(vram.data(), kVramSize);
    if (!fb.map(bus, kVram, kRegs) || !fb.set_mode(800, 600, 15)) {
        std::printf("FAIL display setup\n");
        return 1;
    }
    fb.set_enabled(true);
    Scheduler sched;
    fb.attach_interrupt(sched, [](void*, bool) {}, nullptr);
    const std::vector<u8>& image = p.mem.at(0);
    std::memcpy(&ram[kBase], image.data(), image.size());
    const u32 traps[kNumStubs] = {kTrapComplete, kTrapGetSize, kTrapGet,   kTrapGetIntFns, kTrapInstallIntFns,
                                  kTrapVslNew,   kTrapVslDo,   kTrapVslDispose, kTrapEnabler};
    for (u32 i = 0; i < kNumStubs; ++i) {
        wr32(bus, kStubTv + 16 * i, kStubCode + 4 * i);
        wr32(bus, kStubTv + 16 * i + 4, 0x00777000u);
        wr32(bus, kStubCode + 4 * i, traps[i]);
    }
    wr32(bus, kReturn, kStopWord);

    Host host;
    host.bus = &bus;
    host.props["assigned-addresses"] = {0x82008010u, 0, kVram, 0, kVramSize, 0x82008014u, 0, kRegs, 0, Framebuffer::kRegsSize};
    host.props["driver-ist"] = {kIstSet, kIstMember, 0, 0, 0, 0};  // made by Mac OS from "interrupts"
    ppc::Ppc750 cpu(bus);
    cpu.set_hle_handler(trap, &host);

    auto export_of = [&](const char* name, u8 want_cls) {
        u32 v = 0;
        int16_t sec = -1;
        u8 cls = 0;
        if (!pef_test::find_export(pef, name, v, sec, cls) || sec != 0 || cls != want_cls)
            return 0u;
        return kBase + v;
    };
    const u32 tv = export_of("DoDriverIO", g3::pef::kTVectorSymbol);
    const u32 desc = export_of("TheDriverDescription", g3::pef::kDataSymbol);
    if (!tv || !desc) {
        std::printf("FAIL exports missing\n");
        return 1;
    }

    // A routine reached through a transition vector, called as Mac OS would.
    bool ran = true;
    auto callv = [&](u32 tvec, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) -> u32 {
        cpu.reset();
        ppc::CpuState& s = cpu.state();
        s.msr = ppc::msr::FP;
        s.pc = rd32(bus, tvec);
        s.gpr[2] = rd32(bus, tvec + 4);
        s.gpr[12] = tvec;
        s.gpr[1] = kStack;
        s.gpr[3] = a1;
        s.gpr[4] = a2;
        s.gpr[5] = a3;
        s.gpr[6] = a4;
        s.gpr[7] = a5;
        s.lr() = kReturn;
        host.stopped = false;
        for (u64 n = 0; !host.stopped && !cpu.checkstopped() && n < 200000000ull; ++n)
            cpu.step();
        if (!host.stopped || s.gpr[1] != kStack) {
            ran = false;
            std::printf("FAIL the call through 0x%08X did not return (pc=0x%08X)\n", tvec, s.pc);
        }
        return s.gpr[3];
    };
    // DoDriverIO(spaceID, ID, contents, code, kind).
    auto io = [&](u32 code, u32 contents, u32 kind, u32 id = 0x0BAD0001u) -> s16 {
        return static_cast<s16>(callv(tv, 1, id, contents, code, kind));
    };
    // Control/Status with a record: a CntrlParam at kScratch, the record after it.
    const u32 pb = kScratch, rec = kScratch + 0x100, table = kScratch + 0x400, vpb = kScratch + 0x1000;
    auto call = [&](u32 code, u32 cs, u32 kind = kImmediate) {
        zero(bus, pb, 50);
        wr16(bus, pb + 26, cs);
        wr32(bus, pb + 28, rec);
        return io(code, pb, kind);
    };
    auto control = [&](u32 cs) { return call(kControl, cs); };
    auto status = [&](u32 cs) { return call(kStatus, cs); };

    // --- the driver description ---------------------------------------------------
    EXPECT(rd32(bus, desc) == 0x6D74656Au, "signature 'mtej': 0x%08X", rd32(bus, desc));
    EXPECT(rd32(bus, desc + 4) == 0, "version");
    {
        std::string name;
        for (u32 i = 0; i < rd8(bus, desc + 8); ++i)
            name.push_back(static_cast<char>(rd8(bus, desc + 9 + i)));
        EXPECT(name == "display", "nameInfoStr '%s' (the node name)", name.c_str());
        EXPECT(rd32(bus, desc + 44) == 6u, "runtime: expert control + opened upon load");
        std::string drv;
        for (u32 i = 0; i < rd8(bus, desc + 48); ++i)
            drv.push_back(static_cast<char>(rd8(bus, desc + 49 + i)));
        EXPECT(drv == ".Display_Video_G3MacPi", "driverName '%s'", drv.c_str());
        EXPECT(rd32(bus, desc + 112) == 1, "one service");
        EXPECT(rd32(bus, desc + 116) == 0x6E647276u, "category 'ndrv'");
        EXPECT(rd32(bus, desc + 120) == 0x7669646Fu, "type 'vido'");
    }

    // --- before initialization ------------------------------------------------------
    EXPECT(control(2) == controlErr, "control before init");
    EXPECT(io(kOpen, pb, kImmediate) == noHardwareErr, "open before init");

    // --- initialize: DriverInitInfo {refNum, RegEntryID} ------------------------------
    const u32 init = kScratch + 0x2000;
    wr16(bus, init, static_cast<u16>(-50));
    for (u32 i = 0; i < 16; ++i)
        wr8(bus, init + 2 + i, kEntryMagic);
    {
        // No addresses: no hardware.
        auto saved = host.props;
        host.props.clear();
        EXPECT(io(kInitialize, init, kImmediate) == noHardwareErr, "init without assigned-addresses");
        // Addresses pointing at RAM: the ID register doesn't match.
        host.props["assigned-addresses"] = {0x82008010u, 0, 0x300000, 0, 0x1000, 0x82008014u, 0, 0x300000, 0, 0x1000};
        EXPECT(io(kInitialize, init, kImmediate) == noHardwareErr, "init on the wrong device");
        // AAPL,address (Mac OS's logical addresses) wins over assigned-addresses.
        host.props["AAPL,address"] = {0x300000, 0x300000};
        host.props["assigned-addresses"] = saved["assigned-addresses"];
        EXPECT(io(kInitialize, init, kImmediate) == noHardwareErr, "init uses AAPL,address first");
        host.props = saved;
    }
    host.completions.clear();
    EXPECT(io(kInitialize, init, kSync, 0x1111) == noErr, "initialize");
    EXPECT(host.completions.size() == 1 && host.completions[0].first == 0x1111 && host.completions[0].second == 0,
           "synchronous initialize completed through IOCommandIsComplete");
    EXPECT(host.registry_calls >= 2, "the driver read the Name Registry");
    EXPECT(fb.width() == 800 && fb.height() == 600 && fb.depth() == 15 && fb.enabled(),
           "firmware mode kept: %ux%u depth %u", fb.width(), fb.height(), fb.depth());
    EXPECT(fb.palette(0) == 0x808080u && fb.palette(255) == 0x808080u, "50%% gray CLUT before a mode set");
    // --- vertical blanking: a 'vbl ' service signalled from the device's interrupt ---
    EXPECT(host.vsl_new == 1 && host.vsl_entry_ok && host.vsl_new_type == 0x76626C20u,
           "VSLNewInterruptService(our RegEntryID, 'vbl ') once (calls %u, type 0x%08X)", host.vsl_new,
           host.vsl_new_type);
    EXPECT(host.get_fns == 1 && host.get_fns_ok, "GetInterruptFunctions on driver-ist member 0");
    EXPECT(host.installs == 1 && host.installed_handler != 0 && host.installed_handler != kOldHandler &&
               host.installed_enabler == 0 && host.installed_disabler == 0,
           "our handler installed, the enabler and disabler kept (nil)");
    EXPECT(host.enables == 1 && host.enable_ok, "the existing enabler called for our member");
    auto reg = [&](u32 r) { u32 v = 0; bus.read32(kRegs + r, v); return v; };
    EXPECT(reg(Framebuffer::IRQ_ENABLE) == 1, "the device's vertical-blank interrupt on");
    {
        // A vertical blank: Mac OS calls the handler at hardware interrupt level
        // with the InterruptSetMember (two words), refCon and the count.
        const u32 handler = host.installed_handler;
        sched.advance_to(sched.now() + Framebuffer::kVblPs);
        EXPECT(reg(Framebuffer::IRQ_STATUS) == 1, "a blank is pending");
        EXPECT(callv(handler, kIstSet, kIstMember, host.installed_refcon, 1, 0) == kIsrIsComplete,
               "the handler claims its interrupt");
        EXPECT(host.vsl_do == 1, "and signals the VBL service (%u)", host.vsl_do);
        EXPECT(reg(Framebuffer::IRQ_STATUS) == 0, "and acknowledges it");
        EXPECT(callv(handler, kIstSet, kIstMember, host.installed_refcon, 2, 0) == kIsrIsNotComplete &&
                   host.vsl_do == 1,
               "nothing pending: not ours");
        // SetInterrupt csMode 1: VBL interrupts off; GetInterrupt reports it.
        zero(bus, rec, 4);
        wr8(bus, rec, 1);
        EXPECT(control(7) == noErr, "SetInterrupt (off)");
        EXPECT(status(7) == noErr && rd8(bus, rec) == 1, "GetInterrupt: off");
        sched.advance_to(sched.now() + Framebuffer::kVblPs);
        EXPECT(callv(handler, kIstSet, kIstMember, 0, 3, 0) == kIsrIsComplete && host.vsl_do == 1 &&
                   reg(Framebuffer::IRQ_STATUS) == 0,
               "off: acknowledged, no VBL service call");
        zero(bus, rec, 4);
        EXPECT(control(7) == noErr && status(7) == noErr && rd8(bus, rec) == 0, "SetInterrupt (on)");
        sched.advance_to(sched.now() + Framebuffer::kVblPs);
        callv(handler, kIstSet, kIstMember, 0, 4, 0);
        EXPECT(host.vsl_do == 2, "on again");
    }
    host.completions.clear();
    EXPECT(io(kOpen, pb, kImmediate) == noErr, "open");
    EXPECT(host.completions.empty(), "immediate requests are not completed through IOCommandIsComplete");

    // --- current mode ---------------------------------------------------------------
    EXPECT(status(10) == noErr, "GetCurrentMode");
    EXPECT(rd16(bus, rec) == 129 && rd32(bus, rec + 2) == 2 && rd16(bus, rec + 6) == 0 && rd32(bus, rec + 8) == kVram,
           "current: depth %u mode %u page %u base 0x%08X", rd16(bus, rec), rd32(bus, rec + 2), rd16(bus, rec + 6),
           rd32(bus, rec + 8));
    EXPECT(status(2) == noErr && rd16(bus, rec) == 129 && rd32(bus, rec + 8) == kVram, "GetMode");
    EXPECT(status(4) == noErr && rd16(bus, rec + 6) == 1, "GetPages: one page");
    zero(bus, rec, 12);                                     // csPage 0 (GetPages left 1 here)
    EXPECT(status(5) == noErr && rd32(bus, rec + 8) == kVram, "GetBaseAddress");
    wr16(bus, rec + 6, 1);
    EXPECT(status(5) == paramErr, "GetBaseAddress of page 1");

    // --- resolutions: 4 MB of VRAM limits the depths --------------------------------
    {
        const u32 w[] = {640, 800, 1024, 1152, 1280, 1600}, h[] = {480, 600, 768, 870, 1024, 1200};
        const u32 maxd[] = {130, 130, 130, 130, 129, 129};
        u32 prev = 0xFFFFFFFEu;  // kDisplayModeIDFindFirstResolution
        for (u32 i = 0; i < 6; ++i) {
            zero(bus, rec, 32);
            wr32(bus, rec, prev);
            EXPECT(call(kStatus, 17) == noErr, "GetNextResolution %u", i);
            const u32 id = rd32(bus, rec + 4);
            EXPECT(rd32(bus, rec + 8) == w[i] && rd32(bus, rec + 12) == h[i], "resolution %u: %ux%u", i,
                   rd32(bus, rec + 8), rd32(bus, rec + 12));
            EXPECT(rd16(bus, rec + 20) == maxd[i], "resolution %u max depth %u", i, rd16(bus, rec + 20));
            EXPECT(rd32(bus, rec + 16) >> 16 >= 60, "refresh rate");
            EXPECT(id != 0 && (id & 0x80000000u) == 0, "DisplayModeID %u not reserved", id);
            prev = id;
        }
        zero(bus, rec, 32);
        wr32(bus, rec, prev);
        EXPECT(call(kStatus, 17) == noErr && rd32(bus, rec + 4) == 0xFFFFFFFDu, "end of the list");
        zero(bus, rec, 32);
        wr32(bus, rec, 99);
        wr32(bus, rec + 4, 0x12345678u);
        EXPECT(call(kStatus, 17) == paramErr && rd32(bus, rec + 4) == 0x12345678u, "invalid ID: paramErr, untouched");
        zero(bus, rec, 32);
        wr32(bus, rec, 0);  // kDisplayModeIDCurrent
        EXPECT(call(kStatus, 17) == noErr && rd32(bus, rec + 8) == 800, "current resolution");
    }

    // --- video parameters (VPBlock, 68K-aligned) --------------------------------------
    auto params = [&](u32 id, u32 depth) {
        zero(bus, rec, 32);
        zero(bus, vpb, 64);
        wr32(bus, rec, id);
        wr16(bus, rec + 4, depth);
        wr32(bus, rec + 6, vpb);
        return call(kStatus, 18);
    };
    EXPECT(params(1, 128) == noErr, "GetVideoParameters 640x480x8");
    EXPECT(rd32(bus, vpb) == 0 && rd16(bus, vpb + 4) == 640, "rowBytes %u", rd16(bus, vpb + 4));
    EXPECT(rd16(bus, vpb + 6) == 0 && rd16(bus, vpb + 8) == 0 && rd16(bus, vpb + 10) == 480 && rd16(bus, vpb + 12) == 640,
           "bounds");
    EXPECT(rd32(bus, vpb + 22) == 0x00480000u && rd32(bus, vpb + 26) == 0x00480000u, "72 dpi");
    EXPECT(rd16(bus, vpb + 30) == 0 && rd16(bus, vpb + 32) == 8 && rd16(bus, vpb + 34) == 1 && rd16(bus, vpb + 36) == 8,
           "indexed 8-bit pixels");
    EXPECT(rd32(bus, rec + 10) == 1 && rd32(bus, rec + 14) == 0, "one page, clutType");
    EXPECT(params(2, 129) == noErr && rd16(bus, vpb + 4) == 1600 && rd16(bus, vpb + 30) == 16 &&
               rd16(bus, vpb + 32) == 16 && rd16(bus, vpb + 34) == 3 && rd16(bus, vpb + 36) == 5 && rd32(bus, rec + 14) == 2,
           "800x600 x1R5G5B5: RGBDirect, directType");
    EXPECT(params(4, 130) == noErr && rd16(bus, vpb + 4) == 4608 && rd16(bus, vpb + 32) == 32 && rd16(bus, vpb + 36) == 8,
           "1152x870x32");
    EXPECT(params(5, 130) == paramErr, "1280x1024x32 doesn't fit");
    EXPECT(params(1, 131) == paramErr, "no fourth depth");

    // --- switching modes --------------------------------------------------------------
    auto switch_mode = [&](u32 id, u32 depth, u32 page = 0) {
        zero(bus, rec, 16);
        wr16(bus, rec, depth);
        wr32(bus, rec + 2, id);
        wr16(bus, rec + 6, page);
        return control(10);
    };
    EXPECT(switch_mode(1, 128) == noErr && rd32(bus, rec + 8) == kVram, "SwitchMode 640x480x8");
    EXPECT(fb.width() == 640 && fb.height() == 480 && fb.depth() == 8 && fb.pitch() == 640, "device mode");
    EXPECT(switch_mode(5, 130) == paramErr && fb.width() == 640, "a mode that doesn't fit is refused");
    EXPECT(switch_mode(1, 128, 1) == paramErr, "no page 1");

    // --- colour table ---------------------------------------------------------------
    auto put_colors = [&](s16 start, const std::vector<std::vector<u32>>& c, u32 cs = 3) {
        for (size_t i = 0; i < c.size(); ++i) {
            wr16(bus, table + 8 * static_cast<u32>(i), c[i][0]);
            wr16(bus, table + 8 * static_cast<u32>(i) + 2, c[i][1]);
            wr16(bus, table + 8 * static_cast<u32>(i) + 4, c[i][2]);
            wr16(bus, table + 8 * static_cast<u32>(i) + 6, c[i][3]);
        }
        zero(bus, rec, 8);
        wr32(bus, rec, table);
        wr16(bus, rec + 4, static_cast<u16>(start));
        wr16(bus, rec + 6, static_cast<u32>(c.size() - 1));  // 0-based count
        return control(cs);
    };
    EXPECT(put_colors(10, {{0, 0xFFFF, 0, 0}, {0, 0, 0x8000, 0}, {0, 0x1234, 0x5678, 0x9ABC}}) == noErr, "SetEntries");
    EXPECT(fb.palette(10) == 0xFF0000u && fb.palette(11) == 0x008000u && fb.palette(12) == 0x12569Au,
           "palette 0x%06X 0x%06X 0x%06X", fb.palette(10), fb.palette(11), fb.palette(12));
    EXPECT(put_colors(-1, {{200, 0x0100, 0x0200, 0x0300}, {7, 0xFFFF, 0xFFFF, 0xFFFF}}) == noErr, "SetEntries by value");
    EXPECT(fb.palette(200) == 0x010203u && fb.palette(7) == 0xFFFFFFu, "indexed by the value fields");
    EXPECT(put_colors(255, {{0, 0, 0, 0}, {0, 0, 0, 0}}) == paramErr, "past entry 255");
    zero(bus, rec, 8);
    wr32(bus, rec, table);
    wr16(bus, rec + 4, 10);
    wr16(bus, rec + 6, 2);
    EXPECT(status(3) == noErr && rd16(bus, table) == 10 && rd16(bus, table + 2) == 0xFFFFu &&
               rd16(bus, table + 8 + 4) == 0x8080u && rd16(bus, table + 16 + 6) == 0x9A9Au,
           "GetEntries");
    // Gamma: an inverting one-channel table, then three channels, then linear.
    const u32 gt = kScratch + 0x3000;
    auto put_gamma = [&](u32 version, u32 chans, u32 formula, auto value) {
        wr16(bus, gt, version); wr16(bus, gt + 2, 0); wr16(bus, gt + 4, formula); wr16(bus, gt + 6, chans);
        wr16(bus, gt + 8, 256); wr16(bus, gt + 10, 8);
        for (u32 c = 0; c < chans; ++c)
            for (u32 i = 0; i < 256; ++i)
                wr8(bus, gt + 12 + formula + 256 * c + i, value(c, i));
        zero(bus, rec, 4);
        wr32(bus, rec, gt);
        return control(4);
    };
    EXPECT(put_gamma(0, 1, 2, [](u32, u32 i) { return 255 - i; }) == noErr, "SetGamma, one channel, formula data");
    put_colors(20, {{0, 0xFFFF, 0x0000, 0x4000}});
    EXPECT(fb.palette(20) == 0x00FFBFu, "inverted through gamma: 0x%06X", fb.palette(20));
    EXPECT(put_gamma(0, 3, 0, [](u32 c, u32 i) { return c == 0 ? i : c == 1 ? 0u : 255u; }) == noErr, "three channels");
    put_colors(21, {{0, 0x1200, 0x3400, 0x5600}});
    EXPECT(fb.palette(21) == 0x1200FFu, "per-channel gamma: 0x%06X", fb.palette(21));
    EXPECT(status(8) == noErr, "GetGamma");
    {
        const u32 cur = rd32(bus, rec);
        EXPECT(cur != 0 && rd16(bus, cur + 6) == 3 && rd16(bus, cur + 8) == 256 && rd8(bus, cur + 12 + 256) == 0,
               "GetGamma points at the current table");
    }
    EXPECT(put_gamma(1, 1, 0, [](u32, u32 i) { return i; }) == paramErr, "gVersion 1 refused");
    EXPECT(put_gamma(0, 2, 0, [](u32, u32 i) { return i; }) == paramErr, "two channels refused");
    zero(bus, rec, 4);
    EXPECT(control(4) == noErr, "SetGamma(NULL): linear");
    put_colors(22, {{0, 0x1200, 0x3400, 0x5600}});
    EXPECT(fb.palette(22) == 0x123456u, "linear again: 0x%06X", fb.palette(22));
    // Luminance mapping.
    zero(bus, rec, 2);
    wr8(bus, rec, 1);
    EXPECT(control(6) == noErr, "SetGray on");
    put_colors(23, {{0, 0x1000, 0x8000, 0x4000}});
    EXPECT(fb.palette(23) == 0x808080u, "gray of equal brightness: 0x%06X", fb.palette(23));
    EXPECT(status(6) == noErr && rd8(bus, rec) == 1, "GetGray");
    zero(bus, rec, 2);
    control(6);
    // Indexed vs direct.
    EXPECT(put_colors(0, {{0, 0, 0, 0}}, 8) == controlErr, "DirectSetEntries on an indexed mode");

    // --- GrayPage -------------------------------------------------------------------
    zero(bus, rec, 12);
    EXPECT(control(5) == noErr, "GrayPage 8-bit");
    EXPECT(rd32(bus, kVram) == 0xFF00FF00u && rd32(bus, kVram + 636) == 0xFF00FF00u && rd32(bus, kVram + 640) == 0x00FF00FFu,
           "8-bit dither: 0x%08X 0x%08X", rd32(bus, kVram), rd32(bus, kVram + 640));
    wr16(bus, rec + 6, 1);
    EXPECT(control(5) == paramErr, "GrayPage page 1");
    EXPECT(switch_mode(4, 130) == noErr && fb.width() == 1152 && fb.depth() == 32 && fb.pitch() == 4608,
           "SwitchMode 1152x870x32");
    zero(bus, rec, 12);
    EXPECT(control(5) == noErr && rd32(bus, kVram) == 0xFFFFFFFFu && rd32(bus, kVram + 4) == 0 &&
               rd32(bus, kVram + 4608) == 0 && rd32(bus, kVram + 4608 * 869 + 4) == 0xFFFFFFFFu,
           "32-bit dither");
    EXPECT(put_colors(0, {{0, 0, 0, 0}}) == controlErr, "SetEntries on a direct mode");
    EXPECT(put_colors(0, {{0, 0xFFFF, 0, 0}}, 8) == noErr, "DirectSetEntries on a direct mode");
    // SetMode: depth only, same resolution.
    zero(bus, rec, 12);
    wr16(bus, rec, 129);
    EXPECT(control(2) == noErr && fb.width() == 1152 && fb.depth() == 15 && rd32(bus, rec + 8) == kVram, "SetMode 16-bit");

    // --- timing, connection, preferences, sync --------------------------------------
    zero(bus, rec, 20);
    wr32(bus, rec, 3);
    EXPECT(status(13) == noErr && rd32(bus, rec + 8) == 0x6465636Cu && rd32(bus, rec + 12) == 190 &&
               (rd32(bus, rec + 16) & 3) == 3,
           "GetModeTiming 1024x768: 'decl', VESA 60 Hz, valid and safe");
    zero(bus, rec, 20);
    wr32(bus, rec, 77);
    EXPECT(status(13) == paramErr, "GetModeTiming unknown");
    zero(bus, rec, 16);
    EXPECT(status(12) == noErr && rd16(bus, rec) == 6 && rd8(bus, rec + 2) == 6 && rd8(bus, rec + 3) == 0x23,
           "GetConnection: multiscan, raw sense 6, extended 0x23");
    zero(bus, rec, 16);
    wr16(bus, rec, 128);
    wr32(bus, rec + 2, 3);
    EXPECT(control(16) == noErr, "SavePreferredConfiguration");
    zero(bus, rec, 16);
    EXPECT(status(16) == noErr && rd16(bus, rec) == 128 && rd32(bus, rec + 2) == 3, "GetPreferredConfiguration");
    zero(bus, rec, 2);
    wr8(bus, rec, 3);
    wr8(bus, rec + 1, 3);
    EXPECT(control(11) == noErr && !fb.enabled(), "SetSync: syncs off blanks the display");
    zero(bus, rec, 2);
    EXPECT(control(11) == noErr && fb.enabled(), "SetSync with csFlags 0 acts as 0x3: back on");
    zero(bus, rec, 2);
    wr8(bus, rec, 0xFF);
    EXPECT(status(11) == noErr && rd8(bus, rec) == 0x43, "GetSync capabilities 0x%02X", rd8(bus, rec));

    // --- what isn't supported ---------------------------------------------------------
    EXPECT(control(0) == controlErr && control(9) == controlErr && control(99) == controlErr, "obsolete/unknown control");
    EXPECT(status(9) == statusErr && status(14) == statusErr && status(99) == statusErr, "obsolete/unknown status");
    zero(bus, rec, 2);
    wr8(bus, rec, 1);
    EXPECT(status(22) == noErr && rd8(bus, rec) == 0, "no hardware cursor");
    EXPECT(io(kRead, pb, kImmediate) == readErr && io(kWrite, pb, kImmediate) == writErr, "no read/write");
    EXPECT(io(kKillIO, pb, kImmediate) == noErr, "KillIO: nothing pending");

    // --- asynchronous request ---------------------------------------------------------
    host.completions.clear();
    EXPECT(call(kStatus, 99, kAsync) == noErr, "async: DoDriverIO returns IOCommandIsComplete's result");
    EXPECT(host.completions.size() == 1 && host.completions[0].second == statusErr, "async completion carries statusErr");

    // --- close, finalize --------------------------------------------------------------
    EXPECT(io(kClose, pb, kImmediate) == noErr, "close");
    EXPECT(io(kFinalize, init, kImmediate) == noErr && !fb.enabled(), "finalize blanks the display");
    EXPECT(host.vsl_disposed == 1 && reg(Framebuffer::IRQ_ENABLE) == 0, "finalize: interrupt off, service disposed");
    EXPECT(host.installs == 2 && host.installed_handler == kOldHandler && host.installed_refcon == kOldRefCon,
           "finalize puts the previous handler back");
    EXPECT(control(2) == controlErr, "control after finalize");

    // --- a second device with 1 MB of VRAM: the resolution list stops at 1152x870 ----
    {
        std::vector<u8> small_vram(1u << 20);
        Framebuffer small(small_vram.data(), static_cast<u32>(small_vram.size()));
        EXPECT(small.map(bus, 0x92000000u, 0x93000000u) && small.set_mode(1600, 1200, 8) == false,
               "second device (1600x1200 doesn't fit in 1 MB)");
        host.props["AAPL,address"] = {0x92000000u, 0x93000000u};  // Mac OS's logical addresses
        host.props.erase("driver-ist");                           // no interrupt for this one
        const u32 vsl_before = host.vsl_new, installs_before = host.installs;
        EXPECT(io(kInitialize, init, kImmediate) == noErr, "initialize on the small device");
        EXPECT(host.vsl_new == vsl_before && host.installs == installs_before,
               "no driver-ist: no VBL service, nothing installed");
        EXPECT(small.width() == 640 && small.depth() == 8 && small.enabled(), "unknown firmware mode: 640x480x8");
        u32 prev = 0xFFFFFFFEu, n = 0;
        for (; n < 10; ++n) {
            zero(bus, rec, 32);
            wr32(bus, rec, prev);
            if (call(kStatus, 17) != noErr || rd32(bus, rec + 4) == 0xFFFFFFFDu)
                break;
            prev = rd32(bus, rec + 4);
            EXPECT(rd16(bus, rec + 20) == (n < 2 ? 129u : 128u), "1 MB: mode %u max depth %u", n, rd16(bus, rec + 20));
        }
        EXPECT(n == 4, "1 MB: 4 resolutions, got %u", n);
        EXPECT(params(5, 128) == paramErr, "1280x1024 doesn't fit in 1 MB");
        EXPECT(io(kFinalize, init, kImmediate) == noErr, "finalize the small device");
    }
    EXPECT(ran && !cpu.checkstopped(), "every call returned");

    std::printf("guest ndrv: %d failures\n", failures);
    return failures ? 1 : 0;
}
