// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/forth/forth.h — the Forth interpreter of our Open Firmware.
//
// Mac OS's boot file carries an Open Firmware (IEEE 1275) Forth script.
// This is an independent implementation of the language core that such
// scripts use; firmware words (device tree, packages, loading) are added
// by the firmware through define_native().
//
// Facts (docs/firmware/forth.md): IEEE 1275-1994's Forth is based on ANS
// Forth (X3.215-1994); IEEE 1275 differences used here: the initial
// number base is hexadecimal; d# h# o# b# prefixes; the " string word with
// "(hex hex ...)" byte escapes. Cells are 32 bits (PowerPC binding).
//
// Memory model: Forth addresses are guest addresses. The data space
// (variables, create, allot, here, string buffers) lives in a region of
// guest memory accessed through the MemoryInterface, so values stored by a
// script are visible to client programs, as on real firmware. Compiled
// colon definitions and the word headers are host-side.
//
// Errors don't use C++ exceptions: an error (unknown word, stack
// underflow, throw) unwinds to the nearest catch or to interpret().

#ifndef G3_CORE_FIRMWARE_FORTH_FORTH_H
#define G3_CORE_FIRMWARE_FORTH_FORTH_H

#include "core/common/types.h"
#include "core/cpu/ppc750/memory_interface.h"

namespace g3::forth {

using Cell = u32;

class Forth {
public:
    static constexpr u32 kStackCells = 256;
    static constexpr u32 kMaxWords = 1024;
    static constexpr u32 kMaxCode = 32768;      // compiled cells
    static constexpr u32 kNamePool = 16384;
    static constexpr u32 kMaxNameLength = 31;
    static constexpr u32 kStringBuffer = 256;   // transient " / s" buffers (two)

    // Throw codes (ANS Forth).
    static constexpr s32 kAbort = -1, kAbortQuote = -2, kStackOverflow = -3,
                         kStackUnderflow = -4, kReturnOverflow = -5, kReturnUnderflow = -6, kDivByZero = -10,
                         kUndefined = -13, kCompileOnly = -14, kInvalidAddress = -9,
                         kControlMismatch = -22;

    using Sink = void (*)(void* context, const char* text, u32 len);
    using Native = void (*)(Forth& f, void* context);

    // data_base/data_size: the guest memory region for the data space.
    Forth(ppc::MemoryInterface& mem, u32 data_base, u32 data_size);

    void set_output(Sink s, void* ctx) { out_ = s; out_ctx_ = ctx; }

    // Runs text as if typed; returns false if it ended in an uncaught error
    // (then error_code()/error_text() describe it; stacks are reset).
    bool interpret(const char* text, u32 len);
    bool interpret(const char* text);
    s32 error_code() const { return error_code_; }
    const char* error_text() const { return error_text_; }

    // Data stack access for native words and tests.
    void push(Cell v);
    Cell pop();
    Cell peek(u32 i = 0) const;  // 0 = top
    u32 depth() const { return sp_; }
    bool compiling() const { return state_; }

    // Adds a word implemented in C++.
    bool define_native(const char* name, Native fn, void* context, bool immediate = false);
    // A constant-like word (pushes `value`).
    bool define_constant(const char* name, Cell value);

    // Signals an error from a native word (like `throw`).
    void fail(s32 code, const char* text = nullptr);
    bool failed() const { return error_; }

    // Data space.
    u32 here() const { return here_; }
    bool allot(s32 n);
    // Copies a string into a transient buffer; returns its guest address.
    u32 transient_string(const char* s, u32 len);
    // Reads a guest string into host memory (NUL-terminated, truncated to max-1).
    bool read_string(u32 addr, u32 len, char* out, u32 max);

    ppc::MemoryInterface& memory() { return mem_; }
    void emit_text(const char* s, u32 len);

private:
    enum class Kind : u8 { Primitive, Native, Colon, Variable, Constant, Value, Create, Defer };
    struct Word {
        u32 name;          // name pool offset
        u8 name_len;
        bool immediate;
        bool hidden;       // during its own definition
        Kind kind;
        u32 param;         // primitive id, code index, data address or constant
        u32 does;          // Create: code index of does> part (0 = none)
        Native fn;
        void* ctx;
    };

    // Control-flow entries while compiling.
    enum class Ctl : u8 { Orig, Dest, Do, Case, Of, Colon };
    struct Control { Ctl kind; u32 at; };

    // --- input ---
    struct Source { const char* text; u32 len; u32 pos; };
    bool parse_word(const char*& w, u32& len);
    bool parse_until(char delim, const char*& s, u32& len);
    bool parse_quoted(char* out, u32 max, u32& len);  // the IEEE 1275 " syntax

    // --- dictionary ---
    s32 find(const char* name, u32 len) const;
    s32 add_word(const char* name, u32 len, Kind kind, u32 param);
    void add_primitives();
    bool name_equal(const Word& w, const char* name, u32 len) const;

    // --- compiling ---
    bool emit_cell(u32 c);
    void ctl_push(Ctl k, u32 at);
    bool ctl_pop(Ctl k, u32& at);
    bool compile_string(u32 op, const char* s, u32 len);

    // --- execution ---
    bool interpret_source();
    bool interpret_word(const char* w, u32 len);
    bool to_number(const char* w, u32 len, Cell base, Cell& value) const;
    bool execute(u32 word);
    bool run_code(u32 ip);
    bool primitive(u32 id);
    void rpush(Cell v);
    Cell rpop();
    Cell base() const;
    void print_number(Cell v, bool is_signed, u32 width = 0);
    void reset_after_error();

    ppc::MemoryInterface& mem_;
    Sink out_ = nullptr;
    void* out_ctx_ = nullptr;

    Cell stack_[kStackCells] = {};
    u32 sp_ = 0;
    Cell rstack_[kStackCells] = {};
    u32 rp_ = 0;

    Word words_[kMaxWords] = {};
    u32 word_count_ = 0;
    char names_[kNamePool] = {};
    u32 names_used_ = 0;
    u32 code_[kMaxCode] = {};
    u32 code_used_ = 1;    // cell 0 unused (0 = "no does> part")

    bool state_ = false;   // compiling
    s32 defining_ = -1;    // word being defined
    s32 latest_create_ = -1;
    Control ctl_[64] = {};
    u32 ctl_sp_ = 0;
    u32 leave_[64] = {};   // pending LEAVE / ?DO forward references
    u32 leave_sp_ = 0;

    Source src_[8] = {};
    u32 src_depth_ = 0;
    char eval_buf_[4][1024] = {};

    u32 data_base_, data_end_, here_;
    u32 base_addr_ = 0;    // the BASE variable
    u32 strbuf_[2] = {};
    u32 strbuf_next_ = 0;

    u32 call_depth_ = 0;
    u32 catch_depth_ = 0;
    bool error_ = false;
    s32 error_code_ = 0;
    char error_text_[96] = {};
};

} // namespace g3::forth

#endif // G3_CORE_FIRMWARE_FORTH_FORTH_H
