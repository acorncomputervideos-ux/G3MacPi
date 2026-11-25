// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/host/main.cpp — g3_host: runs the emulated Mac on Linux from a
// g3macpi.ini, through the same MacSession the Pi uses, and saves a
// screenshot. Headless; for checking start-up without a Pi.
//
//   g3_host <g3macpi.ini> <seconds of guest time> [screen.ppm] [display driver .pef]
//   g3_host --selftest [programs]   the Pi's CPU self-test (generated code on
//                                   the A64 simulator unless on AArch64)
//
// Paths in the .ini are host paths. Progress goes to stdout once per
// second of guest time. G3_STATE=file: the Mac's NVRAM, PRAM and clock
// are read from that file at the start (if valid) and written at the end.
// G3_INPUT=s:dx:dy:b,...: at guest second s, move the mouse by (dx, dy) in
// steps of at most 2 pixels per 10 ms, then set the button to b (0/1).
// G3_SIMLAN=1: the Mac's network is a small simulated one (sim_lan.h): a
// router with DHCP, DNS and a web page. G3_MODEMWAV=file.wav: the dial-up
// modem's speaker (modem = yes), in guest time. G3_WRITEBACK=1: disk
// images the Mac changed are written back to their files at the end.
// G3_VIDEO=file.mp4: a video of the screen, one frame per 40 ms of guest
// time (25 frames per guest second), each frame fitted into 1152x870; needs
// ffmpeg on PATH. With jit = yes in the .ini: the block cache; G3_JITSIM=1
// adds generated code on the A64 simulator (slow), and every 10 guest
// seconds a line of the JIT's profile (platform/common/jit_report.h).

#include "platform/common/dialup_modem.h"
#include "platform/common/cpu_selftest.h"
#include "tools/a64sim/a64_sim.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include "core/machine/bw_firmware.h"
#include "core/machine/yosemite.h"
#include "platform/common/guest_pacer.h"
#include "platform/common/mac_session.h"
#include "platform/common/modem_sound.h"
#include "app/host/sim_lan.h"
#include "platform/common/machine_state.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
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
    std::vector<std::string> paths;  // of data[i]
    std::vector<bool> writable;
    ModemSound speaker;
    SimLan lan;
    ModemSound* modem_sound() override { return std::getenv("G3_MODEMWAV") ? &speaker : nullptr; }
    // G3_WRITEBACK: the writable images back to their files.
    void write_back()
    {
        for (size_t i = 0; i < data.size(); ++i) {
            if (!writable[i])
                continue;
            FILE* f = std::fopen(paths[i].c_str(), "wb");
            const bool ok = f && std::fwrite(data[i]->data(), 1, data[i]->size(), f) == data[i]->size();
            if (f)
                std::fclose(f);
            std::printf("[g3_host] image %s %s\n", ok ? "written back to" : "NOT written back to", paths[i].c_str());
        }
    }
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
        paths.push_back(path);
        writable.push_back(!read_only);
        return devices.back().get();
    }
    void close_image(BlockDevice*) override {}
    void console(const char* text, u32 len) override { std::fwrite(text, 1, len, stdout); }
    void disc_ejected(u32 drive) override { std::printf("[g3_host] disc ejected from drive %u\n", drive); }
    AudioSink* audio_sink() override { return &wav; }
    NetLink* net_link() override { return &net; }

    // G3_WAV=file.wav: the Mac's sound, as it plays (16-bit stereo).
    struct Wav final : AudioSink {
        std::vector<s16> samples;
        u32 rate = 0;
        u64 frames = 0;
        void audio_out(const s16* f, u32 n, u32 r) override
        {
            frames += n;
            rate = r;
            if (std::getenv("G3_WAV"))
                samples.insert(samples.end(), f, f + 2 * n);
        }
        bool write(const char* path) const
        {
            FILE* o = std::fopen(path, "wb");
            if (!o)
                return false;
            const u32 bytes = static_cast<u32>(samples.size() * 2);
            auto le32 = [&](u32 v) { for (int i = 0; i < 4; ++i) std::fputc((v >> (8 * i)) & 0xFF, o); };
            auto le16 = [&](u32 v) { std::fputc(v & 0xFF, o); std::fputc((v >> 8) & 0xFF, o); };
            std::fwrite("RIFF", 1, 4, o); le32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, o);
            le32(16); le16(1); le16(2); le32(rate ? rate : 44100); le32((rate ? rate : 44100) * 4); le16(4); le16(16);
            std::fwrite("data", 1, 4, o); le32(bytes);
            for (s16 v : samples)
                le16(static_cast<u16>(v));
            return std::fclose(o) == 0;
        }
    } wav;
    // The network: frames the Mac sends are logged (G3_NETLOG=1); nothing
    // comes back.
    struct Net final : NetLink {
        u64 sent = 0;
        SimLan* lan = nullptr;
        void net_send(const u8* f, u32 n) override
        {
            ++sent;
            if (lan)
                lan->from_mac(f, n);
            if (std::getenv("G3_NETLOG") && n >= 14)
                std::printf("[g3_host] frame out: %u bytes to %02x:%02x:%02x:%02x:%02x:%02x type %02x%02x\n", n, f[0],
                            f[1], f[2], f[3], f[4], f[5], f[12], f[13]);
        }
        u32 net_receive(u8* buf) override { return lan ? lan->to_mac(buf) : 0; }
    } net;
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

