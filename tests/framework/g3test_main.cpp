// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/framework/g3test_main.cpp — test registry and runner for g3test.h.

#include "tests/framework/g3test.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace g3test {
namespace {

struct TestEntry {
    const char* name;
    TestFn fn;
    const char* file;
    int line;
};

constexpr int kMaxTests = 8192;
TestEntry g_tests[kMaxTests];
int g_test_count = 0;
bool g_current_failed = false;
int g_failure_count = 0;
const char* g_current_name = "";

} // namespace

Registrar::Registrar(const char* name, TestFn fn, const char* file, int line)
{
    if (g_test_count >= kMaxTests) {
        std::fprintf(stderr, "g3test: too many tests (max %d)\n", kMaxTests);
        return;
    }
    g_tests[g_test_count++] = TestEntry{name, fn, file, line};
}

void report_failure(const char* file, int line, const char* expr, const char* detail)
{
    g_current_failed = true;
    ++g_failure_count;
    std::fprintf(stderr, "  FAIL [%s] %s:%d: %s%s%s\n", g_current_name, file, line, expr,
                 detail ? " — " : "", detail ? detail : "");
}

void report_eq_failure(const char* file, int line, const char* a_expr, const char* b_expr,
                       uint64_t a, uint64_t b)
{
    g_current_failed = true;
    ++g_failure_count;
    std::fprintf(stderr,
                 "  FAIL [%s] %s:%d: CHECK_EQ(%s, %s)\n"
                 "       left  = %" PRIu64 " (0x%" PRIX64 ")\n"
                 "       right = %" PRIu64 " (0x%" PRIX64 ")\n",
                 g_current_name, file, line, a_expr, b_expr, a, a, b, b);
}

bool current_test_failed() { return g_current_failed; }

} // namespace g3test

int main(int argc, char** argv)
{
    using namespace g3test;
    const char* filter = nullptr;
    bool list_only = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list") == 0)
            list_only = true;
        else
            filter = argv[i];
    }

    int run = 0, failed = 0;
    for (int i = 0; i < g_test_count; ++i) {
        const TestEntry& t = g_tests[i];
        if (filter && std::strncmp(t.name, filter, std::strlen(filter)) != 0)
            continue;
        if (list_only) {
            std::printf("%s\n", t.name);
            continue;
        }
        g_current_name = t.name;
        g_current_failed = false;
        t.fn();
        ++run;
        if (g_current_failed) {
            ++failed;
            std::printf("FAILED  %s  (%s:%d)\n", t.name, t.file, t.line);
        }
    }
    if (list_only)
        return 0;

    std::printf("\n%d test(s) run, %d passed, %d failed, %d failed check(s)\n", run,
                run - failed, failed, g_failure_count);
    if (run == 0) {
        std::printf("No tests matched.\n");
        return 2;
    }
    return failed == 0 ? 0 : 1;
}
