// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the Forth interpreter (ANS Forth core + IEEE 1275 differences).

#include "core/firmware/forth/forth.h"
#include "core/mem/physical_bus.h"
#include "tests/framework/g3test.h"

#include <memory>
#include <string>
#include <vector>

using namespace g3;
using namespace g3::forth;

namespace {

struct Rig {
    std::vector<u8> ram = std::vector<u8>(0x10000);
    PhysicalBus bus;
    std::unique_ptr<Forth> f;
    std::string out;
    Rig()
    {
        bus.map_ram(0, ram.data(), static_cast<u32>(ram.size()));
        f = std::make_unique<Forth>(bus, 0x8000, 0x4000);
        f->set_output([](void* c, const char* t, u32 n) { static_cast<std::string*>(c)->append(t, n); }, &out);
    }
    bool run(const std::string& s) { return f->interpret(s.c_str(), static_cast<u32>(s.size())); }
    // Runs and returns the stack (bottom first), clearing it.
    std::vector<s32> stack(const std::string& s)
    {
        std::vector<s32> v;
        if (!run(s)) {
            v.push_back(0x7EADBEEF);  // marks a failed run
            return v;
        }
        while (f->depth())
            v.insert(v.begin(), static_cast<s32>(f->pop()));
        return v;
    }
    std::string output(const std::string& s)
    {
        out.clear();
        run(s);
        return out;
    }
};

using V = std::vector<s32>;

} // namespace

TEST_CASE("forth/numbers_and_arithmetic")
{
    Rig r;
    CHECK(r.stack("10 20 +") == V({0x30}));                  // IEEE 1275: initial base is 16
    CHECK(r.stack("decimal 10 20 +") == V({30}));
    CHECK(r.stack("7 3 - 6 * 4 /") == V({6}));
    CHECK(r.stack("-7 2 / -7 2 mod") == V({-3, -1}));         // truncating division (documented)
    CHECK(r.stack("7 2 /mod") == V({1, 3}));
    CHECK(r.stack("-5 abs 3 negate 4 9 min 4 9 max") == V({5, -3, 4, 9}));
    CHECK(r.stack("hex FF ff -1 0") == V({255, 255, -1, 0})); // digits are case-insensitive
    CHECK(r.stack("d# 100 h# 10 o# 17 b# 101") == V({100, 16, 15, 5}));
    CHECK(r.stack("decimal 1 4 lshift 256 2 rshift 7 1 << 8 1 >>") == V({16, 64, 14, 4}));
    CHECK(r.stack("-8 2/ -8 u2/ 0 invert") == V({-4, 0x7FFFFFFC, -1}));
    CHECK(r.stack("5 3 and 5 3 or 5 3 xor") == V({1, 7, 6}));
    CHECK(r.stack("1 1+ 1 1- 1 2+ 1 2- 3 2* 4 cells 4 cell+") == V({2, 0, 3, -1, 6, 16, 8}));
    CHECK(!r.run("7 0 /"));
    CHECK_EQ(r.f->error_code(), Forth::kDivByZero);
    CHECK_EQ(r.f->depth(), 0u);                              // stacks reset after an error
    CHECK(r.stack("-2147483648 -1 /") == V({static_cast<s32>(0x80000000u)}));  // defined, no trap
}

