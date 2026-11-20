// SPDX-License-Identifier: GPL-3.0-or-later
// Test helper: a tiny PowerPC client program (an ELF file) that prints a
// line through the Open Firmware client interface. Used by the boot tests.

#ifndef G3_TESTS_UNIT_MACHINE_TEST_CLIENT_H
#define G3_TESTS_UNIT_MACHINE_TEST_CLIENT_H

#include "core/common/types.h"
#include "core/cpu/ppc750/cpu_state.h"
#include "tests/unit/cpu/ppc_asm.h"

#include <cstring>
#include <string>
#include <vector>

namespace g3 {
namespace test_client {

// The client program: code at 0x6000, data at 0x7000 (absolute addressing
// with ra = 0 reaches both). It calls finddevice("/chosen"),
// getprop(chosen, "stdout", buf, 4) and write(stdout, msg, len).
struct Client {
    static constexpr u32 kCode = 0x6000, kData = 0x7000;
    static constexpr u32 kArgs1 = kData + 0x000, kArgs2 = kData + 0x040, kArgs3 = kData + 0x080;
    static constexpr u32 kBuf = kData + 0x0C0, kStrings = kData + 0x100;
    std::vector<u8> data = std::vector<u8>(0x200);
    std::vector<u32> code;
    std::string msg = "Hello from a client program\n";

    void w(u32 addr, u32 v)
    {
        const u32 o = addr - kData;
        for (u32 k = 0; k < 4; ++k) data[o + k] = static_cast<u8>(v >> (24 - 8 * k));
    }
    u32 s(u32& at, const std::string& str)
    {
        const u32 a = at;
        std::memcpy(&data[a - kData], str.c_str(), str.size() + 1);
        at = (at + static_cast<u32>(str.size()) + 4) & ~3u;
        return a;
    }
    Client()
    {
        using namespace asm_ppc;
        u32 at = kStrings;
        const u32 finddevice = s(at, "finddevice"), chosen = s(at, "/chosen"), getprop = s(at, "getprop"),
                  stdout_name = s(at, "stdout"), write = s(at, "write"), m = s(at, msg);
        w(kArgs1, finddevice); w(kArgs1 + 4, 1); w(kArgs1 + 8, 1); w(kArgs1 + 12, chosen);
        w(kArgs2, getprop); w(kArgs2 + 4, 4); w(kArgs2 + 8, 1);
        w(kArgs2 + 16, stdout_name); w(kArgs2 + 20, kBuf); w(kArgs2 + 24, 4);
        w(kArgs3, write); w(kArgs3 + 4, 3); w(kArgs3 + 8, 1);
        w(kArgs3 + 16, m); w(kArgs3 + 20, static_cast<u32>(msg.size()));
        const u32 mr31_r5 = X(31, 5, 31, 5, 444);  // or r31,r5,r5 (mr r31,r5)
        code = {
            mr31_r5,
            stw(3, 0x7180, 0),                         // r3 on entry (must be 0)
            mtspr(ppc::spr::CTR, 31), li(3, static_cast<s16>(kArgs1)), bctr() | 1,
            lwz(4, kArgs1 + 16, 0), stw(4, kArgs2 + 12, 0),     // phandle of /chosen
            mtspr(ppc::spr::CTR, 31), li(3, static_cast<s16>(kArgs2)), bctr() | 1,
            lwz(4, kBuf, 0), stw(4, kArgs3 + 12, 0),            // the stdout ihandle
            mtspr(ppc::spr::CTR, 31), li(3, static_cast<s16>(kArgs3)), bctr() | 1,
            stw(3, 0x7184, 0),                         // status of the write call
            b(0),
        };
    }
    // An ELF file: header, two PT_LOAD segments (code, data).
    std::vector<u8> elf() const
    {
        std::vector<u8> e(0x600);
        auto b16 = [&](size_t a, u32 v) { e[a] = static_cast<u8>(v >> 8); e[a + 1] = static_cast<u8>(v); };
        auto b32 = [&](size_t a, u32 v) { b16(a, v >> 16); b16(a + 2, v & 0xFFFF); };
        e[0] = 0x7F; e[1] = 'E'; e[2] = 'L'; e[3] = 'F'; e[4] = 1; e[5] = 2; e[6] = 1;
        b16(16, 2); b16(18, 20); b32(20, 1); b32(24, kCode); b32(28, 52); b16(42, 32); b16(44, 2);
        const u32 code_bytes = static_cast<u32>(code.size() * 4);
        b32(52, 1); b32(56, 0x100); b32(60, kCode); b32(64, kCode); b32(68, code_bytes); b32(72, code_bytes);
        b32(84, 1); b32(88, 0x300); b32(92, kData); b32(96, kData); b32(100, 0x200); b32(104, 0x400);
        for (size_t i = 0; i < code.size(); ++i) b32(0x100 + 4 * i, code[i]);
        std::memcpy(&e[0x300], data.data(), data.size());
        return e;
    }
};

} // namespace test_client
} // namespace g3

#endif // G3_TESTS_UNIT_MACHINE_TEST_CLIENT_H
