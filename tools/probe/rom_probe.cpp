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

#include "core/machine/bw_firmware.h"

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
        m.run_for(ms * 1000000ull);
        if (fw.client_exited())
            std::printf("\n[probe] the client called exit\n");
        const auto& s = m.cpu().state();
        std::printf("\n[probe] pc=0x%08X lr=0x%08X msr=0x%08X r1=0x%08X r3=0x%08X%s\n", s.pc, s.lr(), s.msr,
                    s.gpr[1], s.gpr[3], m.cpu().checkstopped() ? " CHECKSTOP" : "");
    }
    return ok ? 0 : 1;
}
