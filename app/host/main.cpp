// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/host/main.cpp — g3_host: runs the emulated Mac on Linux from a
// g3macpi.ini, through the same MacSession the Pi uses, and saves a
// screenshot. Headless; for checking start-up without a Pi.
//
//   g3_host <g3macpi.ini> <seconds of guest time> [screen.ppm] [display driver .pef]
//
// Paths in the .ini are host paths. Progress goes to stdout once per
// second of guest time.

#include "platform/common/mac_session.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

using namespace g3;

namespace {

bool read_file(const char* path, std::vector<u8>& out)
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

// Images are read into memory (fine for a test runner); writes stay there.
struct HostFiles final : MacHost {
    std::vector<u8> rom;
    std::vector<std::unique_ptr<std::vector<u8>>> data;
    std::vector<std::unique_ptr<RamBlockDevice>> devices;
    const u8* load_file(const char* path, u32& size) override
    {
        if (!read_file(path, rom))
            return nullptr;
        size = static_cast<u32>(rom.size());
        return rom.data();
    }
    BlockDevice* open_image(const char* path, bool read_only) override
    {
        auto bytes = std::make_unique<std::vector<u8>>();
        if (!read_file(path, *bytes))
            return nullptr;
        devices.push_back(std::make_unique<RamBlockDevice>(bytes->data(), bytes->size(), read_only));
        data.push_back(std::move(bytes));
        return devices.back().get();
    }
    void close_image(BlockDevice*) override {}
    void console(const char* text, u32 len) override { std::fwrite(text, 1, len, stdout); }
    void disc_ejected(u32 drive) override { std::printf("[g3_host] disc ejected from drive %u\n", drive); }
};

bool write_ppm(const char* path, MacSession& s)
{
    const u32 w = s.screen_width(), h = s.screen_height();
    if (w == 0 || h == 0)
        return false;
    std::vector<u32> px(static_cast<size_t>(w) * h);
    s.screen(px.data(), w);
    FILE* f = std::fopen(path, "wb");
    if (!f)
        return false;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (u32 p : px) {
        const u8 rgb[3] = {static_cast<u8>(p >> 16), static_cast<u8>(p >> 8), static_cast<u8>(p)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: g3_host <g3macpi.ini> <seconds> [screen.ppm] [driver.pef]\n");
        return 2;
    }
    std::vector<u8> ini;
    if (!read_file(argv[1], ini)) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    MachineConfig config;
    ConfigParseReport report;
    parse_machine_config(reinterpret_cast<const char*>(ini.data()), static_cast<u32>(ini.size()), config, report);
    if (report.ignored)
        std::printf("[g3_host] %u line(s) ignored; first: line %u (%s)\n", report.ignored, report.first_line,
                    report.first_reason);
    std::vector<u8> driver;
    if (argc > 4 && !read_file(argv[4], driver)) {
        std::fprintf(stderr, "cannot read %s\n", argv[4]);
        return 1;
    }

    std::vector<u8> ram(static_cast<size_t>(config.ram_mb) << 20), vram(8u << 20);
    MacSession::Memory mem;
    mem.ram = ram.data();
    mem.ram_size = static_cast<u32>(ram.size());
    mem.vram = vram.data();
    mem.vram_size = static_cast<u32>(vram.size());
    HostFiles host;
    MacSession session;
    const MacSession::Error e = session.start(config, host, mem, driver.empty() ? nullptr : driver.data(),
                                              static_cast<u32>(driver.size()),
                                              static_cast<u64>(std::time(nullptr)));
    if (e != MacSession::Error::None) {
        std::fprintf(stderr, "cannot start: %s%s%s\n", MacSession::error_text(e), session.detail()[0] ? ": " : "",
                     session.detail());
        return 1;
    }
    std::printf("[g3_host] started: %u MB, %ux%u\n", config.ram_mb, config.width, config.height);

    const u32 seconds = static_cast<u32>(std::strtoul(argv[2], nullptr, 10));
    const auto t0 = std::chrono::steady_clock::now();
    for (u32 s = 0; s < seconds && !session.halted() && !session.powered_off(); ++s) {
        for (u32 slice = 0; slice < 100; ++slice)
            session.run(10000000000ull);  // 10 ms of guest time
        const double real = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("[g3_host] %u s guest, %.1f s real, screen %s\n", s + 1, real, session.screen_on() ? "on" : "off");
        std::fflush(stdout);
    }
    if (session.halted())
        std::printf("[g3_host] the CPU halted (checkstop)\n");
    if (argc > 3) {
        if (write_ppm(argv[3], session))
            std::printf("[g3_host] screen saved to %s\n", argv[3]);
        else
            std::printf("[g3_host] no screen to save\n");
    }
    return 0;
}
