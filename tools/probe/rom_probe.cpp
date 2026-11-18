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

#include "core/cpu/ppc750/disasm.h"
#include "core/machine/bw_firmware.h"
#include "core/machine/bw_nvram.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace g3;

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

    static std::vector<u8> ram(256u << 20), vram(8u << 20);
    YosemiteConfig cfg;
    cfg.ram = ram.data();
    cfg.ram_size = static_cast<u32>(ram.size());
    cfg.vram = vram.data();
    cfg.vram_size = static_cast<u32>(vram.size());
    YosemiteMachine m(cfg);
    BwFirmware::Config fc;
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
    const bool ok = fw.command("init-program");
    std::printf("\n[probe] init-program: %s\n", ok ? "completed" : "FAILED");
    if (!ok)
        std::printf("[probe] Forth error: %s\n", fw.forth().error_text());
    if (fw.client_started()) {
        std::printf("[probe] client started at 0x%08X; running %llu ms of guest time\n", fw.client_entry(),
                    static_cast<unsigned long long>(ms));
        if (std::getenv("G3_STEP")) {
            // Step by step (no device time passes), reporting each exception
            // entry: the vector, SRR0/SRR1, DAR/DSISR, and the last PCs.
            const u64 steps = ms * 350000ull;  // ~ms at 350 MHz, one instruction per cycle
            u32 history[16] = {}, hp = 0, reported = 0;
            auto& st = m.cpu().state();
            for (u64 i = 0; i < steps && !m.cpu().checkstopped() && !fw.client_exited(); ++i) {
                const u32 before = st.pc;
                history[hp++ % 16] = before;
                m.cpu().step();
                const u32 after = st.pc;
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
        } else if (const char* every = std::getenv("G3_SAMPLE_MS")) {
            // Run in slices, printing where the CPU is after each one.
            const u64 slice = std::strtoull(every, nullptr, 10);
            for (u64 t = 0; slice && t < ms && !fw.client_exited(); t += slice) {
                m.run_for(slice * 1000000000ull);
                const auto& st = m.cpu().state();
                std::printf("[sample] %6llu ms  pc=%08X lr=%08X msr=%08X r1=%08X r3=%08X r24=%08X r25=%08X  %llu insns\n",
                            static_cast<unsigned long long>(t + slice), st.pc, st.lr(), st.msr, st.gpr[1], st.gpr[3],
                            st.gpr[24], st.gpr[25], static_cast<unsigned long long>(m.cpu().instructions_executed()));
                std::fflush(stdout);
            }
        } else {
            m.run_for(ms * 1000000000ull);  // picoseconds
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
                std::printf(mem.read16(at + i + k, v) ? " %04X" : " ----", v);
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