TEST_CASE("forth/stack_words_and_comparisons")
{
    Rig r;
    r.run("decimal");
    CHECK(r.stack("1 2 3 rot") == V({2, 3, 1}));
    CHECK(r.stack("1 2 3 -rot") == V({3, 1, 2}));
    CHECK(r.stack("1 2 swap 3 over") == V({2, 1, 3, 1}));
    CHECK(r.stack("1 2 nip 3 4 tuck") == V({2, 4, 3, 4}));
    CHECK(r.stack("10 20 30 2 pick") == V({10, 20, 30, 10}));
    CHECK(r.stack("10 20 30 2 roll") == V({20, 30, 10}));
    CHECK(r.stack("0 ?dup 5 ?dup") == V({0, 5, 5}));
    CHECK(r.stack("1 2 3 4 2swap") == V({3, 4, 1, 2}));
    CHECK(r.stack("1 2 3 4 2over") == V({1, 2, 3, 4, 1, 2}));
    CHECK(r.stack("1 2 2dup 2drop depth") == V({1, 2, 2}));
    CHECK(r.stack("1 >r 2 r@ r> ") == V({2, 1, 1}));
    CHECK(r.stack("1 2 = 2 2 = 1 2 <> -1 1 < -1 1 u<") == V({0, -1, -1, -1, 0}));
    CHECK(r.stack("0 0= 5 0= -3 0< 3 0> 3 0<> 2 2 <= 3 2 >=") == V({-1, 0, -1, -1, -1, -1, -1}));
    CHECK(r.stack("5 1 10 within 10 1 10 within 10 1 10 between") == V({-1, 0, -1}));
    CHECK(r.stack("true false bl") == V({-1, 0, 32}));
    CHECK(!r.run("drop"));
    CHECK_EQ(r.f->error_code(), Forth::kStackUnderflow);
    CHECK(!r.run("r>"));
    CHECK_EQ(r.f->error_code(), Forth::kReturnUnderflow);
}

TEST_CASE("forth/memory_is_guest_memory")
{
    Rig r;
    r.run("decimal");
    CHECK(r.stack("variable v 1234 v ! v @") == V({1234}));
    // The variable lives in guest RAM, big-endian.
    const u32 addr = static_cast<u32>(r.stack("v")[0]);
    CHECK(addr >= 0x8000u && addr < 0xC000u);
    CHECK_EQ(r.ram[addr + 2], 0x04u);                        // 1234 = 0x000004D2
    CHECK_EQ(r.ram[addr + 3], 0xD2u);
    CHECK(r.stack("5 v +! v @") == V({1239}));
    CHECK(r.stack("hex 1234 100 w! 100 c@ 101 c@ 100 w@") == V({0x12, 0x34, 0x1234}));
    CHECK(r.stack("11223344 200 l! 200 l@ 203 c@") == V({0x11223344, 0x44}));
    r.run("decimal");
    CHECK(r.stack("here 4 allot here swap -") == V({4}));
    CHECK(r.stack("here 7 , @ here 3 c, c@") == V({7, 3}));
    // move with overlap, fill, comp, count
    r.run("hex 41 300 c! 42 301 c! 43 302 c! 300 301 2 move 300 c@ 301 c@ 302 c@");
    CHECK(r.stack("") == V({0x41, 0x41, 0x42}));
    CHECK(r.stack("400 3 5A fill 401 c@ 300 300 3 comp 300 400 1 comp") == V({0x5A, 0, -1}));
    r.ram[0x500] = 2; r.ram[0x501] = 'h'; r.ram[0x502] = 'i';
    CHECK(r.output("500 count type") == "hi");
    CHECK(!r.run("hex 7FFFFFF0 @"));                         // nothing mapped there
    CHECK_EQ(r.f->error_code(), Forth::kInvalidAddress);
}

