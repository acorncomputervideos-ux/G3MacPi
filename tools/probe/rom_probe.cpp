// SPDX-License-Identifier: GPL-3.0-or-later
//
// tools/probe/rom_probe.cpp — a development probe: powers on the Blue &
// White machine with our firmware, places a boot file (e.g. Apple's
// "Mac OS ROM", supplied by the user) at load-base as `load` would, runs
// init-program (which runs its bootinfo script) and, if a client program
// was started, runs the CPU for a while. Prints what happened: console
// output, the Forth error if any, and the CPU state at the end.
//
//   g3_rom_probe "Mac OS ROM" [milliseconds-of-guest-time] ["forth before init-program"]

#include "tools/a64sim/a64_sim.h"
#include "core/cpu/ppc750/disasm.h"
#include "platform/common/jit_report.h"
#include "core/machine/bw_firmware.h"
#include "core/machine/bw_nvram.h"
#include "core/devices/ata/atapi_cdrom.h"
#include "core/machine/machine_config.h"
#include "core/storage/block_device.h"

#include <cstdint>
#include "platform/common/disk_cache.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <cstdlib>
#include <string>
#include <vector>

using namespace g3;

namespace {
bool load_file(const char* path, std::vector<u8>& out)
{
    FILE* f = std::fopen(path, "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    out.resize(static_cast<size_t>(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s BOOTFILE [ms] [forth]\n", argv[0]);
        return 2;
    }
    std::vector<u8> file;
    {
        FILE* f = std::fopen(argv[1], "rb");
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", argv[1]);
            return 2;
        }
        for (int c; (c = std::fgetc(f)) != EOF;)
            file.push_back(static_cast<u8>(c));
        std::fclose(f);
    }
    const u64 ms = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 100;

    // G3_CONFIG=g3macpi.ini: the machine configuration the Pi's menu writes
    // (RAM, drives, display); docs/machine-config.md. Image paths are host
    // paths here.
    static MachineConfig mc;
    if (const char* cp = std::getenv("G3_CONFIG")) {
        std::vector<u8> text;
        if (!load_file(cp, text)) {
            std::fprintf(stderr, "cannot read %s\n", cp);
            return 1;
        }
        ConfigParseReport rep;
        parse_machine_config(reinterpret_cast<const char*>(text.data()), static_cast<u32>(text.size()), mc, rep);
        std::printf("[probe] config %s: %u MB, %ux%ux%u, %u line(s) ignored%s%s\n", cp, mc.ram_mb, mc.width,
                    mc.height, mc.depth, rep.ignored, rep.ignored ? " - first: " : "", rep.first_reason);
    } else {
        mc.drive[0].type = MachineConfig::DriveType::None;  // no config: no drives unless G3_DISK
    }
    // G3_RAM_MB: emulated RAM (default: the config's, 256).
    const u32 ram_mb = std::getenv("G3_RAM_MB") ? static_cast<u32>(std::strtoul(std::getenv("G3_RAM_MB"), nullptr, 10)) : mc.ram_mb;
    static std::vector<u8> ram(static_cast<size_t>(ram_mb) << 20), vram(8u << 20);
    YosemiteConfig cfg;
    cfg.ram = ram.data();
    cfg.ram_size = static_cast<u32>(ram.size());
    cfg.vram = vram.data();
    cfg.vram_size = static_cast<u32>(vram.size());
    // G3_JIT=MB: the CPU's decoded-block cache, with that much memory.
    static std::vector<u8> jit;
    if (const char* j = std::getenv("G3_JIT")) {
        jit.resize(static_cast<size_t>(std::strtoul(j, nullptr, 10)) << 20);
        cfg.jit_memory = jit.data();
        cfg.jit_size = static_cast<u32>(jit.size());
    }
    // G3_JITCODE=MB: generated AArch64 code too (with G3_JIT). On other
    // hosts it runs on the A64 simulator (slow; for checking the JIT).
    static std::vector<u32> jit_code;
    static const void* jit_range[2];
    if (const char* jc = std::getenv("G3_JITCODE"); jc && !jit.empty()) {
        jit_code.resize(static_cast<size_t>(std::strtoul(jc, nullptr, 10)) << 18);  // MB in words
        cfg.jit_code = jit_code.data();
        cfg.jit_code_words = static_cast<u32>(jit_code.size());
        jit_range[0] = jit_code.data();
        jit_range[1] = jit_code.data() + jit_code.size();
#if !defined(__aarch64__)
        cfg.jit_runner = a64::simulate_jit_block;
        cfg.jit_runner_context = jit_range;
#endif
    }
    // G3_JITBG: with G3_JITCODE, compile on another thread (jit_worker.h).
    cfg.jit_background = std::getenv("G3_JITBG") != nullptr;
    YosemiteMachine m(cfg);
    static std::atomic<bool> jitbg_stop{false};
    static std::thread jitbg_thread;
    if (ppc::JitWorker* w = m.jit_worker()) {
        jitbg_thread = std::thread([w] {
            while (!jitbg_stop.load(std::memory_order_acquire))
                if (!w->poll())
                    std::this_thread::yield();
        });
        std::printf("[probe] compiling on another thread\n");
    }
    if (!jit.empty())
        std::printf("[probe] block cache: %s, %u blocks; generated code: %s\n", m.cpu().block_cache() ? "on" : "FAILED",
                    m.cpu().block_cache() ? m.cpu().block_cache()->capacity() : 0,
                    m.jit_enabled() ? "on" : (jit_code.empty() ? "off" : "FAILED"));
    BwFirmware::Config fc;
    fc.width = mc.width;
    fc.height = mc.height;
    fc.depth = mc.depth;
    // G3_NDRV=file.pef: our display driver for the display node.
    std::vector<u8> ndrv;
    if (const char* path = std::getenv("G3_NDRV")) {
        if (FILE* f = std::fopen(path, "rb")) {
            for (int c; (c = std::fgetc(f)) != EOF;)
                ndrv.push_back(static_cast<u8>(c));
            std::fclose(f);
            fc.display_driver = ndrv.data();
            fc.display_driver_size = static_cast<u32>(ndrv.size());
            std::printf("[probe] display driver: %zu bytes from %s\n", ndrv.size(), path);
        } else {
            std::printf("[probe] cannot open %s\n", path);
            return 2;
        }
    }
    BwFirmware fw(m, fc);
    std::string console;
    fw.set_console([](void* c, const char* t, u32 n) {
        static_cast<std::string*>(c)->append(t, n);
        std::fwrite(t, 1, n, stdout);
        std::fflush(stdout);
    }, &console);
    if (!fw.ok()) {
        std::printf("firmware failed to power on\n");
        return 1;
    }
    if (!fw.place_file(file.data(), static_cast<u32>(file.size()), "hd:2,\\\\:tbxi")) {
        std::printf("the file doesn't fit at load-base\n");
        return 1;
    }
    std::printf("[probe] %zu bytes at load-base\n", file.size());
    if (argc > 3 && std::string(argv[3]) == "--words") {
        // Lists the words of the bootinfo script that our Forth doesn't define.
        std::string text(file.begin(), file.end());
        const size_t a = text.find("<BOOT-SCRIPT>"), b = text.find("</BOOT-SCRIPT>");
        if (a == std::string::npos || b == std::string::npos)
            return 1;
        text = text.substr(a + 13, b - a - 13);
        for (std::string::size_type p; (p = text.find("&gt;")) != std::string::npos;) text.replace(p, 4, ">");
        for (std::string::size_type p; (p = text.find("&lt;")) != std::string::npos;) text.replace(p, 4, "<");
        std::vector<std::string> seen;
        std::string tok;
        auto check = [&](const std::string& t) {
            if (t.empty())
                return;
            for (const auto& x : seen)
                if (x == t)
                    return;
            seen.push_back(t);
            const std::string cmd = "' " + t + " drop";
            if (!fw.command(cmd.c_str()))
                std::printf("missing: %s\n", t.c_str());
        };
        for (char c : text) {
            if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
                check(tok);
                tok.clear();
            } else {
                tok.push_back(c);
            }
        }
        check(tok);
        return 0;
    }
    if (argc > 3 && !fw.command(argv[3])) {
        std::printf("\n[probe] pre-command failed: %s\n", fw.forth().error_text());
        return 1;
    }
    // Log every client interface call: service, arguments, results; strings
    // for the services that take them.
    fw.client().set_trace([](void* c, const char* service, u32 a, bool handled) {
        ppc::MemoryInterface& mem = static_cast<BwFirmware*>(c)->client().memory();
        auto cell = [&](u32 addr) { u32 v = 0; mem.read32(addr, v); return v; };
        auto str = [&](u32 addr, u32 max = 64) {
            std::string s;
            for (u32 i = 0; i < max; ++i) {
                u8 ch = 0;
                if (!mem.read8(addr + i, ch) || !ch)
                    break;
                s.push_back(ch >= 0x20 && ch < 0x7F ? static_cast<char>(ch) : '.');
            }
            return s;
        };
        const u32 nargs = cell(a + 4), nrets = cell(a + 8);
        std::printf("[ci] %s%s(", service, handled ? "" : " UNHANDLED");
        for (u32 i = 0; i < nargs && i < 8; ++i)
            std::printf("%s%X", i ? " " : "", cell(a + 12 + 4 * i));
        std::printf(") ->");
        for (u32 i = 0; i < nrets && i < 6; ++i)
            std::printf(" %X", cell(a + 12 + 4 * (nargs + i)));
        const std::string sv = service;
        if (sv == "finddevice" || sv == "interpret")
            std::printf("  \"%s\"", str(cell(a + 12)).c_str());
        if (sv == "getprop" || sv == "getproplen" || sv == "setprop" || sv == "nextprop")
            std::printf("  \"%s\"", str(cell(a + 16)).c_str());
        if (sv == "call-method")
            std::printf("  \"%s\"", str(cell(a + 12)).c_str());
        if (sv == "write")
            std::printf("  \"%s\"", str(cell(a + 16), cell(a + 20) < 60 ? cell(a + 20) : 60).c_str());
        std::printf("\n");
    }, &fw);
    // G3_VIA=N: keep the last N VIA register accesses, printed at the end.
    struct ViaRing { std::vector<u32> e; std::vector<u32> pc; std::vector<u32> pc68; std::vector<u64> when; u32 pos = 0; u64 total = 0; YosemiteMachine* m = nullptr; };
    static ViaRing via;
    if (const char* vn = std::getenv("G3_VIA")) {
        via.e.assign(std::strtoul(vn, nullptr, 10), 0);
        via.pc.assign(via.e.size(), 0);
        via.pc68.assign(via.e.size(), 0);
        via.when.assign(via.e.size(), 0);
        via.m = &m;
        m.macio().via().set_trace([](void* c, u32 reg, u8 v, bool write) {
            auto* r = static_cast<ViaRing*>(c);
            if (r->e.empty())
                return;
            const u32 x = (write ? 0x80000000u : 0) | (reg << 8) | v;
            ++r->total;
            if (r->pos && r->e[(r->pos - 1) % r->e.size()] == x)
                return;  // repeats of the same access (polling) are kept once
            r->pc[r->pos % r->e.size()] = r->m->cpu().state().pc;
            // r24 holds the 68K program counter while Apple's 68K emulator runs.
            r->pc68[r->pos % r->e.size()] = r->m->cpu().state().gpr[24];
            r->when[r->pos % r->e.size()] = r->m->cpu().instructions_executed();
            r->e[r->pos++ % r->e.size()] = x;
        }, &via);
    }
    // Drives from the configuration: ata0.master = channel 0 device 0, etc.
    static std::vector<u8> drive_bytes[MachineConfig::kDrives];
    static RamBlockDevice* drive_dev[MachineConfig::kDrives] = {};
    static DiskCache* cached_dev[MachineConfig::kDrives] = {};
    auto dev = [&](u32 i) -> BlockDevice* { return cached_dev[i] ? static_cast<BlockDevice*>(cached_dev[i]) : drive_dev[i]; };
    static AtapiCdrom cdrom[MachineConfig::kDrives];
    for (u32 i = 0; i < MachineConfig::kDrives; ++i) {
        const MachineConfig::Drive& d = mc.drive[i];
        if (d.type == MachineConfig::DriveType::None)
            continue;
        AtaBus& bus = i / 2 == 0 ? m.macio().ide(0).bus() : m.cmd646().channel(0);  // as MacSession
        if (d.image[0]) {
            if (!load_file(d.image, drive_bytes[i]) || drive_bytes[i].size() < 512) {
                std::fprintf(stderr, "cannot read image %s\n", d.image);
                return 1;
            }
            drive_dev[i] = new RamBlockDevice(drive_bytes[i].data(), drive_bytes[i].size(), d.read_only);
            // G3_DISKCACHE: the drive's I/O through a DiskCache served by
            // another thread (platform/common/disk_cache.h), as on the Pi.
            if (std::getenv("G3_DISKCACHE")) {
                static std::vector<u8> lines[MachineConfig::kDrives], wq[MachineConfig::kDrives];
                lines[i].resize(64 * DiskCache::kLineBytes);
                wq[i].resize(256 * BlockDevice::kSectorSize);
                auto* c = new DiskCache(*drive_dev[i], lines[i].data(), 64, wq[i].data(), 256);
                c->set_wait([](void*) { std::this_thread::yield(); }, nullptr);
                new std::thread([c] {  // runs until the probe exits
                    for (;;)
                        if (!c->service())
                            std::this_thread::yield();
                });
                cached_dev[i] = c;
                std::printf("[probe] %s: through a disk cache (I/O on another thread)\n", MachineConfig::drive_name(i));
            }
        }
        if (d.type == MachineConfig::DriveType::CdRom) {
            // G3_ATAPI: print every packet command (up to 300) with its sense.
            if (std::getenv("G3_ATAPI")) {
                static u32 shown = 0;
                cdrom[i].set_packet_trace([](void*, const u8* c, u32 sense) {
                    if (shown++ >= std::strtoul(std::getenv("G3_ATAPI"), nullptr, 0))
                        return;
                    std::printf("[atapi]");
                    for (u32 k = 0; k < 12; ++k)
                        std::printf(" %02X", c[k]);
                    std::printf("  -> %06X\n", sense);
                }, nullptr);
            }
            bus.attach_cdrom(i % 2, &cdrom[i]);
            cdrom[i].insert(dev(i));  // nullptr: an empty drive
            std::printf("[probe] %s: CD-ROM, %s (%u blocks)\n", MachineConfig::drive_name(i),
                        d.image[0] ? d.image : "no disc", cdrom[i].blocks());
        } else if (drive_dev[i]) {
            bus.attach(i % 2, dev(i));
            std::printf("[probe] %s: disk %s (%zu sectors%s)\n", MachineConfig::drive_name(i), d.image,
                        drive_bytes[i].size() / 512, d.read_only ? ", read-only" : "");
        }
    }
    // The start-up disk, as MacSession chooses it: /chosen bootpath and the
    // loaded file's path.
    {
        bool has_image[MachineConfig::kDrives];
        for (u32 i = 0; i < MachineConfig::kDrives; ++i)
            has_image[i] = drive_dev[i] != nullptr;
        const u32 bd = choose_boot_drive(mc, has_image);
        char bp[256];
        if (bd < MachineConfig::kDrives && fw.set_boot_device(bd, *dev(bd), bp, sizeof bp)) {
            fw.place_file(file.data(), static_cast<u32>(file.size()), bp);
            std::printf("[probe] start-up disk: %s\n", bp);
        }
    }
    // G3_DISK=image[,channel]: a disk image (loaded into memory, writable)
    // as the master device on mac-io ATA channel 0 (or 1).
    static std::vector<u8> disk_bytes;
    static RamBlockDevice* disk_dev = nullptr;
    if (const char* dp = std::getenv("G3_DISK")) {
        std::string path(dp);
        u32 channel = 0;
        if (const size_t comma = path.rfind(','); comma != std::string::npos) {
            channel = static_cast<u32>(std::strtoul(path.c_str() + comma + 1, nullptr, 0)) & 1;
            path.resize(comma);
        }
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            std::fseek(f, 0, SEEK_END);
            disk_bytes.resize(static_cast<size_t>(std::ftell(f)));
            std::fseek(f, 0, SEEK_SET);
            if (std::fread(disk_bytes.data(), 1, disk_bytes.size(), f) != disk_bytes.size())
                disk_bytes.clear();
            std::fclose(f);
        }
        if (disk_bytes.empty()) {
            std::fprintf(stderr, "cannot read disk image %s\n", path.c_str());
            return 1;
        }
        disk_dev = new RamBlockDevice(disk_bytes.data(), disk_bytes.size());
        m.macio().ide(channel).bus().attach(0, disk_dev);
        std::printf("[probe] disk %s (%zu sectors) on ATA channel %u\n", path.c_str(),
                    disk_bytes.size() / 512, channel);
    }
    // G3_ATA: print every ATA command (both mac-io channels).
    if (std::getenv("G3_ATA")) {
        static YosemiteMachine* am = &m;
        for (u32 ch = 0; ch < 3; ++ch) {
            AtaBus& traced = ch < 2 ? m.macio().ide(ch).bus() : m.cmd646().channel(0);
            traced.set_command_trace([](void* c, const AtaBus::CommandTrace& t) {
                const u32 lba = (static_cast<u32>(t.device & 0x0F) << 24) | (t.lba_high << 16) |
                                (t.lba_mid << 8) | t.lba_low;
                std::printf("[ata%u] cmd %02X feat %02X count %3u lba %07X dev %02X  pc=%08X at %llu\n",
                            static_cast<u32>(reinterpret_cast<uintptr_t>(c)), t.command, t.features,
                            t.count, lba, t.device, am->cpu().state().pc,
                            static_cast<unsigned long long>(am->cpu().instructions_executed()));
            }, reinterpret_cast<void*>(static_cast<uintptr_t>(ch)));
        }
    }
    // G3_CODEWATCH=lo,hi: report (up to G3_CODEWATCH_MAX, default 40, times) when code runs in a 4 KB
    // page overlapping [lo, hi] (effective addresses).
    if (const char* cw = std::getenv("G3_CODEWATCH")) {
        char* end = nullptr;
        const u32 lo = static_cast<u32>(std::strtoul(cw, &end, 16));
        const u32 hi = (end && *end == ',') ? static_cast<u32>(std::strtoul(end + 1, nullptr, 16)) : lo;
        static YosemiteMachine* wm = &m;
        static u32 reports = 0, max_reports = 40;
        if (const char* mx = std::getenv("G3_CODEWATCH_MAX"))
            max_reports = static_cast<u32>(std::strtoul(mx, nullptr, 0));
        m.cpu().set_code_watch(lo, hi, [](void*, u32 pc) {
            if (reports++ < max_reports)
                std::printf("[watch] pc=%08X lr=%08X r24=%08X at %llu\n", pc,
                            wm->cpu().state().spr[ppc::spr::LR], wm->cpu().state().gpr[24],
                            static_cast<unsigned long long>(wm->cpu().instructions_executed()));
        }, nullptr);
    }
    // G3_CUDA: print every Cuda packet (host -> Cuda and Cuda -> host).
    if (std::getenv("G3_CUDA")) {
        static YosemiteMachine* cm = &m;
        m.macio().cuda().set_packet_trace([](void*, const u8* b, u32 n, bool to_host) {
            std::printf("[cuda] %s", to_host ? "<-" : "->");
            for (u32 i = 0; i < n; ++i)
                std::printf(" %02X", b[i]);
            std::printf("   r24=%08X at %llu\n", cm->cpu().state().gpr[24],
                        static_cast<unsigned long long>(cm->cpu().instructions_executed()));
        }, nullptr);
    }
    const bool ok = fw.command("init-program");
    std::printf("\n[probe] init-program: %s\n", ok ? "completed" : "FAILED");
    if (!ok)
        std::printf("[probe] Forth error: %s\n", fw.forth().error_text());
    if (fw.client_started()) {
        std::printf("[probe] client started at 0x%08X; running %llu ms of guest time\n", fw.client_entry(),
                    static_cast<unsigned long long>(ms));
        // G3_EXC=N: log up to N exceptions (any run mode), skipping DSI/ISI page
        // faults the OS handles itself unless G3_EXC_ALL is set.
        struct ExcLog { u32 left; bool all; ppc::Ppc750* cpu; u64 count[32]; };
        static ExcLog exc{0, false, nullptr, {}};
        if (const char* e = std::getenv("G3_EXC")) {
            exc.left = static_cast<u32>(std::strtoul(e, nullptr, 10));
            exc.all = std::getenv("G3_EXC_ALL") != nullptr;
            exc.cpu = &m.cpu();
            m.cpu().set_exception_hook([](void* c, u32 vector, u32 srr0, u32 srr1) {
                auto* x = static_cast<ExcLog*>(c);
                ++x->count[(vector >> 8) & 31];
                static const char* only = std::getenv("G3_EXC_VEC");  // e.g. "200": only that vector
                static const u64 from = std::getenv("G3_EXC_FROM") ? std::strtoull(std::getenv("G3_EXC_FROM"), nullptr, 0) : 0;
                if (x->cpu->instructions_executed() < from)
                    return;
                if (only && vector != std::strtoul(only, nullptr, 16))
                    return;
                if (!x->left || (!only && !x->all && (vector == 0x300 || vector == 0x400 || vector == 0x900 ||
                                                      vector == 0x500 || vector == 0x700)))
                    return;
                --x->left;
                const auto& st = x->cpu->state();
                std::printf("[exc] vector %04X srr0=%08X srr1=%08X dar=%08X dsisr=%08X lr=%08X dec=%08X insns=%llu\n",
                            vector, srr0, srr1, st.spr[ppc::spr::DAR], st.spr[ppc::spr::DSISR], st.lr(),
                            st.spr[ppc::spr::DEC], static_cast<unsigned long long>(x->cpu->instructions_executed()));
            }, &exc);
        }
        if (const char* ps = std::getenv("G3_PCSTOP")) {  // TEMP
            char* e = nullptr;
            const u32 pc = static_cast<u32>(std::strtoul(ps, &e, 16));
            const u64 after = std::strtoull(e + 1, nullptr, 10);
            while (m.cpu().instructions_executed() < after) m.run_for(100000000ull);
            for (u64 i = 0; i < 400000000ull; ++i) {
                if (m.cpu().state().pc == pc) {
                    auto& st = m.cpu().state();
                    std::printf("[pcstop] pc=%08X at %llu r10=%08X r11=%08X r12=%08X\n", st.pc, (unsigned long long)m.cpu().instructions_executed(), st.gpr[10], st.gpr[11], st.gpr[12]);
                    break;
                }
                m.run_for(2857);
            }
            return 0;
        }
        if (std::getenv("G3_STEP")) {
            // Step by step (no device time passes), reporting each exception
            // entry: the vector, SRR0/SRR1, DAR/DSISR, and the last PCs.
            const u64 steps = ms * 350000ull;  // ~ms at 350 MHz, one instruction per cycle
            u32 history[16] = {}, hp = 0, reported = 0;
            auto& st = m.cpu().state();
            // G3_WATCH=physaddr: report every change of that word, with the PC.
            const char* watch_env = std::getenv("G3_WATCH");
            const u32 watch = watch_env ? static_cast<u32>(std::strtoul(watch_env, nullptr, 16)) : 0;
            u32 watched = 0, watch_reports = 0;
            if (watch_env)
                m.bus().read32(watch, watched);
            for (u64 i = 0; i < steps && !m.cpu().checkstopped() && !fw.client_exited(); ++i) {
                const u32 before = st.pc;
                history[hp++ % 16] = before;
                m.cpu().step();
                const u32 after = st.pc;
                if (watch_env && watch_reports < 40) {
                    u32 now = 0;
                    m.bus().read32(watch, now);
                    if (now != watched) {
                        ++watch_reports;
                        std::printf("[watch] step %llu pc=%08X: %08X -> %08X (msr=%08X)\n",
                                    static_cast<unsigned long long>(i), before, watched, now, st.msr);
                        watched = now;
                    }
                }
                const bool vector = (after & 0xFF) == 0 && (after & ~0xFFF0FF00u) == 0 && after >= 0x100 &&
                                    (after & 0xFFFF) <= 0x1F00 && after != before + 4;
                if (vector && reported < 40 && (st.spr[ppc::spr::SRR0] == before || st.spr[ppc::spr::SRR0] == before + 4)) {
                    ++reported;
                    std::printf("[probe] exception -> 0x%08X at step %llu: srr0=0x%08X srr1=0x%08X dar=0x%08X "
                                "dsisr=0x%08X msr=0x%08X\n[probe]   last pcs:", after,
                                static_cast<unsigned long long>(i), st.spr[ppc::spr::SRR0], st.spr[ppc::spr::SRR1],
                                st.spr[ppc::spr::DAR], st.spr[ppc::spr::DSISR], st.msr);
                    for (u32 k = 0; k < 16; ++k)
                        std::printf(" %08X", history[(hp + k) % 16]);
                    if (after == 0x700 || after == 0x00000700) {
                        // The instruction that raised it (SRR0 is physical while the handler
                        // runs with translation off; read through the CPU's current view).
                        u32 word = 0;
                        fw.client().memory().read32(st.spr[ppc::spr::SRR0], word);
                        char text[96];
                        ppc::disassemble(st.spr[ppc::spr::SRR0], word, text, sizeof text);
                        std::printf("[probe]   instruction %08X  %s\n", word, text);
                    }
                    std::printf("[probe]   sdr1=%08X sr0=%08X sr15=%08X", st.spr[ppc::spr::SDR1], st.sr[0], st.sr[15]);
                    for (u32 b = 0; b < 4; ++b)
                        std::printf(" dbat%u=%08X/%08X", b, st.spr[ppc::spr::DBAT0U + 2 * b],
                                    st.spr[ppc::spr::DBAT0U + 2 * b + 1]);
                    std::printf("\n");
                }
            }
        }
        // G3_TRACE68=first-instruction: from there on, every change of the
        // 68K emulator's PC (r24) with d0-d7 (r8-r15), a0 (r16) and a7 (r1),
        // in a ring of 4096; frozen when the PC reaches the ROM's fatal-error
        // routine (FFC046C0..FFC046F0); printed at the end.
        struct Trace68 {
            u64 from = 0, to = ~0ull;  // G3_TRACE68_TO: stop recording there
            u32 a7_hi = 0;             // G3_TRACE68_A7=hex: freeze when a7's top 16 bits become this
            u64 count = 0;
            ppc::Ppc750* cpu = nullptr;
            bool frozen = false;
            u32 last = 0;
            struct E { u64 n; u32 pc, d[8], a0, a1, a7; } ring[4096];
            u32 pos = 0, used = 0;
        };
        static Trace68 tr;
        // G3_MMIO=from,to: device-register accesses while the instruction
        // count is in [from, to), with the PC and the 68K PC (r24).
        struct MmioLog { u64 from = 0, to = 0; ppc::Ppc750* cpu = nullptr; u32 shown = 0;
                         u32 last_addr = 0, last_value = 0; bool last_write = false; u64 repeats = 0; };
        static MmioLog ml;
        if (const char* mm = std::getenv("G3_MMIO")) {
            char* end = nullptr;
            ml.from = std::strtoull(mm, &end, 0);
            ml.to = end && *end == ',' ? std::strtoull(end + 1, nullptr, 0) : ~0ull;
            ml.cpu = &m.cpu();
            m.bus().set_mmio_trace([](void* c, u32 addr, u32 size, bool write, u32 value) {
                auto* l = static_cast<MmioLog*>(c);
                const u64 n = l->cpu->instructions_executed();
                if (n < l->from || n >= l->to || l->shown >= (std::getenv("G3_MMIO_MAX") ? std::strtoul(std::getenv("G3_MMIO_MAX"), nullptr, 10) : 3000))
                    return;
                if (!write && !l->last_write && addr == l->last_addr && value == l->last_value) {
                    ++l->repeats;  // the same read again (a polling loop): counted, not printed
                    return;
                }
                if (l->repeats)
                    std::printf("[mmio]   (the read above %llu more times)\n", static_cast<unsigned long long>(l->repeats));
                l->repeats = 0;
                l->last_addr = addr;
                l->last_value = value;
                l->last_write = write;
                ++l->shown;
                const auto& st = l->cpu->state();
                std::printf("[mmio] %llu %s %08X/%u = %08X  pc=%08X r24=%08X\n", static_cast<unsigned long long>(n),
                            write ? "W" : "R", addr, size, value, st.pc, st.gpr[24]);
            }, &ml);
        }
        // G3_PC68=lo,hi: each entry of the 68K PC (r24) into [lo, hi], with
        // d0, d1, a0, a1, a7 (first 300).
        struct Pc68 { u32 lo = 0, hi = 0; bool in = false; u32 shown = 0; ppc::Ppc750* cpu = nullptr; const u8* ram = nullptr; u32 ram_size = 0; };
        static Pc68 p68;
        if (const char* pr = std::getenv("G3_PC68")) {
            char* end = nullptr;
            p68.lo = static_cast<u32>(std::strtoul(pr, &end, 16));
            p68.hi = end && *end == ',' ? static_cast<u32>(std::strtoul(end + 1, nullptr, 16)) : p68.lo;
            p68.cpu = &m.cpu();
            p68.ram = ram.data();
            p68.ram_size = static_cast<u32>(ram.size());
            m.cpu().set_step_hook([](void* c, const ppc::CpuState& s) {
                auto* p = static_cast<Pc68*>(c);
                const u32 pc = s.gpr[24];
                const bool in = pc >= p->lo && pc <= p->hi;
                if (in && !p->in && p->shown < 300) {
                    ++p->shown;
                    std::printf("[pc68] %llu pc=%08X d0=%08X d1=%08X a0=%08X a1=%08X a7=%08X",
                                static_cast<unsigned long long>(p->cpu->instructions_executed()), pc, s.gpr[8],
                                s.gpr[9], s.gpr[16], s.gpr[17], s.gpr[1]);
                    if (std::getenv("G3_PC68_A6ARGS")) {  // longs at a6+8 and a6+12 (Pascal-style arguments)
                        for (u32 k = 8; k <= 12; k += 4) {
                            u32 pa = 0, v = 0;
                            if (p->cpu->probe_translate(s.gpr[22] + k, false, pa) && pa + 4 <= p->ram_size)
                                v = load_be32(p->ram + pa);
                            std::printf(" [a6+%u]=%08X", k, v);
                        }
                    }
                    if (std::getenv("G3_PC68_A1STR")) {  // the bytes a1 points to, as text
                        std::printf("  a1->\"");
                        for (u32 i = 0; i < 40; ++i) {
                            u32 pa = 0;
                            if (!p->cpu->probe_translate(s.gpr[17] + i, false, pa) || pa >= p->ram_size)
                                break;
                            const u8 ch = p->ram[pa];
                            std::printf("%c", ch >= 32 && ch < 127 ? ch : '.');
                        }
                        std::printf("\"");
                    }
                    std::printf("\n");
                }
                p->in = in;
            }, &p68);
        }
        // G3_PPAGES=from,to: the physical code pages executed while the
        // instruction count is in [from, to), with the first PC of each.
        struct PPages { u64 from = 0, to = 0; ppc::Ppc750* cpu = nullptr; u32 last = ~0u; u32 pages[4096]; u32 first_pc[4096]; u32 n = 0; };
        static PPages pp;
        if (const char* ps = std::getenv("G3_PPAGES")) {
            char* end = nullptr;
            pp.from = std::strtoull(ps, &end, 0);
            pp.to = end && *end == ',' ? std::strtoull(end + 1, nullptr, 0) : ~0ull;
            pp.cpu = &m.cpu();
            m.cpu().set_step_hook([](void* c, const ppc::CpuState& s) {
                auto* p = static_cast<PPages*>(c);
                const u64 k = p->cpu->instructions_executed();
                if (k < p->from || k >= p->to)
                    return;
                u32 pa = s.pc;
                if (s.msr & ppc::msr::IR)
                    p->cpu->probe_translate(s.pc, false, pa);  // (data translation; for listing only)
                const u32 page = pa >> 12;
                if (page == p->last)
                    return;
                p->last = page;
                for (u32 i = 0; i < p->n; ++i)
                    if (p->pages[i] == page)
                        return;
                if (p->n < 4096) {
                    p->pages[p->n] = page;
                    p->first_pc[p->n++] = s.pc;
                }
            }, &pp);
        }
        // G3_PCHIST=lo,hi,from,to: instructions executed per 64-byte block of
        // [lo, hi] (effective addresses) while the count is in [from, to).
        struct PcHist { u32 lo = 0, hi = 0; u64 from = 0, to = ~0ull; ppc::Ppc750* cpu = nullptr; u32 counts[65536] = {}; };
        static PcHist ph;
        if (const char* hs = std::getenv("G3_PCHIST")) {
            unsigned long long a = 0, b = 0, c = 0, d = ~0ull;
            std::sscanf(hs, "%llx,%llx,%llu,%llu", &a, &b, &c, &d);
            ph.lo = static_cast<u32>(a);
            ph.hi = static_cast<u32>(b);
            ph.from = c;
            ph.to = d;
            ph.cpu = &m.cpu();
            m.cpu().set_step_hook([](void* ctx, const ppc::CpuState& s) {
                auto* h = static_cast<PcHist*>(ctx);
                if (s.pc < h->lo || s.pc > h->hi)
                    return;
                const u64 k = h->cpu->instructions_executed();
                if (k < h->from || k >= h->to)
                    return;
                const u32 i = (s.pc - h->lo) >> 6;
                if (i < 65536)
                    ++h->counts[i];
            }, &ph);
        }
        // G3_REGSAT=pc: print the registers the first time the CPU is about
        // to execute that address.
        // G3_REGSAT_N=n: instead, one line (lr, r3-r9) for each of the first n hits;
        // G3_REGSAT_LR=lr: only hits with that link register.
        struct RegsAt { u32 pc = 0; bool done = false; u64 from = 0; ppc::Ppc750* cpu = nullptr; const u8* ram = nullptr; u32 ram_size = 0;
                        u32 lines = 0; u32 lr = 0; bool lr_filter = false; };
        static RegsAt ra;
        // G3_REGSAT_PATH=n,o1,o2...: 8 words at rn, then at [rn+o1], then at [[rn+o1]+o2]... (offsets hex)
        struct RegsPath {
            static void print(const RegsAt* r, const ppc::CpuState& s) {
                if (const char* path = std::getenv("G3_REGSAT_PATH")) {
                    char* e = nullptr;
                    if (path[0] == 'p') {  // p<hex>: 32 bytes of physical memory, as 8 little- and big-endian words
                        // p<hex>[,len]: len bytes (hex, default 32), 32 per line
                        const u32 pa = static_cast<u32>(std::strtoul(path + 1, &e, 16));
                        const u32 n = e && *e == ',' ? static_cast<u32>(std::strtoul(e + 1, nullptr, 16)) : 32;
                        for (u32 k = 0; k < n && pa + k + 4 <= r->ram_size; k += 4) {
                            if (k % 32 == 0)
                                std::printf("%s[regs] phys %08X:", k ? "\n" : "", pa + k);
                            std::printf(" %08X", load_be32(r->ram + pa + k));
                        }
                        std::printf("\n");
                        return;
                    }
                    u32 a = s.gpr[std::strtoul(path, &e, 10) & 31];
                    if (e && *e == ':')  // n:off: start at rn + off (hex, may be negative)
                        a += static_cast<u32>(std::strtol(e + 1, &e, 16));
                    auto word = [&](u32 ea) {
                        u32 pa = 0;
                        return r->cpu->probe_translate(ea, false, pa) && pa + 4 <= r->ram_size ? load_be32(r->ram + pa) : 0u;
                    };
                    for (;;) {
                        u32 path_pa = 0;
                        const bool mapped = r->cpu->probe_translate(a, false, path_pa);
                        std::printf("[regs] [%08X] (pa %08X%s):", a, path_pa, mapped ? "" : " unmapped");
                        for (u32 k = 0; k < 32; k += 4)
                            std::printf(" %08X", word(a + k));
                        std::printf("  \"");
                        for (u32 k = 0; k < 32; ++k) {
                            const u8 ch = static_cast<u8>(word((a + k) & ~3u) >> (24 - 8 * ((a + k) & 3)));
                            std::printf("%c", ch >= 32 && ch < 127 ? ch : 0x2E);
                        }
                        std::printf("\"\n");
                        if (!e || *e != ',')
                            break;
                        a = word(a + static_cast<u32>(std::strtoul(e + 1, &e, 16)));
                    }
                }
            }
        };
        if (const char* rs = std::getenv("G3_REGSAT")) {
            char* rend = nullptr;
            ra.pc = static_cast<u32>(std::strtoul(rs, &rend, 16));
            if (rend && *rend == ',')  // G3_REGSAT=pc,from: the first time at or after that instruction count
                ra.from = std::strtoull(rend + 1, nullptr, 0);
            ra.cpu = &m.cpu();
            ra.ram = ram.data();
            ra.ram_size = static_cast<u32>(ram.size());
            if (const char* n = std::getenv("G3_REGSAT_N"))
                ra.lines = static_cast<u32>(std::strtoul(n, nullptr, 0));
            if (const char* l = std::getenv("G3_REGSAT_LR")) {
                ra.lr = static_cast<u32>(std::strtoul(l, nullptr, 16));
                ra.lr_filter = true;
            }
            m.cpu().set_step_hook([](void* c, const ppc::CpuState& s) {
                auto* r = static_cast<RegsAt*>(c);
                if (r->done || (s.pc != r->pc && r->pc != 0xFFFFFFFFu) || r->cpu->instructions_executed() < r->from)  // pc FFFFFFFF: any
                    return;
                if (r->lr_filter && s.lr() != r->lr)
                    return;
                if (r->lines) {
                    u32 tv_pa = 0, tv = 0;
                    if (r->cpu->probe_translate(s.gpr[12], false, tv_pa) && tv_pa + 4 <= r->ram_size)
                        tv = load_be32(r->ram + tv_pa);
                    std::printf("[hit] lr=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x r8=%08x r9=%08x r12=%08x [r12]=%08x r31=%08x (instruction %llu)\n",
                                s.lr(), s.gpr[3], s.gpr[4], s.gpr[5], s.gpr[6], s.gpr[7], s.gpr[8], s.gpr[9], s.gpr[12], tv, s.gpr[31],
                                static_cast<unsigned long long>(r->cpu->instructions_executed()));
                    RegsPath::print(r, s);
                    r->done = --r->lines == 0;
                    return;
                }
                r->done = true;
                std::printf("[regs] at %08X msr=%08X lr=%08X ctr=%08X dec=%08X  (instruction %llu)\n", s.pc, s.msr, s.lr(), s.ctr(), s.spr[ppc::spr::DEC],
                            static_cast<unsigned long long>(r->cpu->instructions_executed()));
                for (u32 i = 0; i < 32; i += 8)
                    std::printf("[regs] r%-2u %08X %08X %08X %08X %08X %08X %08X %08X\n", i, s.gpr[i], s.gpr[i + 1],
                                s.gpr[i + 2], s.gpr[i + 3], s.gpr[i + 4], s.gpr[i + 5], s.gpr[i + 6], s.gpr[i + 7]);
                RegsPath::print(r, s);
                std::printf("[regs] stack at r1:");  // 32 words, through the data translation
                for (u32 k = 0; k < 128; k += 4) {
                    u32 pa = 0, v = 0;
                    if (r->cpu->probe_translate(s.gpr[1] + k, false, pa) && pa + 4 <= r->ram_size)
                        v = load_be32(r->ram + pa);
                    std::printf("%s%08X", k % 32 ? " " : "\n[regs]   ", v);
                }
                std::printf("\n");
            }, &ra);
        }
        // G3_MTDEC=from: from that instruction count, log up to 60 decrementer
        // writes (mtspr 22) made in real mode: pc, the value, DEC before.
        struct MtDec { u64 from = 0; u32 left = 60; ppc::Ppc750* cpu = nullptr; const u8* ram = nullptr; u32 ram_size = 0; };
        static MtDec md;
        if (const char* mds = std::getenv("G3_MTDEC")) {
            md.from = std::strtoull(mds, nullptr, 0);
            md.cpu = &m.cpu();
            md.ram = ram.data();
            md.ram_size = static_cast<u32>(ram.size());
            m.cpu().set_step_hook([](void* c, const ppc::CpuState& s) {
                auto* d = static_cast<MtDec*>(c);
                if (!d->left || (s.msr & ppc::msr::IR) || s.pc + 4 > d->ram_size || d->cpu->instructions_executed() < d->from)
                    return;
                const u32 insn = load_be32(d->ram + s.pc);
                if ((insn & 0xFC1FFFFFu) != 0x7C1603A6u)
                    return;
                --d->left;
                std::printf("[mtdec] pc=%08X value=%08X dec=%08X tb=%016llX (instruction %llu)\n", s.pc, s.gpr[(insn >> 21) & 31],
                            s.spr[ppc::spr::DEC], static_cast<unsigned long long>(s.tb),
                            static_cast<unsigned long long>(d->cpu->instructions_executed()));
            }, &md);
        }
        // G3_ITRACE=from,to: every instruction (pc, disassembly, r3-r12, r24)
        // while the instruction count is in [from, to).
        struct ITrace { u64 from = 0, to = 0; ppc::Ppc750* cpu = nullptr; const u8* ram = nullptr; u32 ram_size = 0; PhysicalBus* bus = nullptr; };
        static ITrace it;
        if (const char* its = std::getenv("G3_ITRACE")) {
            char* e = nullptr;
            it.from = std::strtoull(its, &e, 0);
            it.to = e && *e == ',' ? std::strtoull(e + 1, nullptr, 0) : it.from + 1000;
            it.cpu = &m.cpu();
            it.ram = ram.data();
            it.ram_size = static_cast<u32>(ram.size());
            it.bus = &m.bus();
            m.cpu().set_step_hook([](void* c, const ppc::CpuState& s) {
                auto* t = static_cast<ITrace*>(c);
                const u64 n = t->cpu->instructions_executed();
                if (n < t->from || n >= t->to)
                    return;
                u32 pa = 0, insn = 0;
                if (t->cpu->probe_translate(s.pc, false, pa)) {
                    if (pa + 4 <= t->ram_size)
                        insn = load_be32(t->ram + pa);
                    else
                        t->bus->read32(pa, insn);
                }
                char text[96];
                if (!ppc::disassemble(s.pc, insn, text, sizeof text))
                    std::snprintf(text, sizeof text, ".long 0x%08X", insn);
                std::printf("[it] %llu %08X %-32s r1=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X r11=%08X r12=%08X r24=%08X\n",
                            static_cast<unsigned long long>(n), s.pc, text, s.gpr[1], s.gpr[3], s.gpr[4], s.gpr[5], s.gpr[6],
                            s.gpr[7], s.gpr[8], s.gpr[9], s.gpr[10], s.gpr[11], s.gpr[12], s.gpr[24]);
            }, &it);
        }
        // G3_R3RET=from,to,lo,hi: each blr in [from, to) with r3 in [lo, hi]
        // (signed, decimal), e.g. CFM error codes -2830,-2800 (first 60).
        struct R3Ret { u64 from = 0, to = 0; s32 lo = 0, hi = 0; u32 left = 100000; ppc::Ppc750* cpu = nullptr; const u8* ram = nullptr; u32 ram_size = 0; PhysicalBus* bus = nullptr; };
        static R3Ret r3r;
        if (const char* rr = std::getenv("G3_R3RET")) {
            char* e = nullptr;
            r3r.from = std::strtoull(rr, &e, 0);
            r3r.to = std::strtoull(e + 1, &e, 0);
            r3r.lo = static_cast<s32>(std::strtol(e + 1, &e, 0));
            r3r.hi = static_cast<s32>(std::strtol(e + 1, &e, 0));
            r3r.cpu = &m.cpu();
            r3r.ram = ram.data();
            r3r.ram_size = static_cast<u32>(ram.size());
            r3r.bus = &m.bus();
            m.cpu().set_step_hook([](void* c, const ppc::CpuState& s) {
                auto* t = static_cast<R3Ret*>(c);
                const u64 n = t->cpu->instructions_executed();
                const s32 v = static_cast<s32>(s.gpr[3]);
                if (!t->left || n < t->from || n >= t->to || v < t->lo || v > t->hi)
                    return;
                u32 pa = 0, insn = 0;
                if (!t->cpu->probe_translate(s.pc, false, pa))
                    return;
                if (pa + 4 <= t->ram_size)
                    insn = load_be32(t->ram + pa);
                else
                    t->bus->read32(pa, insn);
                if (insn != 0x4E800020u)
                    return;
                --t->left;
                std::printf("[r3ret] %llu blr at %08X r3=%d lr=%08X r1=%08X r24=%08X\n", static_cast<unsigned long long>(n), s.pc, v,
                            s.lr(), s.gpr[1], s.gpr[24]);
            }, &r3r);
        }
        // G3_R1IN=lo,hi[,from]: the first 40 times r1 enters [lo, hi) (pc, lr, r24).
        struct R1In { u32 lo = 0, hi = 0; u64 from = 0; bool in = false; u32 left = 40; ppc::Ppc750* cpu = nullptr; };
        static R1In r1in;
        if (const char* rs = std::getenv("G3_R1IN")) {
            char* e = nullptr;
            r1in.lo = static_cast<u32>(std::strtoul(rs, &e, 16));
            r1in.hi = static_cast<u32>(std::strtoul(e + 1, &e, 16));
            if (*e == ',')
                r1in.from = std::strtoull(e + 1, nullptr, 0);
            r1in.cpu = &m.cpu();
            m.cpu().set_step_hook([](void* c, const ppc::CpuState& s) {
                auto* r = static_cast<R1In*>(c);
                const bool in = s.gpr[1] >= r->lo && s.gpr[1] < r->hi;
                if (in && !r->in && r->left && r->cpu->instructions_executed() >= r->from) {
                    --r->left;
                    std::printf("[r1in] %llu pc=%08X lr=%08X r1=%08X r24=%08X msr=%08X\n",
                                static_cast<unsigned long long>(r->cpu->instructions_executed()), s.pc, s.lr(), s.gpr[1],
                                s.gpr[24], s.msr);
                }
                r->in = in;
            }, &r1in);
        }
        // G3_MEMWATCH=physaddr: report every change of that byte of RAM
        // (checked after each instruction) with the PC and the 68K PC (r24).
        struct MemWatch { const u8* p = nullptr; u8 last = 0; ppc::Ppc750* cpu = nullptr; u32 shown = 0; };
        static MemWatch mw;
        if (const char* mws = std::getenv("G3_MEMWATCH")) {
            const u32 pa = static_cast<u32>(std::strtoul(mws, nullptr, 16));
            mw.p = pa < ram.size() ? ram.data() + pa : nullptr;
            mw.cpu = &m.cpu();
            if (mw.p) {
                mw.last = *mw.p;
                std::printf("[memwatch] %08X starts as %02X\n", pa, mw.last);
                m.cpu().set_step_hook([](void* c, const ppc::CpuState& s) {
                    auto* w = static_cast<MemWatch*>(c);
                    if (*w->p == w->last || w->shown >= (std::getenv("G3_MEMWATCH_MAX") ? std::strtoul(std::getenv("G3_MEMWATCH_MAX"), nullptr, 10) : 200))
                        return;
                    std::printf("[memwatch] %02X -> %02X after pc=%08X (lr=%08X r24=%08X) at %llu\n", w->last, *w->p,
                                s.pc, s.lr(), s.gpr[24],
                                static_cast<unsigned long long>(w->cpu->instructions_executed()));
                    w->last = *w->p;
                    ++w->shown;
                }, &mw);
            }
        }
        if (const char* t68 = std::getenv("G3_TRACE68")) {
            tr.from = std::strtoull(t68, nullptr, 0);
            if (const char* tt = std::getenv("G3_TRACE68_TO"))
                tr.to = std::strtoull(tt, nullptr, 0);
            if (const char* ta = std::getenv("G3_TRACE68_A7"))
                tr.a7_hi = static_cast<u32>(std::strtoul(ta, nullptr, 16));
            tr.cpu = &m.cpu();
            m.cpu().set_step_hook([](void* c, const ppc::CpuState& s) {
                auto* t = static_cast<Trace68*>(c);
                if (t->frozen || s.gpr[24] == t->last)
                    return;
                const u64 n = t->cpu->instructions_executed();
                t->last = s.gpr[24];
                if (n < t->from)
                    return;
                if (n >= t->to) {
                    t->frozen = true;
                    return;
                }
                Trace68::E& e = t->ring[t->pos];
                e.n = n;
                e.pc = s.gpr[24];
                for (u32 i = 0; i < 8; ++i)
                    e.d[i] = s.gpr[8 + i];
                e.a0 = s.gpr[16];
                e.a1 = s.gpr[17];
                e.a7 = s.gpr[1];
                t->pos = (t->pos + 1) % 4096;
                if (t->used < 4096)
                    ++t->used;
                if (e.pc >= 0xFFC046C0u && e.pc <= 0xFFC046F0u)
                    t->frozen = true;
                if (t->a7_hi && (e.a7 >> 16) == t->a7_hi)
                    t->frozen = true;
            }, &tr);
        }
        if (false) {
        } else if (const char* every = std::getenv("G3_SAMPLE_MS")) {
            // Run in slices, printing where the CPU is after each one.
            const u64 slice = std::strtoull(every, nullptr, 10);
            for (u64 t = 0; slice && t < ms && !fw.client_exited(); t += slice) {
                m.run_for(slice * 1000000000ull);
                const auto& st = m.cpu().state();
                std::printf("[sample] %6llu ms  pc=%08X lr=%08X msr=%08X hid0=%08X r1=%08X r3=%08X r24=%08X r25=%08X  %llu insns\n",
                            static_cast<unsigned long long>(t + slice), st.pc, st.lr(), st.msr,
                            st.spr[ppc::spr::HID0], st.gpr[1], st.gpr[3],
                            st.gpr[24], st.gpr[25], static_cast<unsigned long long>(m.cpu().instructions_executed()));
                std::fflush(stdout);
            }
        } else {
            m.run_for(ms * 1000000000ull);  // picoseconds
        }
        if (ph.cpu) {
            for (u32 i = 0; i < 65536; ++i)
                if (ph.counts[i])
                    std::printf("[pchist] %08X %u\n", ph.lo + (i << 6), ph.counts[i]);
        }
        if (pp.cpu) {
            std::printf("[ppages] %u pages\n", pp.n);
            for (u32 i = 0; i < pp.n; ++i)
                std::printf("[ppages] pa %08X  first pc %08X\n", pp.pages[i] << 12, pp.first_pc[i]);
        }
        if (tr.cpu) {
            const u32 show = std::getenv("G3_TRACE68_N") ? static_cast<u32>(std::strtoul(std::getenv("G3_TRACE68_N"), nullptr, 10)) : 300;
            const u32 n = tr.used < show ? tr.used : show;
            std::printf("[t68] %s; the last %u PC changes:\n", tr.frozen ? "reached the fatal-error routine" : "not frozen", n);
            for (u32 i = 0; i < n; ++i) {
                const Trace68::E& e = tr.ring[(tr.pos + 4096 - n + i) % 4096];
                std::printf("[t68] %llu pc=%08X d0=%08X d1=%08X d2=%08X d3=%08X d4=%08X d5=%08X d6=%08X d7=%08X a0=%08X a1=%08X a7=%08X\n",
                            static_cast<unsigned long long>(e.n), e.pc, e.d[0], e.d[1], e.d[2], e.d[3], e.d[4], e.d[5],
                            e.d[6], e.d[7], e.a0, e.a1, e.a7);
            }
        }
        if (std::getenv("G3_EXC")) {
            std::printf("[exc] totals:");
            for (u32 v = 0; v < 32; ++v)
                if (exc.count[v])
                    std::printf(" %03X:%llu", v << 8, static_cast<unsigned long long>(exc.count[v]));
            std::printf("\n");
        }
        if (fw.firmware().exception_vector())
            std::printf("\n[probe] unexpected exception 0x%X: srr0=0x%08X srr1=0x%08X\n",
                        fw.firmware().exception_vector(), fw.firmware().exception_srr0(),
                        fw.firmware().exception_srr1());
        else if (fw.client_exited())
            std::printf("\n[probe] the client called exit\n");
        if (fw.powered_off() || fw.reboot_requested())
            std::printf("[probe] RTAS %s\n", fw.powered_off() ? "power-off" : "system-reboot");
        const auto& s = m.cpu().state();
        std::printf("\n[probe] pc=0x%08X lr=0x%08X msr=0x%08X r1=0x%08X r3=0x%08X%s\n", s.pc, s.lr(), s.msr,
                    s.gpr[1], s.gpr[3], m.cpu().checkstopped() ? " CHECKSTOP" : "");
        std::printf("[probe] %llu instructions executed; srr0=0x%08X srr1=0x%08X dar=0x%08X dsisr=0x%08X\n",
                    static_cast<unsigned long long>(m.cpu().instructions_executed()), s.spr[ppc::spr::SRR0],
                    s.spr[ppc::spr::SRR1], s.spr[ppc::spr::DAR], s.spr[ppc::spr::DSISR]);
        for (u32 i = 0; i < 32; i += 8)
            std::printf("[probe] r%-2u %08X %08X %08X %08X %08X %08X %08X %08X\n", i, s.gpr[i], s.gpr[i + 1],
                        s.gpr[i + 2], s.gpr[i + 3], s.gpr[i + 4], s.gpr[i + 5], s.gpr[i + 6], s.gpr[i + 7]);
    }
    if (!via.e.empty()) {
        static const char* const names[16] = {"ORB", "ORA", "DDRB", "DDRA", "T1CL", "T1CH", "T1LL", "T1LH",
                                               "T2CL", "T2CH", "SR", "ACR", "PCR", "IFR", "IER", "ORA_NH"};
        std::printf("[via] %llu accesses; the last:\n", static_cast<unsigned long long>(via.total));
        const u32 n = via.pos < via.e.size() ? via.pos : static_cast<u32>(via.e.size());
        for (u32 i = 0; i < n; ++i) {
            const u32 k = (via.pos - n + i) % via.e.size();
            const u32 x = via.e[k];
            std::printf("[via] %s %-6s %02X  pc=%08X r24=%08X  at %llu\n", (x & 0x80000000u) ? "W" : "R", names[(x >> 8) & 15],
                        x & 0xFF, via.pc[k], via.pc68[k], static_cast<unsigned long long>(via.when[k]));
        }
    }
    if (jitbg_thread.joinable()) {
        jitbg_stop.store(true, std::memory_order_release);
        jitbg_thread.join();
        const auto& js = m.cpu().jit_stats();
        std::printf("[probe] background jit: %llu queued, %llu installed, %llu stale, %llu queue full\n",
                    static_cast<unsigned long long>(js.queued), static_cast<unsigned long long>(js.installed),
                    static_cast<unsigned long long>(js.stale), static_cast<unsigned long long>(js.queue_full));
    }
    // A fingerprint of all of RAM (FNV-1a), for comparing runs.
    {
        u64 h = 0xCBF29CE484222325ull;
        for (u8 b : ram)
            h = (h ^ b) * 0x100000001B3ull;
        std::printf("[probe] ram fnv1a=%016llX\n", static_cast<unsigned long long>(h));
    }
    if (ppc::BlockCache* bc = m.cpu().block_cache()) {
        const auto& st = bc->stats();
        std::printf("[probe] blocks: %llu built, %llu hits, %llu flushes, %llu page invalidations\n",
                    static_cast<unsigned long long>(st.built), static_cast<unsigned long long>(st.hits),
                    static_cast<unsigned long long>(st.flushes), static_cast<unsigned long long>(st.invalidations));
        const auto& js = m.cpu().jit_stats();
        if (m.jit_enabled())
            std::printf("[probe] jit: %llu links, %llu compiled, %llu runs (+ %llu chained), %llu native / %llu handler operations, "
                        "%llu resets, %llu loads/stores on the slow path\n",
                        static_cast<unsigned long long>(js.linked),
                        static_cast<unsigned long long>(js.compiled), static_cast<unsigned long long>(js.runs),
                        static_cast<unsigned long long>(js.chained),
                        static_cast<unsigned long long>(js.native_ops), static_cast<unsigned long long>(js.helper_ops),
                        static_cast<unsigned long long>(js.resets), static_cast<unsigned long long>(js.memory_slow));
        if (m.jit_enabled()) {
            JitReport jr;
            make_jit_report(m.cpu(), jr);
            char line[200];
            format_jit_report(jr, line, sizeof line);
            std::printf("[probe] %s\n", line);
        }
    }
    // Interrupt controller state at the end.
    std::printf("[pic] enabled=%016llX output=%d\n",
                static_cast<unsigned long long>(m.macio().pic().enabled_mask()), m.macio().pic().output() ? 1 : 0);
    // NVRAM partitions at the end.
    for (u32 i = 0;; ++i) {
        NvramPartition p{};
        if (!bw_nvram::partition(m.macio().nvram(), i, p))
            break;
        std::printf("[nvram] %04X sig %02X len %04X \"%s\"%s\n", p.offset, p.signature, p.length, p.name,
                    bw_nvram::valid(m.macio().nvram()) ? "" : " (chain invalid)");
    }
    // G3_SAVE=physaddr,len,file: physical memory at the end, raw, to a file.
    if (const char* sv = std::getenv("G3_SAVE")) {
        char* end = nullptr;
        const u32 at = static_cast<u32>(std::strtoul(sv, &end, 16));
        const u32 len = static_cast<u32>(std::strtoul(end + 1, &end, 16));
        if (FILE* o = std::fopen(end + 1, "wb")) {
            for (u32 i = 0; i < len; ++i) {
                u8 b = 0;
                m.bus().read8(at + i, b);
                std::fputc(b, o);
            }
            std::fclose(o);
        }
    }
    // G3_DUMP=physaddr,len: physical memory at the end, in hex.
    if (const char* dump = std::getenv("G3_DUMP")) {
        char* end = nullptr;
        const u32 at = static_cast<u32>(std::strtoul(dump, &end, 16));
        const u32 len = end && *end == ',' ? static_cast<u32>(std::strtoul(end + 1, nullptr, 16)) : 0x40;
        for (u32 i = 0; i < len; i += 16) {
            std::printf("[dump] %08X:", at + i);
            for (u32 k = 0; k < 16; k += 2) {
                u16 v = 0;
                m.bus().read16(at + i + k, v);
                std::printf(" %04X", v);
            }
            std::printf("\n");
        }
    }
    // G3_VDUMP=addr,len: memory as the CPU's data accesses see it now
    // (current translation), in hex; also follows *addr when len is 0.
    if (const char* vd = std::getenv("G3_VDUMP")) {
        char* end = nullptr;
        const u32 at = static_cast<u32>(std::strtoul(vd, &end, 16));
        const u32 len = end && *end == ',' ? static_cast<u32>(std::strtoul(end + 1, nullptr, 16)) : 0x40;
        ppc::MemoryInterface& mem = fw.client().memory();
        u32 pa = 0;
        const bool mapped = m.cpu().probe_translate(at, false, pa);
        std::printf("[vdump] %08X -> %s%08X\n", at, mapped ? "" : "unmapped ", pa);
        for (u32 i = 0; i < len; i += 16) {
            std::printf("[vdump] %08X:", at + i);
            for (u32 k = 0; k < 16; k += 2) {
                u16 v = 0;
                const bool got = mem.read16(at + i + k, v);  // before printing v
                if (got)
                    std::printf(" %04X", v);
                else
                    std::printf(" ----");
            }
            std::printf("\n");
        }
    }
    // G3_SCREEN=file.ppm: the emulated display at the end (binary PPM).
    if (const char* shot = std::getenv("G3_SCREEN"); shot && m.has_display()) {
        Framebuffer& fb = m.display();
        std::vector<u32> px(static_cast<size_t>(fb.width()) * fb.height());
        fb.convert(px.data(), fb.width());
        if (FILE* o = std::fopen(shot, "wb")) {
            std::fprintf(o, "P6\n%u %u\n255\n", fb.width(), fb.height());
            for (u32 v : px) {
                const u8 rgb[3] = {static_cast<u8>(v >> 16), static_cast<u8>(v >> 8), static_cast<u8>(v)};
                std::fwrite(rgb, 1, 3, o);
            }
            std::fclose(o);
            std::printf("[probe] screen saved to %s\n", shot);
        }
    }
    return ok ? 0 : 1;
}
