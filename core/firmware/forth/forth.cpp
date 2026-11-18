// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/forth/forth.cpp — see forth.h.

#include "core/firmware/forth/forth.h"

#include "core/common/cstring.h"

namespace g3::forth {

namespace {

// Inline operations in compiled code (cells with the top bit set).
enum Op : u32 {
    OP_LIT = 0x80000001, OP_BRANCH, OP_0BRANCH, OP_EXIT, OP_DO, OP_QDO, OP_LOOP, OP_PLOOP,
    OP_LEAVE, OP_STR, OP_PRINT, OP_DOES, OP_TOVAL, OP_OF, OP_ABORTQ, OP_DROP,
};
constexpr u32 kXtBase = 0x00E00000u;  // execution tokens: kXtBase + word index

enum Prim : u32 {
    // stack
    P_DUP, P_DROP, P_SWAP, P_OVER, P_ROT, P_MROT, P_NIP, P_TUCK, P_PICK, P_ROLL, P_QDUP,
    P_2DUP, P_2DROP, P_2SWAP, P_2OVER, P_DEPTH, P_TOR, P_RFROM, P_RFETCH, P_I, P_J, P_UNLOOP,
    // arithmetic and logic
    P_ADD, P_SUB, P_MUL, P_DIV, P_MOD, P_DIVMOD, P_NEGATE, P_ABS, P_MIN, P_MAX, P_AND, P_OR,
    P_XOR, P_INVERT, P_LSHIFT, P_RSHIFT, P_1ADD, P_1SUB, P_2ADD, P_2SUB, P_2MUL, P_2DIV, P_U2DIV,
    P_EQ, P_NE, P_LT, P_GT, P_ULT, P_UGT, P_0EQ, P_0NE, P_0LT, P_0GT, P_LE, P_GE, P_WITHIN,
    P_BETWEEN, P_TRUE, P_FALSE, P_BL, P_CELLADD, P_CELLS, P_CHARADD, P_CHARS, P_ALIGNED,
    // memory
    P_FETCH, P_STORE, P_CFETCH, P_CSTORE, P_WFETCH, P_WSTORE, P_LFETCH, P_LSTORE, P_PSTORE,
    P_HERE, P_ALLOT, P_COMMA, P_CCOMMA, P_WCOMMA, P_LCOMMA, P_ALIGN, P_MOVE, P_FILL, P_COMP,
    P_COUNT,
    // output
    P_TYPE, P_EMIT, P_CR, P_SPACE, P_SPACES, P_DOT, P_UDOT, P_DOTS, P_DOTR, P_UDOTR, P_DOTD,
    P_DOTH,
    // base
    P_BASE, P_DECIMAL, P_HEX, P_OCTAL, P_BINARY,
    // control
    P_EXECUTE, P_EVALUATE, P_THROW, P_CATCH, P_ABORT,
    // immediate / parsing words
    P_COLON, P_SEMI, P_IF, P_ELSE, P_THEN, P_BEGIN, P_UNTIL, P_AGAIN, P_WHILE, P_REPEAT, P_DO,
    P_QDO, P_LOOP, P_PLOOP, P_LEAVE, P_RECURSE, P_EXIT, P_LITERAL, P_LBRACKET, P_RBRACKET,
    P_TICK, P_BRACKET_TICK, P_PAREN, P_BACKSLASH, P_DOTQUOTE, P_SQUOTE, P_QUOTE, P_DOTPAREN,
    P_CHAR, P_BRACKET_CHAR, P_VARIABLE, P_CONSTANT, P_VALUE, P_TO, P_CREATE, P_DOES, P_DEFER,
    P_IS, P_IMMEDIATE, P_CASE, P_OF, P_ENDOF, P_ENDCASE, P_ABORTQ, P_DNUM, P_HNUM, P_ONUM,
    P_BNUM,
};

struct PrimDef { const char* name; Prim id; bool immediate; };

const PrimDef kPrims[] = {
    {"dup", P_DUP, false}, {"drop", P_DROP, false}, {"swap", P_SWAP, false},
    {"over", P_OVER, false}, {"rot", P_ROT, false}, {"-rot", P_MROT, false},
    {"nip", P_NIP, false}, {"tuck", P_TUCK, false}, {"pick", P_PICK, false},
    {"roll", P_ROLL, false}, {"?dup", P_QDUP, false}, {"2dup", P_2DUP, false},
    {"2drop", P_2DROP, false}, {"2swap", P_2SWAP, false}, {"2over", P_2OVER, false},
    {"depth", P_DEPTH, false}, {">r", P_TOR, false}, {"r>", P_RFROM, false},
    {"r@", P_RFETCH, false}, {"i", P_I, false}, {"j", P_J, false}, {"unloop", P_UNLOOP, false},
    {"+", P_ADD, false}, {"-", P_SUB, false}, {"*", P_MUL, false}, {"/", P_DIV, false},
    {"mod", P_MOD, false}, {"/mod", P_DIVMOD, false}, {"negate", P_NEGATE, false},
    {"abs", P_ABS, false}, {"min", P_MIN, false}, {"max", P_MAX, false},
    {"and", P_AND, false}, {"or", P_OR, false}, {"xor", P_XOR, false},
    {"invert", P_INVERT, false}, {"not", P_INVERT, false}, {"lshift", P_LSHIFT, false},
    {"<<", P_LSHIFT, false}, {"rshift", P_RSHIFT, false}, {">>", P_RSHIFT, false},
    {"1+", P_1ADD, false}, {"1-", P_1SUB, false}, {"2+", P_2ADD, false}, {"2-", P_2SUB, false},
    {"2*", P_2MUL, false}, {"2/", P_2DIV, false}, {"u2/", P_U2DIV, false},
    {"=", P_EQ, false}, {"<>", P_NE, false}, {"<", P_LT, false}, {">", P_GT, false},
    {"u<", P_ULT, false}, {"u>", P_UGT, false}, {"0=", P_0EQ, false}, {"0<>", P_0NE, false},
    {"0<", P_0LT, false}, {"0>", P_0GT, false}, {"<=", P_LE, false}, {">=", P_GE, false},
    {"within", P_WITHIN, false}, {"between", P_BETWEEN, false}, {"true", P_TRUE, false},
    {"false", P_FALSE, false}, {"bl", P_BL, false}, {"cell+", P_CELLADD, false},
    {"cells", P_CELLS, false}, {"char+", P_CHARADD, false}, {"chars", P_CHARS, false},
    {"aligned", P_ALIGNED, false},
    {"@", P_FETCH, false}, {"!", P_STORE, false}, {"c@", P_CFETCH, false},
    {"c!", P_CSTORE, false}, {"w@", P_WFETCH, false}, {"w!", P_WSTORE, false},
    {"l@", P_LFETCH, false}, {"l!", P_LSTORE, false}, {"+!", P_PSTORE, false},
    {"here", P_HERE, false}, {"allot", P_ALLOT, false}, {",", P_COMMA, false},
    {"c,", P_CCOMMA, false}, {"w,", P_WCOMMA, false}, {"l,", P_LCOMMA, false},
    {"align", P_ALIGN, false}, {"move", P_MOVE, false}, {"cmove", P_MOVE, false},
    {"fill", P_FILL, false}, {"comp", P_COMP, false}, {"count", P_COUNT, false},
    {"type", P_TYPE, false}, {"emit", P_EMIT, false}, {"cr", P_CR, false},
    {"space", P_SPACE, false}, {"spaces", P_SPACES, false}, {".", P_DOT, false},
    {"u.", P_UDOT, false}, {".s", P_DOTS, false}, {".r", P_DOTR, false},
    {"u.r", P_UDOTR, false}, {".d", P_DOTD, false}, {".h", P_DOTH, false},
    {"base", P_BASE, false}, {"decimal", P_DECIMAL, false}, {"hex", P_HEX, false},
    {"octal", P_OCTAL, false}, {"binary", P_BINARY, false},
    {"execute", P_EXECUTE, false}, {"evaluate", P_EVALUATE, false}, {"throw", P_THROW, false},
    {"catch", P_CATCH, false}, {"abort", P_ABORT, false},
    {":", P_COLON, false}, {";", P_SEMI, true}, {"if", P_IF, true}, {"else", P_ELSE, true},
    {"then", P_THEN, true}, {"begin", P_BEGIN, true}, {"until", P_UNTIL, true},
    {"again", P_AGAIN, true}, {"while", P_WHILE, true}, {"repeat", P_REPEAT, true},
    {"do", P_DO, true}, {"?do", P_QDO, true}, {"loop", P_LOOP, true}, {"+loop", P_PLOOP, true},
    {"leave", P_LEAVE, true}, {"recurse", P_RECURSE, true}, {"exit", P_EXIT, true},
    {"literal", P_LITERAL, true}, {"[", P_LBRACKET, true}, {"]", P_RBRACKET, false},
    {"'", P_TICK, false}, {"[']", P_BRACKET_TICK, true}, {"(", P_PAREN, true},
    {"\\", P_BACKSLASH, true}, {".\"", P_DOTQUOTE, true}, {"s\"", P_SQUOTE, true},
    {"\"", P_QUOTE, true}, {".(", P_DOTPAREN, true}, {"char", P_CHAR, false},
    {"[char]", P_BRACKET_CHAR, true}, {"variable", P_VARIABLE, false},
    {"constant", P_CONSTANT, false}, {"value", P_VALUE, false}, {"to", P_TO, true},
    {"create", P_CREATE, false}, {"does>", P_DOES, true}, {"defer", P_DEFER, false},
    {"is", P_IS, true}, {"immediate", P_IMMEDIATE, false}, {"case", P_CASE, true},
    {"of", P_OF, true}, {"endof", P_ENDOF, true}, {"endcase", P_ENDCASE, true},
    {"abort\"", P_ABORTQ, true}, {"d#", P_DNUM, true}, {"h#", P_HNUM, true},
    {"o#", P_ONUM, true}, {"b#", P_BNUM, true},
};

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f'; }
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
s32 sgn(Cell v) { return static_cast<s32>(v); }
Cell flag(bool b) { return b ? 0xFFFFFFFFu : 0; }

} // namespace

// --- setup -------------------------------------------------------------------------------

Forth::Forth(ppc::MemoryInterface& mem, u32 data_base, u32 data_size)
    : mem_(mem), data_base_(data_base), data_end_(data_base + data_size), here_(data_base)
{
    add_primitives();
    base_addr_ = here_;
    allot(4);
    mem_.write32(base_addr_, 16);  // IEEE 1275: the initial base is hexadecimal
    strbuf_[0] = here_;
    allot(kStringBuffer);
    strbuf_[1] = here_;
    allot(kStringBuffer);
}

void Forth::add_primitives()
{
    for (const PrimDef& p : kPrims) {
        const s32 w = add_word(p.name, cstr::length(p.name), Kind::Primitive, p.id);
        if (w >= 0)
            words_[w].immediate = p.immediate;
    }
}

bool Forth::name_equal(const Word& w, const char* name, u32 len) const
{
    if (w.name_len != len)
        return false;
    for (u32 i = 0; i < len; ++i)
        if (lower(names_[w.name + i]) != lower(name[i]))
            return false;
    return true;
}

s32 Forth::find(const char* name, u32 len) const
{
    for (u32 i = word_count_; i-- > 0;)
        if (!words_[i].hidden && name_equal(words_[i], name, len))
            return static_cast<s32>(i);
    return -1;
}

s32 Forth::add_word(const char* name, u32 len, Kind kind, u32 param)
{
    if (len == 0 || len > kMaxNameLength || word_count_ >= kMaxWords || names_used_ + len > kNamePool) {
        fail(-8, "dictionary full or bad name");
        return -1;
    }
    Word& w = words_[word_count_];
    w = Word{};
    w.name = names_used_;
    w.name_len = static_cast<u8>(len);
    for (u32 i = 0; i < len; ++i)
        names_[names_used_++] = name[i];
    w.kind = kind;
    w.param = param;
    return static_cast<s32>(word_count_++);
}

bool Forth::define_native(const char* name, Native fn, void* context, bool immediate)
{
    const s32 w = add_word(name, cstr::length(name), Kind::Native, 0);
    if (w < 0)
        return false;
    words_[w].fn = fn;
    words_[w].ctx = context;
    words_[w].immediate = immediate;
    return true;
}

bool Forth::define_constant(const char* name, Cell value)
{
    return add_word(name, cstr::length(name), Kind::Constant, value) >= 0;
}

// --- errors and stacks -----------------------------------------------------------------

void Forth::fail(s32 code, const char* text)
{
    if (error_)
        return;  // keep the first error
    error_ = true;
    error_code_ = code;
    u32 n = 0;
    if (text)
        for (; text[n] && n + 1 < sizeof error_text_; ++n)
            error_text_[n] = text[n];
    error_text_[n] = '\0';
}

void Forth::push(Cell v)
{
    if (sp_ >= kStackCells) {
        fail(kStackOverflow, "stack overflow");
        return;
    }
    stack_[sp_++] = v;
}

Cell Forth::pop()
{
    if (sp_ == 0) {
        fail(kStackUnderflow, "stack underflow");
        return 0;
    }
    return stack_[--sp_];
}

Cell Forth::peek(u32 i) const { return i < sp_ ? stack_[sp_ - 1 - i] : 0; }

void Forth::rpush(Cell v)
{
    if (rp_ >= kStackCells) {
        fail(kStackOverflow, "return stack overflow");
        return;
    }
    rstack_[rp_++] = v;
}

Cell Forth::rpop()
{
    if (rp_ == 0) {
        fail(kReturnUnderflow, "return stack underflow");
        return 0;
    }
    return rstack_[--rp_];
}

void Forth::reset_after_error()
{
    sp_ = 0;
    rp_ = 0;
    if (state_ && defining_ >= 0 && static_cast<u32>(defining_) == word_count_ - 1)
        --word_count_;  // drop the half-defined word
    state_ = false;
    defining_ = -1;
    ctl_sp_ = 0;
    leave_sp_ = 0;
    call_depth_ = 0;
    src_depth_ = 0;
}

// --- data space ----------------------------------------------------------------------------

bool Forth::allot(s32 n)
{
    const s64 next = static_cast<s64>(here_) + n;
    if (next < data_base_ || next > data_end_) {
        fail(-8, "data space full");
        return false;
    }
    here_ = static_cast<u32>(next);
    return true;
}

u32 Forth::transient_string(const char* s, u32 len)
{
    if (len > kStringBuffer)
        len = kStringBuffer;
    const u32 a = strbuf_[strbuf_next_];
    strbuf_next_ ^= 1;
    for (u32 i = 0; i < len; ++i)
        mem_.write8(a + i, static_cast<u8>(s[i]));
    return a;
}

bool Forth::read_string(u32 addr, u32 len, char* out, u32 max)
{
    u32 n = 0;
    for (; n < len && n + 1 < max; ++n) {
        u8 c = 0;
        if (!mem_.read8(addr + n, c)) {
            fail(kInvalidAddress, "invalid address");
            out[n] = '\0';
            return false;
        }
        out[n] = static_cast<char>(c);
    }
    out[n] = '\0';
    return true;
}

Cell Forth::base() const
{
    u32 b = 10;
    mem_.read32(base_addr_, b);
    return (b >= 2 && b <= 36) ? b : 10;
}

void Forth::emit_text(const char* s, u32 len)
{
    if (out_ && len)
        out_(out_ctx_, s, len);
}

void Forth::print_number(Cell v, bool is_signed, u32 width)
{
    char buf[40];
    u32 n = 0;
    const bool neg = is_signed && sgn(v) < 0;
    u32 u = neg ? 0u - v : v;
    const u32 b = base();
    do {
        const u32 d = u % b;
        buf[n++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
        u /= b;
    } while (u);
    if (neg)
        buf[n++] = '-';
    char out[48];
    u32 m = 0;
    for (u32 pad = n; pad < width && m < 40; ++pad)
        out[m++] = ' ';
    while (n)
        out[m++] = buf[--n];
    emit_text(out, m);
}

// --- input ---------------------------------------------------------------------------------

bool Forth::parse_word(const char*& w, u32& len)
{
    Source& s = src_[src_depth_ - 1];
    while (s.pos < s.len && is_space(s.text[s.pos]))
        ++s.pos;
    if (s.pos >= s.len)
        return false;
    const u32 start = s.pos;
    while (s.pos < s.len && !is_space(s.text[s.pos]))
        ++s.pos;
    w = s.text + start;
    len = s.pos - start;
    if (s.pos < s.len)
        ++s.pos;  // consume the delimiter
    return true;
}

bool Forth::parse_until(char delim, const char*& str, u32& len)
{
    Source& s = src_[src_depth_ - 1];
    const u32 start = s.pos;
    while (s.pos < s.len && s.text[s.pos] != delim)
        ++s.pos;
    str = s.text + start;
    len = s.pos - start;
    const bool found = s.pos < s.len;
    if (found)
        ++s.pos;
    return found;
}

bool Forth::rest_of_line(const char*& str, u32& len)
{
    if (!src_depth_)
        return false;
    Source& s = src_[src_depth_ - 1];
    while (s.pos < s.len && (s.text[s.pos] == ' ' || s.text[s.pos] == '\t'))
        ++s.pos;
    const u32 start = s.pos;
    while (s.pos < s.len && s.text[s.pos] != '\n')
        ++s.pos;
    u32 end = s.pos;
    if (s.pos < s.len)
        ++s.pos;  // the newline
    while (end > start && is_space(s.text[end - 1]))
        --end;
    str = s.text + start;
    len = end - start;
    return len > 0;
}

// IEEE 1275 " : text up to a '"'. After a '"', '(' starts hex byte pairs
// up to ')' and the string continues; white space or the end of input ends
// the string; any other character is taken literally (so "" gives ").
bool Forth::parse_quoted(char* out, u32 max, u32& len)
{
    Source& s = src_[src_depth_ - 1];
    len = 0;
    while (s.pos < s.len) {
        const char c = s.text[s.pos++];
        if (c != '"') {
            if (len >= max)
                return false;
            out[len++] = c;
            continue;
        }
        if (s.pos >= s.len || is_space(s.text[s.pos])) {
            if (s.pos < s.len)
                ++s.pos;
            return true;
        }
        if (s.text[s.pos] == '(') {
            ++s.pos;
            int hi = -1;
            while (s.pos < s.len && s.text[s.pos] != ')') {
                const char h = lower(s.text[s.pos++]);
                int v = -1;
                if (h >= '0' && h <= '9')
                    v = h - '0';
                else if (h >= 'a' && h <= 'f')
                    v = h - 'a' + 10;
                if (v < 0)
                    continue;  // spaces between pairs
                if (hi < 0) {
                    hi = v;
                } else {
                    if (len >= max)
                        return false;
                    out[len++] = static_cast<char>((hi << 4) | v);
                    hi = -1;
                }
            }
            if (s.pos < s.len)
                ++s.pos;  // ')'
            continue;
        }
        if (len >= max)
            return false;
        out[len++] = s.text[s.pos++];
    }
    return true;  // end of input ends the string
}

// --- compiling -------------------------------------------------------------------------------

bool Forth::emit_cell(u32 c)
{
    if (code_used_ >= kMaxCode) {
        fail(-8, "code space full");
        return false;
    }
    code_[code_used_++] = c;
    return true;
}

void Forth::ctl_push(Ctl k, u32 at)
{
    if (ctl_sp_ >= sizeof ctl_ / sizeof ctl_[0]) {
        fail(kControlMismatch, "control structure too deep");
        return;
    }
    ctl_[ctl_sp_++] = {k, at};
}

bool Forth::ctl_pop(Ctl k, u32& at)
{
    if (ctl_sp_ == 0 || ctl_[ctl_sp_ - 1].kind != k) {
        fail(kControlMismatch, "control structure mismatch");
        return false;
    }
    at = ctl_[--ctl_sp_].at;
    return true;
}

bool Forth::compile_string(u32 op, const char* s, u32 len)
{
    if (!emit_cell(op) || !emit_cell(len))
        return false;
    for (u32 i = 0; i < len; i += 4) {
        u32 c = 0;
        for (u32 k = 0; k < 4 && i + k < len; ++k)
            c |= static_cast<u32>(static_cast<u8>(s[i + k])) << (8 * k);
        if (!emit_cell(c))
            return false;
    }
    return true;
}

// --- interpreter -----------------------------------------------------------------------------

bool Forth::interpret(const char* text) { return interpret(text, cstr::length(text)); }

bool Forth::interpret(const char* text, u32 len)
{
    error_ = false;
    error_code_ = 0;
    error_text_[0] = '\0';
    src_depth_ = 1;
    src_[0] = {text, len, 0};
    const bool ok = interpret_source();
    if (!ok)
        reset_after_error();
    src_depth_ = 0;
    return ok;
}

bool Forth::evaluate(const char* text, u32 len)
{
    if (src_depth_ == 0 || src_depth_ >= sizeof src_ / sizeof src_[0]) {
        fail(-8, "evaluate: too deep");
        return false;
    }
    src_[src_depth_++] = {text, len, 0};
    const bool ok = interpret_source();
    --src_depth_;
    return ok;
}

bool Forth::interpret_source()
{
    const char* w;
    u32 len;
    while (!error_ && parse_word(w, len))
        if (!interpret_word(w, len))
            return false;
    return !error_;
}

bool Forth::to_number(const char* w, u32 len, Cell b, Cell& value) const
{
    u32 i = 0;
    bool neg = false;
    if (len > 1 && w[0] == '-') {
        neg = true;
        i = 1;
    }
    if (i >= len)
        return false;
    u32 v = 0;
    for (; i < len; ++i) {
        const char c = lower(w[i]);
        u32 d;
        if (c >= '0' && c <= '9')
            d = static_cast<u32>(c - '0');
        else if (c >= 'a' && c <= 'z')
            d = static_cast<u32>(c - 'a' + 10);
        else
            return false;
        if (d >= b)
            return false;
        v = v * b + d;  // wraps modulo 2^32, like a cell
    }
    value = neg ? 0u - v : v;
    return true;
}

bool Forth::interpret_word(const char* w, u32 len)
{
    const s32 idx = find(w, len);
    if (idx >= 0) {
        if (state_ && !words_[idx].immediate)
            return emit_cell(static_cast<u32>(idx));
        return execute(static_cast<u32>(idx));
    }
    Cell v = 0;
    if (to_number(w, len, base(), v)) {
        if (state_)
            return emit_cell(OP_LIT) && emit_cell(v);
        push(v);
        return !error_;
    }
    char msg[48];
    u32 n = 0;
    for (; n < len && n < 40; ++n)
        msg[n] = w[n];
    msg[n++] = ' ';
    msg[n++] = '?';
    msg[n] = '\0';
    fail(kUndefined, msg);
    return false;
}

bool Forth::execute(u32 idx)
{
    if (idx >= word_count_) {
        fail(kUndefined, "invalid execution token");
        return false;
    }
    Word& w = words_[idx];
    switch (w.kind) {
    case Kind::Primitive:
        return primitive(w.param);
    case Kind::Native:
        w.fn(*this, w.ctx);
        return !error_;
    case Kind::Colon:
        return run_code(w.param);
    case Kind::Variable:
        push(w.param);
        return !error_;
    case Kind::Constant:
        push(w.param);
        return !error_;
    case Kind::Value: {
        u32 v = 0;
        mem_.read32(w.param, v);
        push(v);
        return !error_;
    }
    case Kind::Create:
        push(w.param);
        if (w.does)
            return run_code(w.does);
        return !error_;
    case Kind::Defer: {
        u32 xt = 0;
        mem_.read32(w.param, xt);
        if (xt < kXtBase || xt - kXtBase >= word_count_) {
            fail(kUndefined, "deferred word not set");
            return false;
        }
        return execute(xt - kXtBase);
    }
    }
    return false;
}

bool Forth::run_code(u32 ip)
{
    if (++call_depth_ > 200) {
        --call_depth_;
        fail(kReturnOverflow, "calls nested too deeply");
        return false;
    }
    bool ok = true;
    while (ok && !error_) {
        if (ip >= code_used_) {
            fail(kInvalidAddress, "ran off the end of code");
            ok = false;
            break;
        }
        const u32 c = code_[ip++];
        if (!(c & 0x80000000u)) {
            ok = execute(c);
            continue;
        }
        switch (c) {
        case OP_LIT:
            push(code_[ip++]);
            break;
        case OP_BRANCH:
            ip = code_[ip];
            break;
        case OP_0BRANCH:
            ip = pop() == 0 ? code_[ip] : ip + 1;
            break;
        case OP_EXIT:
            --call_depth_;
            return !error_;
        case OP_DO: {
            const Cell start = pop(), limit = pop();
            rpush(limit);
            rpush(start);
            break;
        }
        case OP_QDO: {
            const Cell start = pop(), limit = pop();
            if (start == limit) {
                ip = code_[ip];
            } else {
                rpush(limit);
                rpush(start);
                ++ip;
            }
            break;
        }
        case OP_LOOP: {
            if (rp_ < 2) {
                fail(kReturnUnderflow, "loop without do");
                break;
            }
            const Cell idx = ++rstack_[rp_ - 1];
            if (idx == rstack_[rp_ - 2]) {
                rp_ -= 2;
                ++ip;
            } else {
                ip = code_[ip];
            }
            break;
        }
        case OP_PLOOP: {
            if (rp_ < 2) {
                fail(kReturnUnderflow, "loop without do");
                break;
            }
            const s32 n = sgn(pop());
            const s32 d = sgn(rstack_[rp_ - 1] - rstack_[rp_ - 2]);  // index - limit
            const s32 nd = static_cast<s32>(static_cast<u32>(d) + static_cast<u32>(n));
            rstack_[rp_ - 1] += static_cast<u32>(n);
            // ANS: leave when the index crosses the limit-1 / limit boundary.
            if (((d ^ nd) & (d ^ n)) < 0) {
                rp_ -= 2;
                ++ip;
            } else {
                ip = code_[ip];
            }
            break;
        }
        case OP_LEAVE:
            if (rp_ < 2) {
                fail(kReturnUnderflow, "leave without do");
                break;
            }
            rp_ -= 2;
            ip = code_[ip];
            break;
        case OP_STR:
        case OP_PRINT:
        case OP_ABORTQ: {
            const u32 len = code_[ip++];
            char buf[kStringBuffer];
            const u32 n = len < kStringBuffer ? len : kStringBuffer;
            for (u32 i = 0; i < n; ++i)
                buf[i] = static_cast<char>(code_[ip + i / 4] >> (8 * (i % 4)));
            ip += (len + 3) / 4;
            if (c == OP_STR) {
                push(transient_string(buf, n));
                push(n);
            } else if (c == OP_PRINT) {
                emit_text(buf, n);
            } else if (pop() != 0) {
                buf[n < kStringBuffer ? n : kStringBuffer - 1] = '\0';
                emit_text(buf, n);
                fail(kAbortQuote, buf);
            }
            break;
        }
        case OP_DOES:
            if (latest_create_ < 0) {
                fail(kUndefined, "does> without create");
                break;
            }
            words_[latest_create_].does = ip;
            --call_depth_;
            return !error_;
        case OP_TOVAL:
            mem_.write32(code_[ip++], pop());
            break;
        case OP_OF: {
            const Cell x2 = pop(), x1 = pop();
            if (x1 == x2) {
                ++ip;
            } else {
                push(x1);
                ip = code_[ip];
            }
            break;
        }
        case OP_DROP:
            pop();
            break;
        default:
            fail(kUndefined, "bad compiled code");
            break;
        }
    }
    --call_depth_;
    return false;
}

// --- primitives --------------------------------------------------------------------------

bool Forth::primitive(u32 id)
{
    auto need = [&](u32 n) {
        if (sp_ < n) {
            fail(kStackUnderflow, "stack underflow");
            return false;
        }
        return true;
    };
    auto rd = [&](u32 a, u32 size, Cell& v) {
        bool ok;
        if (size == 1) {
            u8 b = 0;
            ok = mem_.read8(a, b);
            v = b;
        } else if (size == 2) {
            u16 h = 0;
            ok = mem_.read16(a, h);
            v = h;
        } else {
            ok = mem_.read32(a, v);
        }
        if (!ok)
            fail(kInvalidAddress, "invalid address");
        return ok;
    };
    auto wr = [&](u32 a, u32 size, Cell v) {
        const bool ok = size == 1   ? mem_.write8(a, static_cast<u8>(v))
                        : size == 2 ? mem_.write16(a, static_cast<u16>(v))
                                    : mem_.write32(a, v);
        if (!ok)
            fail(kInvalidAddress, "invalid address");
        return ok;
    };
    auto parse_name = [&](const char*& w, u32& len) {
        if (!parse_word(w, len)) {
            fail(-16, "name expected");
            return false;
        }
        return true;
    };
    auto set_base = [&](Cell b) { mem_.write32(base_addr_, b); };
    auto number_in = [&](Cell b) {
        const char* w;
        u32 len;
        Cell v = 0;
        if (!parse_name(w, len))
            return false;
        if (!to_number(w, len, b, v)) {
            fail(kUndefined, "not a number");
            return false;
        }
        if (state_)
            return emit_cell(OP_LIT) && emit_cell(v);
        push(v);
        return true;
    };
    auto require_compiling = [&]() {
        if (!state_) {
            fail(kCompileOnly, "compile-only word");
            return false;
        }
        return true;
    };
    auto define = [&](Kind kind, u32 param) {
        const char* w;
        u32 len;
        if (!parse_name(w, len))
            return -1;
        return add_word(w, len, kind, param);
    };

    Cell a, b, c;
    switch (id) {
    // --- stack
    case P_DUP: if (!need(1)) break; push(peek()); break;
    case P_DROP: pop(); break;
    case P_SWAP: if (!need(2)) break; a = pop(); b = pop(); push(a); push(b); break;
    case P_OVER: if (!need(2)) break; push(peek(1)); break;
    case P_ROT: if (!need(3)) break; c = pop(); b = pop(); a = pop(); push(b); push(c); push(a); break;
    case P_MROT: if (!need(3)) break; c = pop(); b = pop(); a = pop(); push(c); push(a); push(b); break;
    case P_NIP: if (!need(2)) break; a = pop(); pop(); push(a); break;
    case P_TUCK: if (!need(2)) break; a = pop(); b = pop(); push(a); push(b); push(a); break;
    case P_PICK: a = pop(); if (!need(a + 1)) break; push(peek(a)); break;
    case P_ROLL: {
        a = pop();
        if (!need(a + 1))
            break;
        const Cell v = stack_[sp_ - 1 - a];
        for (u32 i = sp_ - 1 - a; i + 1 < sp_; ++i)
            stack_[i] = stack_[i + 1];
        stack_[sp_ - 1] = v;
        break;
    }
    case P_QDUP: if (!need(1)) break; if (peek()) push(peek()); break;
    case P_2DUP: if (!need(2)) break; a = peek(1); b = peek(); push(a); push(b); break;
    case P_2DROP: pop(); pop(); break;
    case P_2SWAP: {
        if (!need(4)) break;
        const Cell d = pop(), cc = pop(), bb = pop(), aa = pop();
        push(cc); push(d); push(aa); push(bb);
        break;
    }
    case P_2OVER: if (!need(4)) break; a = peek(3); b = peek(2); push(a); push(b); break;
    case P_DEPTH: push(sp_); break;
    case P_TOR: rpush(pop()); break;
    case P_RFROM: push(rpop()); break;
    case P_RFETCH: if (rp_ == 0) { fail(kReturnUnderflow, "return stack underflow"); break; } push(rstack_[rp_ - 1]); break;
    case P_I: if (rp_ < 2) { fail(kReturnUnderflow, "i outside a loop"); break; } push(rstack_[rp_ - 1]); break;
    case P_J: if (rp_ < 4) { fail(kReturnUnderflow, "j outside two loops"); break; } push(rstack_[rp_ - 3]); break;
    case P_UNLOOP: if (rp_ < 2) { fail(kReturnUnderflow, "unloop outside a loop"); break; } rp_ -= 2; break;
    // --- arithmetic (division truncates toward zero, like C)
    case P_ADD: b = pop(); a = pop(); push(a + b); break;
    case P_SUB: b = pop(); a = pop(); push(a - b); break;
    case P_MUL: b = pop(); a = pop(); push(a * b); break;
    case P_DIV:
    case P_MOD:
    case P_DIVMOD: {
        b = pop();
        a = pop();
        if (error_)
            break;
        if (b == 0) {
            fail(kDivByZero, "division by zero");
            break;
        }
        // INT_MIN / -1 overflows in C: define it as the wrapped result.
        s32 q, r;
        if (sgn(a) == static_cast<s32>(0x80000000u) && sgn(b) == -1) {
            q = sgn(a);
            r = 0;
        } else {
            q = sgn(a) / sgn(b);
            r = sgn(a) % sgn(b);
        }
        if (id == P_DIV) push(static_cast<Cell>(q));
        else if (id == P_MOD) push(static_cast<Cell>(r));
        else { push(static_cast<Cell>(r)); push(static_cast<Cell>(q)); }
        break;
    }
    case P_NEGATE: push(0u - pop()); break;
    case P_ABS: a = pop(); push(sgn(a) < 0 ? 0u - a : a); break;
    case P_MIN: b = pop(); a = pop(); push(sgn(a) < sgn(b) ? a : b); break;
    case P_MAX: b = pop(); a = pop(); push(sgn(a) > sgn(b) ? a : b); break;
    case P_AND: b = pop(); a = pop(); push(a & b); break;
    case P_OR: b = pop(); a = pop(); push(a | b); break;
    case P_XOR: b = pop(); a = pop(); push(a ^ b); break;
    case P_INVERT: push(~pop()); break;
    case P_LSHIFT: b = pop(); a = pop(); push(b >= 32 ? 0 : a << b); break;
    case P_RSHIFT: b = pop(); a = pop(); push(b >= 32 ? 0 : a >> b); break;
    case P_1ADD: push(pop() + 1); break;
    case P_1SUB: push(pop() - 1); break;
    case P_2ADD: push(pop() + 2); break;
    case P_2SUB: push(pop() - 2); break;
    case P_2MUL: push(pop() << 1); break;
    case P_2DIV: push(static_cast<Cell>(sgn(pop()) >> 1)); break;
    case P_U2DIV: push(pop() >> 1); break;
    case P_EQ: b = pop(); a = pop(); push(flag(a == b)); break;
    case P_NE: b = pop(); a = pop(); push(flag(a != b)); break;
    case P_LT: b = pop(); a = pop(); push(flag(sgn(a) < sgn(b))); break;
    case P_GT: b = pop(); a = pop(); push(flag(sgn(a) > sgn(b))); break;
    case P_ULT: b = pop(); a = pop(); push(flag(a < b)); break;
    case P_UGT: b = pop(); a = pop(); push(flag(a > b)); break;
    case P_0EQ: push(flag(pop() == 0)); break;
    case P_0NE: push(flag(pop() != 0)); break;
    case P_0LT: push(flag(sgn(pop()) < 0)); break;
    case P_0GT: push(flag(sgn(pop()) > 0)); break;
    case P_LE: b = pop(); a = pop(); push(flag(sgn(a) <= sgn(b))); break;
    case P_GE: b = pop(); a = pop(); push(flag(sgn(a) >= sgn(b))); break;
    case P_WITHIN:  // ( n lo hi -- flag ): lo <= n < hi, unsigned-circular (ANS)
        c = pop(); b = pop(); a = pop(); push(flag(a - b < c - b)); break;
    case P_BETWEEN:  // ( n lo hi -- flag ): lo <= n <= hi (IEEE 1275)
        c = pop(); b = pop(); a = pop(); push(flag(a - b <= c - b)); break;
    case P_TRUE: push(0xFFFFFFFFu); break;
    case P_FALSE: push(0); break;
    case P_BL: push(' '); break;
    case P_CELLADD: push(pop() + 4); break;
    case P_CELLS: push(pop() * 4); break;
    case P_CHARADD: push(pop() + 1); break;
    case P_CHARS: break;
    case P_ALIGNED: push((pop() + 3) & ~3u); break;
    // --- memory
    case P_FETCH: case P_LFETCH: a = pop(); if (rd(a, 4, b)) push(b); break;
    case P_CFETCH: a = pop(); if (rd(a, 1, b)) push(b); break;
    case P_WFETCH: a = pop(); if (rd(a, 2, b)) push(b); break;
    case P_STORE: case P_LSTORE: a = pop(); b = pop(); if (!error_) wr(a, 4, b); break;
    case P_CSTORE: a = pop(); b = pop(); if (!error_) wr(a, 1, b); break;
    case P_WSTORE: a = pop(); b = pop(); if (!error_) wr(a, 2, b); break;
    case P_PSTORE: a = pop(); b = pop(); if (!error_ && rd(a, 4, c)) wr(a, 4, c + b); break;
    case P_HERE: push(here_); break;
    case P_ALLOT: a = pop(); if (!error_) allot(sgn(a)); break;
    case P_COMMA: case P_LCOMMA: a = pop(); if (!error_ && allot(4)) wr(here_ - 4, 4, a); break;
    case P_CCOMMA: a = pop(); if (!error_ && allot(1)) wr(here_ - 1, 1, a); break;
    case P_WCOMMA: a = pop(); if (!error_ && allot(2)) wr(here_ - 2, 2, a); break;
    case P_ALIGN: allot(static_cast<s32>(((here_ + 3) & ~3u) - here_)); break;
    case P_MOVE: {  // ( from to len ) — handles overlap
        c = pop(); b = pop(); a = pop();
        if (error_) break;
        if (b < a) {
            for (u32 i = 0; i < c && !error_; ++i) { Cell v; if (rd(a + i, 1, v)) wr(b + i, 1, v); }
        } else {
            for (u32 i = c; i-- > 0 && !error_;) { Cell v; if (rd(a + i, 1, v)) wr(b + i, 1, v); }
        }
        break;
    }
    case P_FILL: c = pop(); b = pop(); a = pop(); for (u32 i = 0; i < b && !error_; ++i) wr(a + i, 1, c); break;
    case P_COMP: {  // ( addr1 addr2 len -- n ): 0 equal, -1/1 by the first differing byte
        c = pop(); b = pop(); a = pop();
        s32 r = 0;
        for (u32 i = 0; i < c && !error_ && r == 0; ++i) {
            Cell x, y;
            if (rd(a + i, 1, x) && rd(b + i, 1, y) && x != y)
                r = x < y ? -1 : 1;
        }
        push(static_cast<Cell>(r));
        break;
    }
    case P_COUNT: a = pop(); if (rd(a, 1, b)) { push(a + 1); push(b); } break;
    // --- output
    case P_TYPE: {
        b = pop(); a = pop();
        char buf[64];
        for (u32 i = 0; i < b && !error_;) {
            u32 n = 0;
            for (; n < sizeof buf && i < b; ++n, ++i) { Cell v; if (!rd(a + i, 1, v)) break; buf[n] = static_cast<char>(v); }
            emit_text(buf, n);
        }
        break;
    }
    case P_EMIT: { const char ch = static_cast<char>(pop()); if (!error_) emit_text(&ch, 1); break; }
    case P_CR: emit_text("\n", 1); break;
    case P_SPACE: emit_text(" ", 1); break;
    case P_SPACES: a = pop(); for (u32 i = 0; i < a && i < 1024; ++i) emit_text(" ", 1); break;
    case P_DOT: a = pop(); if (!error_) { print_number(a, true); emit_text(" ", 1); } break;
    case P_UDOT: a = pop(); if (!error_) { print_number(a, false); emit_text(" ", 1); } break;
    case P_DOTR: b = pop(); a = pop(); if (!error_) print_number(a, true, b); break;
    case P_UDOTR: b = pop(); a = pop(); if (!error_) print_number(a, false, b); break;
    case P_DOTD: case P_DOTH: {
        a = pop();
        if (error_) break;
        const Cell saved = base();
        set_base(id == P_DOTD ? 10 : 16);
        print_number(a, id == P_DOTD);
        emit_text(" ", 1);
        set_base(saved);
        break;
    }
    case P_DOTS: {
        emit_text("<", 1);
        const Cell saved = base();
        set_base(10);
        print_number(sp_, false);
        set_base(saved);
        emit_text("> ", 2);
        for (u32 i = 0; i < sp_; ++i) { print_number(stack_[i], true); emit_text(" ", 1); }
        break;
    }
    // --- base
    case P_BASE: push(base_addr_); break;
    case P_DECIMAL: set_base(10); break;
    case P_HEX: set_base(16); break;
    case P_OCTAL: set_base(8); break;
    case P_BINARY: set_base(2); break;
    // --- control
    case P_EXECUTE:
        a = pop();
        if (error_) break;
        if (a < kXtBase || a - kXtBase >= word_count_) { fail(kUndefined, "invalid execution token"); break; }
        execute(a - kXtBase);
        break;
    case P_EVALUATE: {
        b = pop(); a = pop();
        if (error_) break;
        if (src_depth_ >= 1 + sizeof eval_buf_ / sizeof eval_buf_[0] || b >= sizeof eval_buf_[0]) {
            fail(-8, "evaluate: too deep or too long");
            break;
        }
        char* buf = eval_buf_[src_depth_ - 1];
        if (!read_string(a, b, buf, sizeof eval_buf_[0]))
            break;
        src_[src_depth_++] = {buf, b, 0};
        const bool ok = interpret_source();
        --src_depth_;
        (void)ok;
        break;
    }
    case P_THROW: a = pop(); if (!error_ && a != 0) fail(sgn(a), "throw"); break;
    case P_CATCH: {
        a = pop();
        if (error_) break;
        if (a < kXtBase || a - kXtBase >= word_count_) { fail(kUndefined, "invalid execution token"); break; }
        const u32 saved_sp = sp_, saved_rp = rp_, saved_src = src_depth_, saved_calls = call_depth_;
        ++catch_depth_;
        const bool ok = execute(a - kXtBase);
        --catch_depth_;
        if (!ok && error_) {
            const s32 code = error_code_;
            error_ = false;
            error_code_ = 0;
            error_text_[0] = '\0';
            sp_ = saved_sp;  // ANS: the stack depth from before the catch (without xt)
            rp_ = saved_rp;
            src_depth_ = saved_src;
            call_depth_ = saved_calls;
            push(static_cast<Cell>(code));
        } else {
            push(0);
        }
        break;
    }
    case P_ABORT: fail(kAbort, "abort"); break;
    // --- defining and compiling
    case P_COLON: {
        if (state_) { fail(kControlMismatch, "nested :"); break; }
        const s32 w = define(Kind::Colon, code_used_);
        if (w < 0) break;
        words_[w].hidden = true;
        defining_ = w;
        state_ = true;
        ctl_push(Ctl::Colon, 0);
        break;
    }
    case P_SEMI: {
        if (!require_compiling()) break;
        u32 dummy;
        if (!ctl_pop(Ctl::Colon, dummy)) break;
        if (!emit_cell(OP_EXIT)) break;
        words_[defining_].hidden = false;
        defining_ = -1;
        state_ = false;
        break;
    }
    case P_IF: if (!require_compiling()) break; emit_cell(OP_0BRANCH); ctl_push(Ctl::Orig, code_used_); emit_cell(0); break;
    case P_ELSE: {
        if (!require_compiling()) break;
        u32 orig;
        if (!ctl_pop(Ctl::Orig, orig)) break;
        emit_cell(OP_BRANCH);
        ctl_push(Ctl::Orig, code_used_);
        emit_cell(0);
        code_[orig] = code_used_;
        break;
    }
    case P_THEN: { if (!require_compiling()) break; u32 orig; if (ctl_pop(Ctl::Orig, orig)) code_[orig] = code_used_; break; }
    case P_BEGIN: if (!require_compiling()) break; ctl_push(Ctl::Dest, code_used_); break;
    case P_UNTIL: { if (!require_compiling()) break; u32 dest; if (ctl_pop(Ctl::Dest, dest)) { emit_cell(OP_0BRANCH); emit_cell(dest); } break; }
    case P_AGAIN: { if (!require_compiling()) break; u32 dest; if (ctl_pop(Ctl::Dest, dest)) { emit_cell(OP_BRANCH); emit_cell(dest); } break; }
    case P_WHILE: {
        if (!require_compiling()) break;
        u32 dest;
        if (!ctl_pop(Ctl::Dest, dest)) break;
        emit_cell(OP_0BRANCH);
        ctl_push(Ctl::Orig, code_used_);
        emit_cell(0);
        ctl_push(Ctl::Dest, dest);
        break;
    }
    case P_REPEAT: {
        if (!require_compiling()) break;
        u32 dest, orig;
        if (!ctl_pop(Ctl::Dest, dest) || !ctl_pop(Ctl::Orig, orig)) break;
        emit_cell(OP_BRANCH);
        emit_cell(dest);
        code_[orig] = code_used_;
        break;
    }
    case P_DO:
    case P_QDO: {
        if (!require_compiling()) break;
        const u32 leave_base = leave_sp_;
        if (id == P_QDO) {
            emit_cell(OP_QDO);
            if (leave_sp_ >= sizeof leave_ / sizeof leave_[0]) { fail(kControlMismatch, "too many leaves"); break; }
            leave_[leave_sp_++] = code_used_;
            emit_cell(0);
        } else {
            emit_cell(OP_DO);
        }
        ctl_push(Ctl::Do, leave_base);
        ctl_push(Ctl::Dest, code_used_);
        break;
    }
    case P_LOOP:
    case P_PLOOP: {
        if (!require_compiling()) break;
        u32 dest, leave_base;
        if (!ctl_pop(Ctl::Dest, dest) || !ctl_pop(Ctl::Do, leave_base)) break;
        emit_cell(id == P_LOOP ? OP_LOOP : OP_PLOOP);
        emit_cell(dest);
        while (leave_sp_ > leave_base)
            code_[leave_[--leave_sp_]] = code_used_;
        break;
    }
    case P_LEAVE: {
        if (!require_compiling()) break;
        bool in_loop = false;
        for (u32 i = 0; i < ctl_sp_; ++i)
            in_loop |= ctl_[i].kind == Ctl::Do;
        if (!in_loop || leave_sp_ >= sizeof leave_ / sizeof leave_[0]) { fail(kControlMismatch, "leave outside a loop"); break; }
        emit_cell(OP_LEAVE);
        leave_[leave_sp_++] = code_used_;
        emit_cell(0);
        break;
    }
    case P_RECURSE: if (require_compiling()) emit_cell(static_cast<u32>(defining_)); break;
    case P_EXIT: if (require_compiling()) emit_cell(OP_EXIT); break;
    case P_LITERAL: if (!require_compiling()) break; a = pop(); if (!error_) { emit_cell(OP_LIT); emit_cell(a); } break;
    case P_LBRACKET: state_ = false; break;
    case P_RBRACKET: state_ = true; break;
    case P_TICK:
    case P_BRACKET_TICK: {
        const char* w;
        u32 len;
        if (!parse_name(w, len)) break;
        const s32 idx = find(w, len);
        if (idx < 0) { fail(kUndefined, "undefined word"); break; }
        if (id == P_BRACKET_TICK && state_) { emit_cell(OP_LIT); emit_cell(kXtBase + static_cast<u32>(idx)); }
        else push(kXtBase + static_cast<u32>(idx));
        break;
    }
    case P_PAREN: { const char* s; u32 len; parse_until(')', s, len); break; }
    case P_BACKSLASH: { const char* s; u32 len; parse_until('\n', s, len); break; }
    case P_DOTPAREN: { const char* s; u32 len; parse_until(')', s, len); emit_text(s, len); break; }
    case P_DOTQUOTE:
    case P_SQUOTE:
    case P_ABORTQ: {
        const char* s;
        u32 len;
        parse_until('"', s, len);
        if (len > kStringBuffer) { fail(-18, "string too long"); break; }
        if (id == P_ABORTQ) {
            if (!require_compiling()) break;
            compile_string(OP_ABORTQ, s, len);
        } else if (state_) {
            compile_string(id == P_DOTQUOTE ? OP_PRINT : OP_STR, s, len);
        } else if (id == P_DOTQUOTE) {
            emit_text(s, len);
        } else {
            push(transient_string(s, len));
            push(len);
        }
        break;
    }
    case P_QUOTE: {
        char buf[kStringBuffer];
        u32 len = 0;
        if (!parse_quoted(buf, sizeof buf, len)) { fail(-18, "string too long"); break; }
        if (state_) {
            compile_string(OP_STR, buf, len);
        } else {
            push(transient_string(buf, len));
            push(len);
        }
        break;
    }
    case P_CHAR:
    case P_BRACKET_CHAR: {
        const char* w;
        u32 len;
        if (!parse_name(w, len)) break;
        const Cell ch = static_cast<u8>(w[0]);
        if (id == P_BRACKET_CHAR && state_) { emit_cell(OP_LIT); emit_cell(ch); }
        else push(ch);
        break;
    }
    case P_VARIABLE: {
        const u32 addr = here_;
        if (!allot(4)) break;
        mem_.write32(addr, 0);
        define(Kind::Variable, addr);
        break;
    }
    case P_CONSTANT: a = pop(); if (!error_) define(Kind::Constant, a); break;
    case P_VALUE: {
        a = pop();
        if (error_) break;
        const u32 addr = here_;
        if (!allot(4)) break;
        mem_.write32(addr, a);
        define(Kind::Value, addr);
        break;
    }
    case P_DEFER: {
        const u32 addr = here_;
        if (!allot(4)) break;
        mem_.write32(addr, 0);
        define(Kind::Defer, addr);
        break;
    }
    case P_TO:
    case P_IS: {
        const char* w;
        u32 len;
        if (!parse_name(w, len)) break;
        const s32 idx = find(w, len);
        if (idx < 0 || (words_[idx].kind != Kind::Value && words_[idx].kind != Kind::Defer)) {
            fail(kUndefined, "to/is needs a value or defer");
            break;
        }
        if (state_) {
            emit_cell(OP_TOVAL);
            emit_cell(words_[idx].param);
        } else {
            a = pop();
            if (!error_) mem_.write32(words_[idx].param, a);
        }
        break;
    }
    case P_CREATE: {
        const s32 w = define(Kind::Create, here_);
        if (w >= 0) latest_create_ = w;
        break;
    }
    case P_DOES: if (require_compiling()) emit_cell(OP_DOES); break;
    case P_IMMEDIATE: if (word_count_) words_[word_count_ - 1].immediate = true; break;
    case P_CASE: if (require_compiling()) ctl_push(Ctl::Case, 0); break;
    case P_OF: if (!require_compiling()) break; emit_cell(OP_OF); ctl_push(Ctl::Of, code_used_); emit_cell(0); break;
    case P_ENDOF: {
        if (!require_compiling()) break;
        u32 of_at;
        if (!ctl_pop(Ctl::Of, of_at)) break;
        emit_cell(OP_BRANCH);
        ctl_push(Ctl::Orig, code_used_);
        emit_cell(0);
        code_[of_at] = code_used_;
        break;
    }
    case P_ENDCASE: {
        if (!require_compiling()) break;
        emit_cell(OP_DROP);
        while (ctl_sp_ > 0 && ctl_[ctl_sp_ - 1].kind == Ctl::Orig)
            code_[ctl_[--ctl_sp_].at] = code_used_;
        u32 dummy;
        ctl_pop(Ctl::Case, dummy);
        break;
    }
    case P_DNUM: number_in(10); break;
    case P_HNUM: number_in(16); break;
    case P_ONUM: number_in(8); break;
    case P_BNUM: number_in(2); break;
    default:
        fail(kUndefined, "unknown primitive");
        break;
    }
    return !error_;
}

} // namespace g3::forth
