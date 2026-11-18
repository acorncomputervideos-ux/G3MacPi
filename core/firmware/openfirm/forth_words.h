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
//
// Property values handed to Forth (get-*-property, encode-*) are copied
// into a scratch region of guest memory used round-robin; a script must
// use them before many more encodings have been made (documented limit).

#ifndef G3_CORE_FIRMWARE_OPENFIRM_FORTH_WORDS_H
#define G3_CORE_FIRMWARE_OPENFIRM_FORTH_WORDS_H

#include "core/common/types.h"
#include "core/firmware/forth/forth.h"
#include "core/firmware/openfirm/device_tree.h"
#include "core/firmware/openfirm/instances.h"

namespace g3::of {

class FirmwareWords {
public:
    using Clock = u32 (*)(void* context);  // milliseconds

    // scratch: guest memory for property values; load_base: where `load` puts files.
    FirmwareWords(forth::Forth& f, DeviceTree& tree, InstanceTable& inst, u32 scratch_base,
                  u32 scratch_size, u32 load_base, u32 load_limit);

    void set_clock(Clock c, void* ctx) { clock_ = c; clock_ctx_ = ctx; }

    Phandle active_package() const { return active_; }
    u32 load_size() const { return load_size_; }

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
    static void w_load_size(forth::Forth&, void*);
    static void w_load(forth::Forth&, void*);
    static void w_ms(forth::Forth&, void*);
    static void w_get_msecs(forth::Forth&, void*);

    forth::Forth& f_;
    DeviceTree& tree_;
    InstanceTable& inst_;
    u32 scratch_base_, scratch_size_, scratch_pos_ = 0;
    u32 load_base_, load_limit_;
    u32 load_size_ = 0;
    Phandle active_ = kNone;
    Clock clock_ = nullptr;
    void* clock_ctx_ = nullptr;
    u8 buf_[4096] = {};  // staging for property bytes and file data
};

} // namespace g3::of

#endif // G3_CORE_FIRMWARE_OPENFIRM_FORTH_WORDS_H
