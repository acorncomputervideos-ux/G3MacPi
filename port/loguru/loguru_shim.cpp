// SPDX-License-Identifier: GPL-3.0-or-later
//
// port/loguru/loguru_shim.cpp — implementation of the loguru replacement.

#include "port/loguru/loguru.hpp"
#include "port/port_log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace {

void default_sink(int verbosity, const char* file, unsigned line, const char* message)
{
    const char* level = verbosity <= loguru::Verbosity_FATAL     ? "FATAL"
                        : verbosity == loguru::Verbosity_ERROR   ? "ERROR"
                        : verbosity == loguru::Verbosity_WARNING ? "WARN "
                        : verbosity == loguru::Verbosity_INFO    ? "INFO "
                                                                 : "DEBUG";
    std::fprintf(stderr, "%s %s:%u| %s\n", level, file, line, message);
}

[[noreturn]] void default_abort(const char*)
{
    std::abort();
}

g3port::LogSink g_sink = default_sink;
g3port::AbortHandler g_abort = default_abort;

// Basename of a path: "a/b/c.cpp" -> "c.cpp" (keeps log lines short).
const char* base_name(const char* path)
{
    const char* base = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    return base;
}

} // namespace

namespace loguru {

Verbosity g_stderr_verbosity = Verbosity_INFO;
bool g_preamble_date = false;
bool g_preamble_time = false;
bool g_preamble_uptime = false;
bool g_preamble_thread = false;

Verbosity current_verbosity_cutoff()
{
    return g_stderr_verbosity;
}

void log(Verbosity verbosity, const char* file, unsigned line, const char* format, ...)
{
    char message[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof message, format, args);
    va_end(args);
    g_sink(verbosity, base_name(file), line, message);
}

void log_and_abort(int, const char* expr, const char* file, unsigned line, const char* format,
                   ...)
{
    char message[512];
    int n = std::snprintf(message, sizeof message, "%s", expr);
    if (n < 0)
        n = 0;
    if (static_cast<size_t>(n) < sizeof message) {
        va_list args;
        va_start(args, format);
        std::vsnprintf(message + n, sizeof message - n, format, args);
        va_end(args);
    }
    g_sink(Verbosity_FATAL, base_name(file), line, message);
    g_abort(message);
    std::abort();  // in case a handler returns despite the contract
}

void log_and_abort(int skip, const char* expr, const char* file, unsigned line)
{
    log_and_abort(skip, expr, file, line, "%s", "");
}

} // namespace loguru

namespace g3port {

void set_log_sink(LogSink sink)
{
    g_sink = sink ? sink : default_sink;
}

void set_abort_handler(AbortHandler handler)
{
    g_abort = handler ? handler : default_abort;
}

void set_max_verbosity(int verbosity)
{
    loguru::g_stderr_verbosity = verbosity;
}

} // namespace g3port