// One video frame: the Mac's screen scaled to fit kVideoW x kVideoH (nearest
// pixel, aspect kept, centred on black), as RGB24.
constexpr u32 kVideoW = 1152, kVideoH = 870;
void video_frame(MacSession& s, std::vector<u8>& out)
{
    out.assign(static_cast<size_t>(kVideoW) * kVideoH * 3, 0);
    const u32 w = s.screen_width(), h = s.screen_height();
    if (!s.screen_on() || w == 0 || h == 0)
        return;
    std::vector<u32> px(static_cast<size_t>(w) * h);
    s.screen(px.data(), w);
    // Largest size that fits, keeping the aspect ratio.
    u32 ow = kVideoW, oh = static_cast<u32>(static_cast<u64>(h) * kVideoW / w);
    if (oh > kVideoH) {
        oh = kVideoH;
        ow = static_cast<u32>(static_cast<u64>(w) * kVideoH / h);
    }
    const u32 x0 = (kVideoW - ow) / 2, y0 = (kVideoH - oh) / 2;
    for (u32 y = 0; y < oh; ++y) {
        const u32* src = px.data() + static_cast<size_t>(y * h / oh) * w;
        u8* d = out.data() + (static_cast<size_t>(y0 + y) * kVideoW + x0) * 3;
        for (u32 x = 0; x < ow; ++x) {
            const u32 p = src[x * w / ow];
            *d++ = static_cast<u8>(p >> 16);
            *d++ = static_cast<u8>(p >> 8);
            *d++ = static_cast<u8>(p);
        }
    }
}

} // namespace