TEST_CASE("forth/colon_definitions_and_control_flow")
{
    Rig r;
    r.run("decimal");
    CHECK(r.stack(": sq dup * ; 7 sq") == V({49}));
    CHECK(r.stack(": sign dup 0< if drop -1 else 0> if 1 else 0 then then ; -5 sign 0 sign 9 sign") ==
          V({-1, 0, 1}));
    CHECK(r.stack(": cnt 0 begin 1+ dup 5 = until ; cnt") == V({5}));
    CHECK(r.stack(": w 0 begin dup 3 < while 1+ repeat ; w") == V({3}));
    CHECK(r.stack(": sum 0 10 0 do i + loop ; sum") == V({45}));
    CHECK(r.stack(": evens 10 0 do i 2 +loop ; evens") == V({0, 2, 4, 6, 8}));
    CHECK(r.stack(": down 0 10 do i -2 +loop ; down") == V({10, 8, 6, 4, 2, 0}));  // ANS +loop
    CHECK(r.stack(": q 5 5 ?do i loop 7 ; q") == V({7}));      // ?do with equal bounds: no pass
    CHECK(r.stack(": lv 100 0 do i dup 3 = if leave then drop loop ; lv") == V({3}));
    CHECK(r.stack(": nest 3 0 do 2 0 do j i loop loop ; nest") == V({0, 0, 0, 1, 1, 0, 1, 1, 2, 0, 2, 1}));
    CHECK(r.stack(": fact dup 1 > if dup 1- recurse * then ; 6 fact") == V({720}));
    CHECK(r.stack(": early 1 exit 2 ; early") == V({1}));
    CHECK(r.stack(": cls case 1 of 10 endof 2 of 20 endof 99 swap endcase ; 1 cls 2 cls 3 cls") ==
          V({10, 20, 99}));
    CHECK(r.stack(": inner 3 0 do i loop ; : outer inner + + ; outer") == V({3}));
    CHECK(r.stack(": x 1 ; : x x 1+ ; x") == V({2}));         // a word is hidden while defined
    CHECK(r.stack(": lit [ 2 3 * ] literal ; lit") == V({6}));
    CHECK(r.stack("DUP") == V({0x7EADBEEF}));                  // underflow marker: DUP found, case-insensitive
    CHECK(r.stack("4 DuP") == V({4, 4}));
    // A definition can span several interpret calls (lines of a script).
    CHECK(r.run(": two"));
    CHECK(r.f->compiling());
    CHECK(r.run("  2 ;"));
    CHECK(r.stack("two") == V({2}));
}

TEST_CASE("forth/defining_words")
{
    Rig r;
    r.run("decimal");
    CHECK(r.stack("42 constant k k") == V({42}));
    CHECK(r.stack("5 value x x 7 to x x") == V({5, 7}));
    CHECK(r.stack(": setx to x ; 9 setx x") == V({9}));        // to compiled
    CHECK(r.stack(": konst create , does> @ ; 17 konst seventeen seventeen") == V({17}));
    CHECK(r.stack("create buf 3 , buf @") == V({3}));
    CHECK(r.stack("defer act ' dup is act 4 act") == V({4, 4}));
    CHECK(r.stack(": setact is act ; ' drop setact 1 2 act") == V({1}));
    CHECK(r.stack(": now 99 ; immediate : user now ; user") == V({99}));
    CHECK(r.stack(": sq dup * ; 3 ' sq execute : tk ['] sq ; 4 tk execute") == V({9, 16}));
    CHECK(r.stack("char A [char] Z") == V({'A', 'Z'}));            // [char] interprets like char
    CHECK(!r.run("defer nothing nothing"));                  // unset defer
}

TEST_CASE("forth/strings_and_output")
{
    Rig r;
    CHECK(r.output(".\" hello\" ") == "hello");
    CHECK(r.output(": hi .\" hi there\" ; hi") == "hi there");
    CHECK(r.output("s\" abc\" type") == "abc");
    CHECK(r.output(": t s\" xyz\" type ; t") == "xyz");
    CHECK(r.output("\" a\"(41 42)b\" type") == "aABb");       // IEEE 1275 hex escapes
    CHECK(r.output("\" q\"\"q\" type") == "q\"q");
    CHECK(r.output(": t2 \" in\"(21)\" type ; t2") == "in!");
    CHECK(r.output("decimal -5 . 42 u. hex ff .") == "-5 42 ff ");
    CHECK(r.output("decimal 5 4 .r 7 3 u.r") == "   5  7");
    CHECK(r.output("decimal 1 2 3 .s") == "<3> 1 2 3 ");
    r.run("decimal");
    while (r.f->depth()) r.f->pop();
    CHECK(r.output("255 .h 16 .d") == "ff 16 ");
    CHECK(r.output("decimal 65 emit cr space 2 spaces") == "A\n   ");
    CHECK(r.output("( a comment ) .( shown) \\ ignored to the end\n") == "shown");
    CHECK(r.stack("s\" 1 2 +\" evaluate") == V({3}));
    CHECK(r.output("decimal 5 binary . decimal 9 octal . decimal") == "101 11 ");
}

