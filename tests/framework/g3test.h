// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/framework/g3test.h — a minimal, dependency-free unit-test framework.
//
// Host-only (uses the C standard library). Usage:
//
//   #include "tests/framework/g3test.h"
//   TEST_CASE("endian/byteswap32") { CHECK_EQ(g3::byteswap32(0x12345678u), 0x78563412u); }
//
// Run all tests:          ./g3_unit_tests
// Run a subset by prefix: ./g3_unit_tests endian/
// List tests:             ./g3_unit_tests --list

#ifndef G3_TESTS_FRAMEWORK_G3TEST_H
#define G3_TESTS_FRAMEWORK_G3TEST_H

#include <stdint.h>

namespace g3test {

using TestFn = void (*)();

struct Registrar {
    Registrar(const char* name, TestFn fn, const char* file, int line);
};

// Records a failure for the currently running test.
void report_failure(const char* file, int line, const char* expr, const char* detail);
// Formats two integers as decimal and hex for failure messages.
void report_eq_failure(const char* file, int line, const char* a_expr, const char* b_expr,
                       uint64_t a, uint64_t b);
bool current_test_failed();

} // namespace g3test

#define G3TEST_CAT2(a, b) a##b
#define G3TEST_CAT(a, b) G3TEST_CAT2(a, b)

#define G3TEST_CASE_IMPL(name, id)                                                       \
    static void G3TEST_CAT(g3test_fn_, id)();                                            \
    static ::g3test::Registrar G3TEST_CAT(g3test_reg_, id)(                              \
        name, &G3TEST_CAT(g3test_fn_, id), __FILE__, __LINE__);                          \
    static void G3TEST_CAT(g3test_fn_, id)()

#define TEST_CASE(name) G3TEST_CASE_IMPL(name, __COUNTER__)

// Non-fatal boolean check.
#define CHECK(expr)                                                                      \
    do {                                                                                 \
        if (!(expr)) ::g3test::report_failure(__FILE__, __LINE__, #expr, nullptr);       \
    } while (0)

// Non-fatal integer equality check; prints both values in decimal and hex.
#define CHECK_EQ(a, b)                                                                   \
    do {                                                                                 \
        const auto g3test_a = (a);                                                       \
        const auto g3test_b = (b);                                                       \
        if (!(g3test_a == g3test_b))                                                     \
            ::g3test::report_eq_failure(__FILE__, __LINE__, #a, #b,                      \
                                        static_cast<uint64_t>(g3test_a),                 \
                                        static_cast<uint64_t>(g3test_b));                \
    } while (0)

// Fatal check: stops the current test on failure.
#define REQUIRE(expr)                                                                    \
    do {                                                                                 \
        if (!(expr)) {                                                                   \
            ::g3test::report_failure(__FILE__, __LINE__, #expr, "REQUIRE failed");       \
            return;                                                                      \
        }                                                                                \
    } while (0)

#endif // G3_TESTS_FRAMEWORK_G3TEST_H