static u64 clock_us(void*)
{
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

static int selftest(int argc, char** argv)
{
    static std::vector<u8> work(CpuSelfTest::kWorkBytes);
    static std::vector<u32> code(128 * 1024);
    static const void* range[2] = {code.data(), code.data() + code.size()};
    CpuSelfTest::Setup s;
    s.work = work.data();
    s.code = code.data();
    s.code_words = static_cast<u32>(code.size());
#if !defined(__aarch64__)
    s.runner = a64::simulate_jit_block;  // the generated code runs on the simulator (slow)
    s.runner_context = range;
#endif
    s.clock_us = clock_us;
    if (argc > 2)
        s.programs = static_cast<u32>(std::strtoul(argv[2], nullptr, 10));
    const CpuSelfTest::Result r = CpuSelfTest::run(s);
    char l1[160], l2[160];
    CpuSelfTest::describe(r, l1, sizeof l1, l2, sizeof l2);
    std::printf("%s\n%s\n", l1, l2);
    return r.ok() ? 0 : 1;
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--selftest") == 0)
        return selftest(argc, argv);
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
    // jit = yes: the block cache. G3_JITSIM=1 adds generated code, run on
    // the A64 simulator unless on AArch64 (slow; for the JIT's profile).
    static std::vector<u8> blocks;
    static std::vector<u32> code;
    static const void* code_range[2] = {};
    if (config.jit) {
        blocks.resize(std::getenv("G3_BLOCKMB") ? std::strtoul(std::getenv("G3_BLOCKMB"), nullptr, 0) << 20 : 64u << 20);  // as on the Pi
        mem.blocks = blocks.data();
        mem.blocks_size = static_cast<u32>(blocks.size());
        if (std::getenv("G3_JITSIM")) {
            code.resize(std::getenv("G3_JITWORDS") ? std::strtoul(std::getenv("G3_JITWORDS"), nullptr, 0) : 4 * 1024 * 1024);  // 16 MB, as on the Pi
            code_range[0] = code.data();
            code_range[1] = code.data() + code.size();
            mem.code = code.data();
            mem.code_words = static_cast<u32>(code.size());
#if !defined(__aarch64__)
            mem.jit_runner = a64::simulate_jit_block;
            mem.jit_runner_context = code_range;
#endif
        }
    }
    HostFiles host;
    MacSession session;
    const char* state_path = std::getenv("G3_STATE");
    static MachineState saved;
    bool have_saved = false;
    if (state_path) {
        std::vector<u8> bytes;
        have_saved = read_file(state_path, bytes) && saved.decode(bytes.data(), static_cast<u32>(bytes.size()));
        std::printf("[g3_host] saved state: %s\n", have_saved ? "restored" : "none");
    }
    if (std::getenv("G3_SIMLAN"))
        host.net.lan = &host.lan;
    // G3_ILLEGAL=1: program exceptions for illegal instructions, with the word.
    struct IllegalLog { MacSession* s; u32 shown; } illegal_log{&session, 0};
    static HostFiles::Wav modem_wav;
    size_t lan_shown = 0;
    const MacSession::Error e = session.start(config, host, mem, driver.empty() ? nullptr : driver.data(),
                                              static_cast<u32>(driver.size()),
                                              static_cast<u64>(std::time(nullptr)), have_saved ? &saved : nullptr);
    if (e != MacSession::Error::None) {
        std::fprintf(stderr, "cannot start: %s%s%s\n", MacSession::error_text(e), session.detail()[0] ? ": " : "",
                     session.detail());
        return 1;
    }
    // G3_MODEMLOG=1: the bytes between the Mac and the virtual modem, a line
    // per direction change ("mac>" what the Mac sent, "<mdm" the modem's).
    if (std::getenv("G3_MODEMLOG") && session.dialup()) {
        struct Log { MacSession* s; int dir; std::string text; double t0; };
        static Log log{&session, -1, {}, 0};
        session.dialup()->set_trace([](void* c, bool to_mac, u32 b) {
            auto* l = static_cast<Log*>(c);
            const double t = static_cast<double>(l->s->machine()->scheduler().now()) / 1e12;
            if (b >= 0x200) {
                static const char* const kEv[] = {"off hook", "digit", "pause", "ringback", "handshake", "connected",
                                                  "on hook"};
                if ((b & 0xFF) != 1 && (b & 0xFF) != 2)
                    std::printf("[modem] %.2f s: event %s\n", t, (b & 0xFF) < 7 ? kEv[b & 0xFF] : "?");
                return;
            }
            if (b >= 0x100) {
                std::printf("[modem] %.2f s: DTR %s\n", t, (b & 1) ? "on" : "off");
                return;
            }
            if (l->dir != (to_mac ? 1 : 0) || b == '\n' || l->text.size() > 60) {
                if (!l->text.empty())
                    std::printf("[modem] %.2f s: %s %s\n", l->t0, l->dir ? "<mdm" : "mac>", l->text.c_str());
                l->text.clear();
                l->dir = to_mac ? 1 : 0;
            }
            char buf[8];
            if (l->text.empty())
                l->t0 = t;  // a line's time is its first byte's
            if (b >= 32 && b < 127)
                l->text.push_back(static_cast<char>(b));
            else {
                std::snprintf(buf, sizeof buf, "<%02x>", b);
                l->text += buf;
            }
        }, &log);
    }
    if (std::getenv("G3_ILLEGAL"))
        session.cpu()->set_exception_hook([](void* c, u32 vector, u32 srr0, u32 srr1) {
            auto* l = static_cast<IllegalLog*>(c);
            if (vector != 0x700 || !(srr1 & ppc::srr1::ILLEGAL) || l->shown >= 40)
                return;
            ++l->shown;
            u32 pa = 0, word = 0;
            if (l->s->cpu()->probe_translate(srr0, true, pa))
                l->s->machine()->bus().read32(pa, word);
            const auto& st = l->s->cpu()->state();
            u32 lpa = 0, before = 0;
            if (l->s->cpu()->probe_translate(st.lr() - 4, true, lpa))
                l->s->machine()->bus().read32(lpa, before);
            std::printf("[illegal] pc %08x (pa %08x): %08x  lr %08x (insn before it %08x, pa %08x) ctr %08x r1 %08x "
                        "msr %08x\n", srr0, pa, word, st.lr(), before, lpa, st.ctr(), st.gpr[1], srr1);
        }, &illegal_log);
    std::printf("[g3_host] started: %u MB, %ux%u\n", config.ram_mb, config.width, config.height);
    // G3_JITHIST=1 (with G3_JITSIM): where the generated code's instructions
    // go, by what they are for (Ppc750::JitWordKind), printed at the end.
    static std::vector<u8> word_kinds;
    static u64 kind_counts[256] = {};
    if (std::getenv("G3_JITHIST") && mem.jit_runner && session.cpu()) {
        word_kinds.assign(code.size(), 0);
        session.cpu()->set_jit_word_kinds(word_kinds.data());
        a64::set_simulator_histogram(word_kinds.data(), kind_counts);
    }
    // G3_MHZ=n: the effective CPU clock (guest time per instruction), as the
    // Pi's pacer sets it when the host is slower than a real G3.
    if (const char* mhz = std::getenv("G3_MHZ"))
        session.set_effective_hz(std::strtoull(mhz, nullptr, 10) * 1000000ull);
    // G3_XPRAM=offset:value[,...]: poke Mac OS's PRAM (the NVRAM partition
    // "APL,MacOS75") before it boots (experiments).
    if (const char* xp = std::getenv("G3_XPRAM")) {
        MacioNvram& nvr = session.machine()->macio().nvram();
        std::vector<u8> nv(nvr.data(), nvr.data() + MacioNvram::kSize);
        for (u32 off = 0; off + 16 <= MacioNvram::kSize;) {
            const u32 len = ((nv[off + 2] << 8) | nv[off + 3]) * 16;
            if (len < 16)
                break;
            if (std::memcmp(&nv[off + 4], "APL,MacOS75", 11) == 0) {
                const char* p = xp;
                while (*p) {
                    char* end = nullptr;
                    const u32 o = static_cast<u32>(std::strtoul(p, &end, 0));
                    const u32 v = static_cast<u32>(std::strtoul(end + 1, &end, 0));
                    nv[off + 16 + o] = static_cast<u8>(v);
                    std::printf("[g3_host] PRAM %02x = %02x\n", o, v);
                    p = *end == ',' ? end + 1 : end;
                }
            }
            off += len;
        }
        nvr.load(nv.data(), MacioNvram::kSize);
    }
    // G3_MMIO_RANGE=lo,hi[,max]: device-register accesses in [lo, hi).
    // G3_MMIO_FROM=s: only from guest second s; G3_MMIO_SKIP=lo,hi: not
    // those in [lo, hi).
    if (const char* r = std::getenv("G3_MMIO_RANGE")) {
        struct Range { u32 lo, hi, max, shown, last_addr, last_value; bool last_read; u64 repeats; PhysicalBus* bus; ppc::Ppc750* cpu; Scheduler* sched; u64 from_ps; u32 skip_lo, skip_hi; };
        static Range range{};
        range.bus = &session.machine()->bus();
        range.cpu = &session.machine()->cpu();
        range.sched = &session.machine()->scheduler();
        if (const char* f = std::getenv("G3_MMIO_FROM"))
            range.from_ps = std::strtoull(f, nullptr, 10) * 1000000000000ull;
        if (const char* k = std::getenv("G3_MMIO_SKIP")) {
            char* e2 = nullptr;
            range.skip_lo = static_cast<u32>(std::strtoul(k, &e2, 0));
            range.skip_hi = e2 && *e2 == ',' ? static_cast<u32>(std::strtoul(e2 + 1, nullptr, 0)) : 0;
        }
        char* end = nullptr;
        range.lo = static_cast<u32>(std::strtoul(r, &end, 0));
        range.hi = end && *end == ',' ? static_cast<u32>(std::strtoul(end + 1, &end, 0)) : ~0u;
        range.max = end && *end == ',' ? static_cast<u32>(std::strtoul(end + 1, nullptr, 0)) : 2000;
        session.machine()->bus().set_mmio_trace([](void* c, u32 addr, u32 size, bool write, u32 value) {
            auto* g = static_cast<Range*>(c);
            if (addr < g->lo || addr >= g->hi || g->shown >= g->max || (addr >= g->skip_lo && addr < g->skip_hi) ||
                g->sched->now() < g->from_ps)
                return;
            if (!write && g->last_read && addr == g->last_addr && value == g->last_value) {
                ++g->repeats;  // a polling loop: counted, not printed
                return;
            }
            if (g->repeats)
                std::printf("[mmio]   (read %llu more times)\n", static_cast<unsigned long long>(g->repeats));
            g->repeats = 0;
            g->last_addr = addr;
            g->last_value = value;
            g->last_read = !write;
            ++g->shown;
            u32 pc_pa = 0;
            const u32 pc = g->cpu->state().pc;
            g->cpu->probe_translate(pc, false, pc_pa);
            std::printf("[mmio] %.3f %s%u %08x = %08x  pc %08x (pa %08x)\n",
                        static_cast<double>(g->sched->now()) / 1e12, write ? "W" : "R", size, addr, value, pc, pc_pa);
            // A DBDMA command pointer written: show the descriptors there.
            if (write && size == 4 && (addr & 0xFFFF8000u) == 0x80808000u && (addr & 0x7F) == 0x0C) {
                const u32 cp = __builtin_bswap32(value);
                for (u32 d = 0; d < 64; ++d) {
                    u32 w[4];
                    for (u32 k = 0; k < 4; ++k) {
                        g->bus->read32(cp + d * 16 + k * 4, w[k]);
                        w[k] = __builtin_bswap32(w[k]);
                    }
                    std::printf("[mmio]     desc %08x: req %u cmd %04x addr %08x dep %08x res %u st %04x\n",
                                cp + d * 16, w[0] & 0xFFFF, w[0] >> 16, w[1], w[2], w[3] & 0xFFFF, w[3] >> 16);
                    if ((w[0] >> 28) == 7 || ((w[0] >> 16) & 0x000C) == 0x000C)
                        break;  // STOP, or a branch always
                }
            }
        }, &range);
    }

    struct InputStep { u32 at; s32 dx, dy; u32 button; };
    std::vector<InputStep> steps;
    if (const char* in = std::getenv("G3_INPUT")) {
        const char* p = in;
        while (*p) {
            InputStep st{};
            char* end = nullptr;
            st.at = static_cast<u32>(std::strtoul(p, &end, 10));
            st.dx = static_cast<s32>(std::strtol(end + 1, &end, 10));
            st.dy = static_cast<s32>(std::strtol(end + 1, &end, 10));
            st.button = static_cast<u32>(std::strtoul(end + 1, &end, 10));
            steps.push_back(st);
            if (end == p)
                break;  // malformed
            p = *end == ',' ? end + 1 : end;
        }
    }
    // G3_KEYS=sec:usage[+usage...],...: at guest second sec, press the HID
    // usages together (e.g. 0xE3 = left Command) and release them.
    struct KeyStep { u32 at; std::vector<u8> keys; };
    std::vector<KeyStep> key_steps;
    if (const char* ks = std::getenv("G3_KEYS")) {
        const char* p = ks;
        while (*p) {
            KeyStep st{};
            char* end = nullptr;
            st.at = static_cast<u32>(std::strtoul(p, &end, 10));
            p = end;
            while (*p == ':' || *p == '+') {
                st.keys.push_back(static_cast<u8>(std::strtoul(p + 1, &end, 0)));
                p = end;
            }
            key_steps.push_back(st);
            if (*p != ',')
                break;
            ++p;
        }
    }
    size_t next_key = 0;
    // G3_LANTRACE=sec:codefile: from guest second sec, find the code in
    // codefile (a PEF code section, e.g. a driver taken from the ROM file)
    // in RAM and log its calls through import glue (lwz r12,d(r2) ...
    // bctr): r3-r6, a text argument and the result. Experiments.
    struct LanTrace {
        u32 start = 0;
        std::vector<u8> code;
        u32 base = 0;  // physical, 0 = not found yet
        std::vector<u32> bases;  // every copy found
        std::vector<bool> stub;
        ppc::Ppc750* cpu = nullptr;
        u8* ram = nullptr;
        u32 ram_size = 0;
        u32 pending_lr_pa = 0, pending_stub = 0, pending_buf = 0;
        u32 shown = 0;
        std::string text(u32 ea) const
        {
            u32 pa;
            std::string t;
            for (u32 i = 0; i < 40; ++i) {
                if (!cpu->probe_translate(ea + i, false, pa) || pa >= ram_size)
                    return "";
                const u8 c = ram[pa];
                if (c == 0)
                    break;
                if (c < 32 || c > 126)
                    return "";
                t += static_cast<char>(c);
            }
            return t.size() >= 3 ? t : "";
        }
    };
    static LanTrace lan;
    bool lan_armed = false;
    if (const char* lt = std::getenv("G3_LANTRACE")) {
        char* end = nullptr;
        lan.start = static_cast<u32>(std::strtoul(lt, &end, 10));
        if (end && *end == ':')
            read_file(end + 1, lan.code);
        lan.stub.assign(lan.code.size() / 4, false);
        for (size_t k = 0; k + 4 <= lan.code.size(); k += 4) {
            const u32 w = (lan.code[k] << 24) | (lan.code[k + 1] << 16) | (lan.code[k + 2] << 8) | lan.code[k + 3];
            if ((w & 0xFFFF0000u) == 0x81820000u && (w & 0x8000))  // lwz r12, -d(r2)
                lan.stub[k / 4] = true;
        }
        lan.cpu = &session.machine()->cpu();
        lan.ram = ram.data();
        lan.ram_size = static_cast<u32>(ram.size());
    }
    size_t next_step = 0;
    s32 move_x = 0, move_y = 0;
    u32 button = 0;
    FILE* video = nullptr;
    std::vector<u8> frame;
    if (const char* vp = std::getenv("G3_VIDEO")) {
        const std::string cmd = std::string("ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s ") +
                                std::to_string(kVideoW) + "x" + std::to_string(kVideoH) +
                                " -r 25 -i - -c:v libx264 -pix_fmt yuv420p -crf 20 '" + vp + "'";
        video = popen(cmd.c_str(), "w");
        if (!video)
            std::fprintf(stderr, "cannot start ffmpeg\n");
    }
    u32 frames = 0;
    const u32 seconds = static_cast<u32>(std::strtoul(argv[2], nullptr, 10));
    // G3_REALTIME=1: <seconds> of real time, guest time paced to it
    // (platform/common/guest_pacer.h), as the Pi runs the Mac.
    if (std::getenv("G3_REALTIME")) {
        using clk = std::chrono::steady_clock;
        const auto t0 = clk::now();
        auto host_ps = [&] {
            return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count()) * 1000u;
        };
        GuestPacer pacer;
        pacer.start(host_ps(), session.guest_time_ps(), session.nominal_hz());
        u64 next_report = 1000 * GuestPacer::kMs, last_instr = 0;
        while (host_ps() < static_cast<u64>(seconds) * 1000 * GuestPacer::kMs && !session.halted()) {
            const u64 now = host_ps();
            const u64 run = pacer.plan(now, session.guest_time_ps());
            if (run == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            } else {
                const u64 i0 = session.instructions();
                session.run(run);
                const u64 hz = pacer.ran(run, session.instructions() - i0, host_ps() - now);
                if (hz)
                    session.set_effective_hz(hz);
            }
            if (host_ps() >= next_report) {
                const u64 instr = session.instructions();
                std::printf("[g3_host] real %.0f s, guest %.2f s, effective clock %llu MHz, %.1f MIPS, dropped %.2f s\n",
                            next_report / 1e12, session.guest_time_ps() / 1e12,
                            static_cast<unsigned long long>(session.effective_hz() / 1000000),
                            (instr - last_instr) / 1e6, pacer.dropped_ps() / 1e12);
                std::fflush(stdout);
                last_instr = instr;
                next_report += 1000 * GuestPacer::kMs;
            }
        }
        if (argc > 3 && write_ppm(argv[3], session))
            std::printf("[g3_host] screen saved to %s\n", argv[3]);
        return 0;
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (u32 s = 0; s < seconds && !session.halted() && !session.powered_off(); ++s) {
        for (u32 slice = 0; slice < 100; ++slice) {
            if (move_x == 0 && move_y == 0 && next_step < steps.size() && steps[next_step].at <= s) {
                move_x = steps[next_step].dx;
                move_y = steps[next_step].dy;
                button = steps[next_step].button;
                ++next_step;
                if (move_x == 0 && move_y == 0)
                    session.mouse(0, 0, button);
            }
            if (move_x || move_y) {
                const s32 dx = move_x > 2 ? 2 : (move_x < -2 ? -2 : move_x);
                const s32 dy = move_y > 2 ? 2 : (move_y < -2 ? -2 : move_y);
                move_x -= dx;
                move_y -= dy;
                session.mouse(dx, dy, button);
            }
            if (slice == 0 && next_key < key_steps.size() && key_steps[next_key].at <= s) {
                const KeyStep& k = key_steps[next_key++];
                for (u8 u : k.keys)
                    session.key(u, true);
                session.run(50000000000ull);  // held for 50 ms
                for (size_t i = k.keys.size(); i-- > 0;)
                    session.key(k.keys[i], false);
                std::printf("[g3_host] keys pressed at %u s\n", s);
            }
            if (!lan.code.empty() && !lan_armed && s >= lan.start && slice == 0) {
                lan_armed = true;
                lan.cpu->set_step_hook([](void* c, const ppc::CpuState& st) {
                    auto* l = static_cast<LanTrace*>(c);
                    // Look for copies of the code now and then (one may be
                    // loaded later).
                    static u32 tries = 0;
                    if ((++tries & 0x3FFFFF) == 1) {
                        for (u32 a = 0; a + l->code.size() <= l->ram_size; a += 4)
                            if (std::memcmp(l->ram + a, l->code.data(), 64) == 0 &&
                                std::find(l->bases.begin(), l->bases.end(), a) == l->bases.end()) {
                                l->bases.push_back(a);
                                std::printf("[lan] code found at physical %#x\n", a);
                            }
                    }
                    u32 pa;
                    if (!l->cpu->probe_translate(st.pc, false, pa))
                        return;
                    for (u32 b : l->bases)
                        if (pa >= b && pa < b + l->code.size())
                            l->base = b;
                    if (l->pending_lr_pa && pa == l->pending_lr_pa) {
                        if (l->shown < 4000)
                            std::printf("[lan]   -> r3 = %#x (%d)\n", st.gpr[3], static_cast<s32>(st.gpr[3]));
                        if (l->pending_buf) {  // a property was read: its value
                            std::printf("[lan]      value:");
                            for (u32 i = 0; i < 8; ++i) {
                                u32 bpa;
                                if (!l->cpu->probe_translate(l->pending_buf + 4 * i, false, bpa) || bpa + 4 > l->ram_size)
                                    break;
                                std::printf(" %08x", load_be32(l->ram + bpa));
                            }
                            std::printf("\n");
                            l->pending_buf = 0;
                        }
                        l->pending_lr_pa = 0;
                    }
                    if (pa < l->base || pa >= l->base + l->code.size())
                        return;
                    const u32 off = pa - l->base;
                    static bool entered = false;
                    if (!entered) {
                        entered = true;
                        std::printf("[lan] first instruction run: code+%#x (pc %#x)\n", off, st.pc);
                    }
                    if (!l->stub[off / 4] || l->shown >= 4000 || off == 0x3994 || off == 0x379c || off == 0x3a24 || off == 0x3a3c)
                        return;
                    ++l->shown;
                    u32 lr_pa = 0;
                    l->cpu->probe_translate(st.lr(), false, lr_pa);
                    std::string args;
                    for (u32 r = 3; r <= 6; ++r) {
                        const std::string t = l->text(st.gpr[r]);
                        if (!t.empty())
                            args += " r" + std::to_string(r) + "=\"" + t + "\"";
                    }
                    std::printf("[lan] stub %#x from %#x: r3=%#x r4=%#x r5=%#x r6=%#x%s\n", off,
                                lr_pa - l->base, st.gpr[3], st.gpr[4], st.gpr[5], st.gpr[6], args.c_str());
                    l->pending_lr_pa = lr_pa;
                    l->pending_buf = !l->text(st.gpr[4]).empty() ? st.gpr[5] : 0;
                }, &lan);
            }
            session.run(10000000000ull);  // 10 ms of guest time
            if (std::getenv("G3_MODEMWAV")) {
                static std::vector<s16> chunk(2 * 441);
                std::fill(chunk.begin(), chunk.end(), 0);
                host.speaker.render_add(chunk.data(), 441, 44100);
                modem_wav.samples.insert(modem_wav.samples.end(), chunk.begin(), chunk.end());
                modem_wav.rate = 44100;
            }
            while (lan_shown < host.lan.log.size())
                std::printf("[sim-lan] %.2f s: %s\n", session.guest_time_ps() / 1e12, host.lan.log[lan_shown++].c_str());
            if (video && slice % 4 == 3) {     // every 40 ms
                video_frame(session, frame);
                std::fwrite(frame.data(), 1, frame.size(), video);
                ++frames;
            }
        }
        const double real = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        static double last_real = 0;
        static u64 last_instr = 0;
        const u64 instr = session.instructions();
        const double mips = real > last_real ? (instr - last_instr) / (real - last_real) / 1e6 : 0;
        last_real = real;
        last_instr = instr;
        std::printf("[g3_host] %u s guest, %.1f s real, screen %s, %.1f MIPS\n", s + 1, real,
                    session.screen_on() ? "on" : "off", mips);
        char jit_line[200];
        if ((s + 1) % 10 == 0 && session.jit_report(jit_line, sizeof jit_line))
            std::printf("[g3_host] %s\n", jit_line);
        std::fflush(stdout);
    }
#if !defined(__aarch64__)
    if (mem.jit_runner) {
        char jit_line[200];
        session.jit_report(jit_line, sizeof jit_line);
        std::printf("[g3_host] %s\n[g3_host] generated code: %llu AArch64 instructions executed (simulator) "
                    "for %llu guest instructions\n", jit_line,
                    static_cast<unsigned long long>(a64::simulated_instructions()),
                    static_cast<unsigned long long>(session.instructions()));
        if (!word_kinds.empty()) {
            static const char* names[] = {"untagged", "block setup", "native ops", "memory fast path",
                                          "memory slow path", "interpreter calls", "final branch",
                                          "pc/count sync, write-back", "block end (flags, known targets)",
                                          "stub: known target", "stub: entry", "stub: dispatch",
                                          "stub: enter", "stub: epilogue", "-> dispatch: blr (returns)",
                                          "-> dispatch: bctr (computed)", "-> dispatch: b/bc to another page",
                                          "-> dispatch: other block ends"};
            u64 total = 0;
            for (u64 c : kind_counts)
                total += c;
            for (u32 k = 0; k < ppc::Ppc750::kWordKinds; ++k)
                if (kind_counts[k])
                    std::printf(k >= ppc::Ppc750::kWordToDispatchReturn ? "[g3_host]   %-34s %6.3f per guest instruction (%4.1f%%)  (= hops)\n" : "[g3_host]   %-34s %6.2f per guest instruction (%4.1f%%)\n", names[k],
                                double(kind_counts[k]) / double(session.instructions()),
                                100.0 * double(kind_counts[k]) / double(total));
        }
        if (const auto* js = session.jit_stats())
            std::printf("[g3_host] blocks: %llu entered from run_blocks, %llu chained, %llu instructions run without code; %llu blocks compiled, %llu code buffer resets, %llu words\n",
                        static_cast<unsigned long long>(js->runs), static_cast<unsigned long long>(js->chained),
                        static_cast<unsigned long long>(js->interpreted), static_cast<unsigned long long>(js->compiled),
                        static_cast<unsigned long long>(js->resets), static_cast<unsigned long long>(js->code_words));
    }
#endif
    if (video) {
        const int rc = pclose(video);
        std::printf("[g3_host] video: %u frames, %s\n", frames, rc == 0 ? "written" : "ffmpeg FAILED");
    }
    if (session.halted())
        std::printf("[g3_host] the CPU halted (checkstop)\n");
    if (const char* rd = std::getenv("G3_RAMDUMP")) {  // the Mac's RAM at the end (experiments)
        FILE* f = std::fopen(rd, "wb");
        if (f) {
            std::fwrite(ram.data(), 1, ram.size(), f);
            std::fclose(f);
            std::printf("[g3_host] RAM written to %s\n", rd);
        }
    }
    // G3_TREEDUMP=1: the device tree's mac-io nodes and their properties after
    // the run (Mac OS's ROM file adds driver properties to the nodes it knows).
    if (std::getenv("G3_TREEDUMP") && session.firmware()) {
        of::DeviceTree& t = session.firmware()->tree();
        const of::BwNodes& n = session.firmware()->nodes();
        const of::Phandle list[] = {n.macio, n.ata[0], n.scsi, n.davbus, n.sound, n.ethernet, n.escc_a, n.cuda};
        for (of::Phandle h : list) {
            if (h == of::kNone)
                continue;
            std::printf("[tree] node %s:", t.name(h));
            for (const char* p = t.next_property(h, nullptr); p; p = t.next_property(h, p)) {
                const u8* v = nullptr;
                u32 len = 0;
                t.get_property(h, p, v, len);
                std::printf(" %s(%u)", p, len);
            }
            std::printf("\n");
        }
    }
    {
        MacioDavbus& snd = session.machine()->macio().sound();
        MacioBmac& eth = session.machine()->macio().ethernet();
        std::printf("[g3_host] sound: %llu frames at %u Hz; codec regs 0-7:", static_cast<unsigned long long>(host.wav.frames),
                    host.wav.rate);
        for (u32 i = 0; i < 8; ++i)
            std::printf(" %03x", snd.codec_reg(i));
        std::printf("; control %08x, codec control %08x", snd.mmio_read(0, 4), snd.mmio_read(0x10, 4));
        const u8* pram = session.machine()->macio().cuda().pram();
        {
            // NVRAM partitions (16-byte headers: signature, checksum, length
            // in 16-byte units, name) and each one's first bytes.
            const u8* nv = session.machine()->macio().nvram().data();
            for (u32 off = 0; off + 16 <= 8192;) {
                const u32 len = ((nv[off + 2] << 8) | nv[off + 3]) * 16;
                if (len < 16)
                    break;
                char name[13] = {};
                std::memcpy(name, nv + off + 4, 12);
                std::printf("\n[g3_host] NVRAM %04x: sig %02x len %u '%s':", off, nv[off], len, name);
                u32 nonzero = 0;
                for (u32 i = 16; i < len; ++i)
                    if (nv[off + i] && nonzero++ < 24)
                        std::printf(" [%x]=%02x", i - 16, nv[off + i]);
                std::printf(" (%u non-zero)", nonzero);
                off += len;
            }
        }
        std::printf("\n[g3_host] PRAM 0x00-0x1F:");
        for (u32 i = 0; i < 0x20; ++i)
            std::printf(" %02x", pram[i]);
        u8 mac[6];
        eth.station_address(mac);
        std::printf("\n[g3_host] ethernet: %llu frames sent, %llu received, %llu dropped; station %02x:%02x:%02x:%02x:%02x:%02x\n",
                    static_cast<unsigned long long>(eth.frames_sent()), static_cast<unsigned long long>(eth.frames_received()),
                    static_cast<unsigned long long>(eth.frames_dropped()), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        if (const char* wp = std::getenv("G3_WAV"))
            std::printf("[g3_host] sound %s %s\n", host.wav.write(wp) ? "written to" : "NOT written to", wp);
    }
    if (const char* mw = std::getenv("G3_MODEMWAV"))
        std::printf("[g3_host] modem sound %s %s\n", modem_wav.write(mw) ? "written to" : "NOT written to", mw);
    if (state_path) {
        static MachineState now;
        std::vector<u8> bytes(MachineState::kFileSize);
        bool ok = session.capture_state(now);
        if (ok) {
            now.encode(bytes.data());
            FILE* f = std::fopen(state_path, "wb");
            ok = f && std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
            if (f)
                ok = std::fclose(f) == 0 && ok;
        }
        std::printf("[g3_host] state %s %s\n", ok ? "saved to" : "NOT saved to", state_path);
    }
    if (argc > 3) {
        if (write_ppm(argv[3], session))
            std::printf("[g3_host] screen saved to %s\n", argv[3]);
        else
            std::printf("[g3_host] no screen to save\n");
    }
    if (std::getenv("G3_WRITEBACK"))
        host.write_back();  // (the images are in memory; written as they are now)
    return 0;
}