TEST_CASE("forth/errors_catch_throw")
{
    Rig r;
    r.run("decimal");
    CHECK(!r.run("1 2 frobnicate"));
    CHECK_EQ(r.f->error_code(), Forth::kUndefined);
    CHECK(std::string(r.f->error_text()) == "frobnicate ?");
    CHECK_EQ(r.f->depth(), 0u);
    // A failed definition is dropped.
    CHECK(!r.run(": bad nonsense ;"));
    CHECK(!r.f->compiling());
    CHECK(!r.run("bad"));
    CHECK_EQ(r.f->error_code(), Forth::kUndefined);
    CHECK(r.stack(": t5 5 throw ; 1 ' t5 catch") == V({1, 5}));
    CHECK(r.stack("1 ' dup catch") == V({1, 1, 0}));
    CHECK(r.stack(": t0 0 throw 7 ; ' t0 catch") == V({7, 0}));
    CHECK(r.stack(": deep 1 2 3 99 throw ; 10 ' deep catch") == V({10, 99}));  // depth restored
    CHECK(r.stack(": uf drop drop drop ; ' uf catch") == V({Forth::kStackUnderflow}));
    CHECK(r.stack(": ab abort\" boom\" ; 0 ab 5") == V({5}));
    r.out.clear();
    CHECK(!r.run("1 ab"));
    CHECK_EQ(r.f->error_code(), Forth::kAbortQuote);
    CHECK(r.out == "boom");
    CHECK(!r.run(": m if ;"));                               // unresolved if
    CHECK_EQ(r.f->error_code(), Forth::kControlMismatch);
    CHECK(!r.run("leave"));                                  // compile-only (if is not: IEEE 1275)
    CHECK_EQ(r.f->error_code(), Forth::kCompileOnly);
    CHECK(!r.run(": r recurse ; r"));                        // runaway recursion is caught
    CHECK(!r.run("abort"));
    CHECK_EQ(r.f->error_code(), Forth::kAbort);
    CHECK(r.stack("3 4 +") == V({7}));                          // still usable afterwards
}

TEST_CASE("forth/native_words")
{
    Rig r;
    int calls = 0;
    struct Ctx { int* calls; } ctx{&calls};
    r.f->define_native("triple", [](Forth& f, void* c) {
        ++*static_cast<Ctx*>(c)->calls;
        f.push(f.pop() * 3);
    }, &ctx);
    r.f->define_constant("load-base", 0x4000);
    r.run("decimal");
    CHECK(r.stack("5 triple : t9 3 triple ; t9") == V({15, 9}));
    CHECK_EQ(calls, 2);
    CHECK(r.stack("load-base") == V({0x4000}));
    r.f->define_native("fails", [](Forth& f, void*) { f.fail(-21, "unsupported"); }, nullptr);
    CHECK(r.stack("' fails catch") == V({-21}));
}

