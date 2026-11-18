// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/forth_words.h — the Open Firmware words of our
// Forth: device tree access, property encoding, packages and loading.
//
// Stack effects (IEEE 1275-1994; written from knowledge of the standard,
// to verify against its text — docs/firmware/forth-words.md):
//   dev ( "path" -- )                 find-device ( path len -- )
//   device-end ( -- )                 active-package ( -- phandle )
//   find-package ( name len -- false | phandle true )
//   get-package-property ( name len phandle -- true | prop len false )
//   get-inherited-property ( name len -- true | prop len false )
//   property ( prop len name len -- )
//   new-device ( -- )  finish-device ( -- )
//   device-name ( name len -- )       device-type ( name len -- )
//   encode-int ( n -- prop len )      encode-string ( str len -- prop len )
//   encode-bytes ( data len -- prop len )  encode+ ( p1 l1 p2 l2 -- p3 l3 )
//   decode-int ( prop len -- prop' len' n )
//   decode-string ( prop len -- prop' len' str len )
//   open-dev ( path len -- ihandle | 0 )   close-dev ( ihandle -- )
//   $call-method ( ... method len ihandle -- ... ): read, write, seek
//   load-base ( -- addr )   load-size ( -- n )
//   load ( "device-specifier<eol>" -- ): reads the file at load-base
//   ms ( n -- )   get-msecs ( -- ms )
//   reg ( phys size -- ): "reg" of the active package (1 address cell and
//     1 size cell assumed — documented limit)
//   my-self ( -- ihandle )   ihandle>phandle ( ihandle -- phandle )
//     Between new-device and finish-device, my-self is an instance of the
//     node being built (IEEE 1275 new-device), so `my-self ihandle>phandle`
//     gives that node; otherwise my-self is 0.
//   init-program ( -- ): prepares the image at load-base (ELF, or a CHRP
//     bootinfo object whose script is then run — our choice, see docs)
//   go ( -- ): starts the prepared ELF program (through the go callback)
//   boot ( "device-specifier<eol>" -- ): load + init-program + go
//
// Property values handed to Forth (get-*-property, encode-*) are copied
// into a scratch region of guest memory used round-robin; a script must
// use them before many more encodings have been made (documented limit).

#ifndef G3_CORE_FIRMWARE_OPENFIRM_FORTH_WORDS_H
#define G3_CORE_FIRMWARE_OPENFIRM_FORTH_WORDS_H

#include "core/common/types.h"
#include "core/firmware/forth/forth.h"
#include "core/firmware/openfirm/bootinfo.h"
#include "core/firmware/openfirm/device_tree.h"
#include "core/firmware/openfirm/elf_loader.h"
#include "core/firmware/openfirm/instances.h"
#include "core/firmware/openfirm/memory_map.h"

namespace g3::of {

class FirmwareWords {
public:
    using Clock = u32 (*)(void* context);  // milliseconds

    // scratch: guest memory for property values; load_base: where `load` puts files.
    FirmwareWords(forth::Forth& f, DeviceTree& tree, InstanceTable& inst, u32 scratch_base,
                  u32 scratch_size, u32 load_base, u32 load_limit);

    void set_clock(Clock c, void* ctx) { clock_ = c; clock_ctx_ = ctx; }
    // Memory for init-program to claim (optional).
    void set_memory(MemoryMap* m) { memory_ = m; }
    // Called by `go` with the entry point; the machine starts the client.
    using Go = void (*)(void* context, u32 entry);
    void set_go(Go g, void* ctx) { go_ = g; go_ctx_ = ctx; }

    // Puts a file at load-base as `load` would (for tools and tests that
    // have the file in hand); false if it doesn't fit below the load limit.
    bool place_file(const u8* data, u32 len, const char* path);

    Phandle active_package() const { return active_; }
    u32 load_size() const { return load_size_; }
    const char* load_path() const { return load_path_; }
    bool program_ready() const { return program_ready_; }
    u32 program_entry() const { return program_.entry; }

private:
    using Native = forth::Forth::Native;
    struct Def { const char* name; Native fn; };
    static FirmwareWords& self(void* ctx) { return *static_cast<FirmwareWords*>(ctx); }

    u32 scratch(u32 n);  // guest address of n free scratch bytes
    u32 copy_out(const u8* data, u32 len);
    bool read_name(u32 addr, u32 len, char* out, u32 max);
    void push_property(Phandle h, const char* name);  // true | prop len false
    Ihandle open_path(const char* path);

    static void w_dev(forth::Forth&, void*);
    static void w_find_device(forth::Forth&, void*);
    static void w_device_end(forth::Forth&, void*);
    static void w_active_package(forth::Forth&, void*);
    static void w_find_package(forth::Forth&, void*);
    static void w_get_package_property(forth::Forth&, void*);
    static void w_get_inherited_property(forth::Forth&, void*);
    static void w_property(forth::Forth&, void*);
    static void w_new_device(forth::Forth&, void*);
    static void w_finish_device(forth::Forth&, void*);
    static void w_device_name(forth::Forth&, void*);
    static void w_device_type(forth::Forth&, void*);
    static void w_encode_int(forth::Forth&, void*);
    static void w_encode_string(forth::Forth&, void*);
    static void w_encode_bytes(forth::Forth&, void*);
    static void w_encode_plus(forth::Forth&, void*);
    static void w_decode_int(forth::Forth&, void*);
    static void w_decode_string(forth::Forth&, void*);
    static void w_open_dev(forth::Forth&, void*);
    static void w_close_dev(forth::Forth&, void*);
    static void w_call_method(forth::Forth&, void*);
    static void w_load_base(forth::Forth&, void*);
    static void w_dot_registers(forth::Forth&, void*);
    static void w_set_my_self(forth::Forth&, void*);
    static void w_load_size(forth::Forth&, void*);
    static void w_load(forth::Forth&, void*);
    static void w_ms(forth::Forth&, void*);
    static void w_get_msecs(forth::Forth&, void*);
    static void w_init_program(forth::Forth&, void*);
    static void w_reg(forth::Forth&, void*);
    static void w_my_self(forth::Forth&, void*);
    static void w_ihandle_to_phandle(forth::Forth&, void*);
    static void w_go(forth::Forth&, void*);
    static void w_boot(forth::Forth&, void*);
    bool load_file(const char* path);
    void run_bootinfo();

    forth::Forth& f_;
    DeviceTree& tree_;
    InstanceTable& inst_;
    u32 scratch_base_, scratch_size_, scratch_pos_ = 0;
    u32 load_base_, load_limit_;
    u32 load_size_ = 0;
    Phandle active_ = kNone;
    Clock clock_ = nullptr;
    void* clock_ctx_ = nullptr;
    MemoryMap* memory_ = nullptr;
    Go go_ = nullptr;
    void* go_ctx_ = nullptr;
    char load_path_[256] = {};
    ElfProgram program_;
    bool program_ready_ = false;
    bool in_script_ = false;
    bool ran_script_ = false;
    // Nodes under construction (new-device ... finish-device): their
    // "instances" for my-self are kSelfBase + depth.
    static constexpr u32 kSelfBase = 0x30000000u;
    Phandle building_[16] = {};
    u32 building_depth_ = 0;
    u32 my_self_ = 0;         // `to my-self` outside new-device ... finish-device  // the last init-program ran a bootinfo script
    BootInfo bootinfo_;
    u8 text_[64 * 1024] = {};  // host copy of a bootinfo object's text
    u8 buf_[4096] = {};  // staging for property bytes and file data
};

} // namespace g3::of

#endif // G3_CORE_FIRMWARE_OPENFIRM_FORTH_WORDS_H
