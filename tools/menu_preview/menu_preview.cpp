// SPDX-License-Identifier: GPL-3.0-or-later
//
// tools/menu_preview/menu_preview.cpp — renders the settings menu as the
// Pi draws it (platform/common/menu_painter) into PPM files, for looking
// at the design without a Pi.
//
//   g3_menu_preview <out-prefix> [width height]
// writes <prefix>-main.ppm, -drive.ppm, -files.ppm, -wifi.ppm, -running.ppm, -splash.ppm.

#include "platform/common/config_menu.h"
#include "platform/common/menu_painter.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace g3;

namespace {

struct SampleFs final : DirectoryLister {
    s32 list(const char* path, DirEntry* out, u32 max) override
    {
        struct F { const char* name; bool dir; u64 size; };
        static const F cd[] = {{"Mac OS 9.2.1 Install.iso", false, 671703040ull}, {"Utilities", true, 0},
                               {"Developer Tools.toast", false, 412000000ull}, {"Games Collection.iso", false, 598000000ull}};
        static const F hdd[] = {{"hd1.img", false, 2147483648ull}, {"Work.img", false, 1073741824ull}};
        const F* f = std::strstr(path, "hdd") ? hdd : cd;
        const u32 n = std::strstr(path, "hdd") ? 2 : 4;
        u32 k = 0;
        for (; k < n && k < max; ++k) {
            std::strncpy(out[k].name, f[k].name, DirEntry::kNameMax - 1);
            out[k].directory = f[k].dir;
            out[k].size = f[k].size;
        }
        return static_cast<s32>(k);
    }
};

bool save(const std::string& path, const std::vector<u32>& px, u32 w, u32 h)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (u32 p : px) {
        const u8 rgb[3] = {static_cast<u8>(p >> 16), static_cast<u8>(p >> 8), static_cast<u8>(p)};
        std::fwrite(rgb, 1, 3, f);
    }
    return std::fclose(f) == 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: g3_menu_preview <out-prefix> [width height]\n");
        return 2;
    }
    const u32 w = argc > 3 ? static_cast<u32>(std::strtoul(argv[2], nullptr, 10)) : 1280;
    const u32 h = argc > 3 ? static_cast<u32>(std::strtoul(argv[3], nullptr, 10)) : 720;
    std::vector<u32> px(static_cast<size_t>(w) * h);
    ScreenSurface s;
    s.base = reinterpret_cast<u8*>(px.data());
    s.width = w;
    s.height = h;
    s.pitch = w * 4;
    s.depth = 32;

    MachineConfig cfg;
    cfg.ram_mb = 256;
    std::strcpy(cfg.rom, "/g3macpi/rom/Mac OS ROM");
    cfg.drive[0].type = MachineConfig::DriveType::CdRom;
    std::strcpy(cfg.drive[0].image, "/g3macpi/cd/Mac OS 9.2.1 Install.iso");
    cfg.drive[2].type = MachineConfig::DriveType::Disk;
    std::strcpy(cfg.drive[2].image, "/g3macpi/hdd/hd1.img");
    SampleFs fs;
    ConfigMenu menu(cfg, fs, "/g3macpi/disks", "/g3macpi/rom");
    menu.set_media_folders("/g3macpi/cd", "/g3macpi/hdd");
    ConfigMenu::Screen scr;
    const std::string prefix = argv[1];
    auto shot = [&](const char* name) {
        menu.render(scr);
        MenuPainter::paint(scr, s);
        const std::string path = prefix + "-" + name + ".ppm";
        std::printf("%s %s\n", save(path, px, w, h) ? "wrote" : "FAILED", path.c_str());
    };
    menu.key(menu_key::kDown);
    menu.key(menu_key::kDown);
    menu.set_status("Settings saved to /g3macpi/g3macpi.ini");
    shot("main");
    menu.key(menu_key::kEnter);   // the CD-ROM drive's page
    menu.key(menu_key::kDown);
    shot("drive");
    menu.key(menu_key::kEnter);   // its file picker
    menu.key(menu_key::kDown);
    menu.key(menu_key::kDown);
    shot("files");
    menu.key(menu_key::kEscape);
    menu.key(menu_key::kEscape);
    menu.key(menu_key::kEscape);
    // The WiFi page, typing the network's name ("My WiFi").
    menu.set_status("");
    while (menu.key(menu_key::kDown), true) {
        menu.render(scr);
        bool at = false;
        for (u32 r = 0; r < ConfigMenu::kRows; ++r)
            at = at || (scr.attr[r] == ConfigMenu::Attr::Selected && std::strstr(scr.text[r], "Network port"));
        if (at)
            break;
    }
    menu.key(menu_key::kEnter);
    menu.key(menu_key::kEnter);   // Use WiFi: yes
    menu.key(menu_key::kDown);
    menu.key(menu_key::kEnter);   // type the name
    const u8 typed[][2] = {{0x10, 0x02}, {0x1C, 0}, {0x2C, 0}, {0x1A, 0x02}, {0x0C, 0x02}, {0x09, 0x02}, {0x0C, 0x02}};
    for (const auto& k : typed)
        menu.key(k[0], k[1]);
    shot("wifi");
    menu.key(menu_key::kEscape);
    menu.key(menu_key::kEscape);
    menu.set_running(true);
    menu.set_status("");
    shot("running");
    MenuPainter::paint_splash(s, "Starting the Mac...", "F12 or Esc: settings");
    const std::string sp = prefix + "-splash.ppm";
    std::printf("%s %s\n", save(sp, px, w, h) ? "wrote" : "FAILED", sp.c_str());
    return 0;
}