TEST_CASE("forth/interpreted_control_structures")
{
    // IEEE 1275 lets control structures be typed outside definitions (Apple's
    // Mac OS ROM boot script relies on it): they run once complete.
    Rig r;
    CHECK(r.stack("1 if 11 else 22 then") == V({0x11}));
    CHECK(r.stack("0 if 11 else 22 then") == V({0x22}));
    CHECK(r.stack("0 5 0 do i + loop") == V({0 + 1 + 2 + 3 + 4}));
    CHECK(r.stack("0 0 0 ?do 99 loop") == V({0}));                 // ?do with an empty range
    CHECK(r.stack("3 begin dup while 1- repeat") == V({0}));
    CHECK(r.stack("0 begin 1+ dup 4 = until") == V({4}));
    CHECK(r.stack("2 case 1 of 10 endof 2 of 20 endof 30 swap endcase") == V({0x20}));
    // Nested structures run once, when the outermost one closes.
    CHECK(r.stack("1 if 0 3 0 do i 1 = if 1+ then loop then") == V({1}));
    // A structure may span several lines (separate interpret calls), as in a boot script.
    REQUIRE(r.run("5 0"));
    REQUIRE(r.run("begin"));
    REQUIRE(r.run(" over while"));
    REQUIRE(r.run(" 1+ swap 1- swap"));
    REQUIRE(r.run("repeat"));
    CHECK(r.stack("nip") == V({5}));
    // The temporary code is discarded: a boot script's many structures don't
    // fill the code space (32768 cells; each of these takes about 8).
    bool all_ran = true;
    for (int i = 0; i < 6000 && all_ran; ++i)
        all_ran = r.run("1 if 2 drop then");
    CHECK(all_ran);
    // Nothing is left behind: a definition made afterwards works normally.
    CHECK(r.stack(": t 1 if 7 then ; t") == V({7}));
    // An error inside a half-typed structure discards it; later input is fine.
    CHECK(!r.run("1 if undefined-word-xyz"));
    CHECK(r.stack("1 if 3 then") == V({3}));
    // Words that only make sense inside a definition still say so, by name.
    CHECK(!r.run("exit"));
    CHECK(std::string(r.f->error_text()).find("exit") != std::string::npos);
}

TEST_CASE("forth/interpreted_abort_quote_and_string_words")
{
    Rig r;
    CHECK(r.stack("0 abort\" not shown\" 5") == V({5}));
    CHECK(r.output("1 abort\" stop here\" 5") == "stop here");
    CHECK(!r.run("1 abort\" again\""));
    CHECK(r.stack("\" abc\" \" abc\" $=") == V({-1}));
    CHECK(r.stack("\" abc\" \" abd\" $=") == V({0}));
    CHECK(r.stack("\" abc\" \" ab\" $=") == V({0}));
    CHECK(r.stack("100 20 bounds") == V({0x120, 0x100}));
    CHECK(r.stack("1 2 3 4 3drop") == V({1}));
    // $find: ( name len -- xt true | name len false )
    CHECK(r.stack(": sq dup * ; \" sq\" $find swap 7 swap execute") == V({-1, 0x31}));
    const V miss = r.stack("\" no-such-word\" $find");
    REQUIRE(miss.size() == 3);
    CHECK_EQ(miss[1], 12);
    CHECK_EQ(miss[2], 0);
}

TEST_CASE("forth/stop_ends_interpretation_without_an_error")
{
    // Open Firmware's `go` doesn't return: a native word calls stop(), and
    // nothing after it runs — on the same line, in the definition that
    // called it, or in an interpreted structure — yet it isn't an error.
    Rig r;
    REQUIRE(r.f->define_native("halt", [](Forth& f, void*) { f.stop(); }, nullptr));
    CHECK(r.output("1 . halt 2 .") == "1 ");
    CHECK(r.stack("") == V({}));
    CHECK(r.output(": t 3 . halt 4 . ; t 5 .") == "3 ");
    CHECK(r.output("1 if 6 . halt 7 . then 8 .") == "6 ");
    CHECK(r.output("7 halt 9 .") == "");
    CHECK(r.stack("") == V({7}));                                // the data stack is kept
    // The next input is interpreted normally.
    CHECK(r.output("9 .") == "9 ");
    CHECK(r.stack("1 if 2 then") == V({2}));
}
