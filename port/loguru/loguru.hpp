// SPDX-License-Identifier: GPL-3.0-or-later
//
// port/loguru/loguru.hpp — minimal replacement for the loguru logging library
// used by DingusPPC.
//
// The real loguru needs signals, pthreads and a background thread, none of
// which exist on bare metal. This header provides the subset of the loguru
// API that DingusPPC's emulation code uses (LOG_F, VLOG_F, LOG_IF_F, ABORT_F,
// CHECK_F, named verbosities, g_stderr_verbosity) with the same macro
// semantics, and forwards messages to the sink in port/port_log.h.
//
// It must be found BEFORE third_party/dingusppc/thirdparty/loguru on the
// include path.

#ifndef G3_PORT_LOGURU_HPP
#define G3_PORT_LOGURU_HPP

namespace loguru {

using Verbosity = int;

enum NamedVerbosity : Verbosity {
    Verbosity_INVALID = -10,
    Verbosity_OFF     = -9,
    Verbosity_FATAL   = -3,
    Verbosity_ERROR   = -2,
    Verbosity_WARNING = -1,
    Verbosity_INFO    = 0,
    Verbosity_0       = 0,
    Verbosity_1       = 1,
    Verbosity_2       = 2,
    Verbosity_3       = 3,
    Verbosity_4       = 4,
    Verbosity_5       = 5,
    Verbosity_6       = 6,
    Verbosity_7       = 7,
    Verbosity_8       = 8,
    Verbosity_9       = 9,
    Verbosity_MAX     = 9,
};

// Same meaning as in loguru: messages above this verbosity are not printed.
extern Verbosity g_stderr_verbosity;
// Accepted for source compatibility; the sink decides about preambles.
extern bool g_preamble_date;
extern bool g_preamble_time;
extern bool g_preamble_uptime;
extern bool g_preamble_thread;

Verbosity current_verbosity_cutoff();

void log(Verbosity verbosity, const char* file, unsigned line, const char* format, ...)
    __attribute__((format(printf, 4, 5)));

[[noreturn]] void log_and_abort(int stack_trace_skip, const char* expr, const char* file,
                                unsigned line, const char* format, ...)
    __attribute__((format(printf, 5, 6)));
[[noreturn]] void log_and_abort(int stack_trace_skip, const char* expr, const char* file,
                                unsigned line);

} // namespace loguru

#define VLOG_F(verbosity, ...)                                                             \
    (((verbosity) > loguru::current_verbosity_cutoff())                                    \
         ? (void)0                                                                         \
         : loguru::log((verbosity), __FILE__, __LINE__, __VA_ARGS__))

#define LOG_F(verbosity_name, ...) VLOG_F(loguru::Verbosity_##verbosity_name, __VA_ARGS__)

#define VLOG_IF_F(verbosity, cond, ...)                                                    \
    (((verbosity) > loguru::current_verbosity_cutoff() || (cond) == false)                 \
         ? (void)0                                                                         \
         : loguru::log((verbosity), __FILE__, __LINE__, __VA_ARGS__))

#define LOG_IF_F(verbosity_name, cond, ...)                                                \
    VLOG_IF_F(loguru::Verbosity_##verbosity_name, cond, __VA_ARGS__)

#define ABORT_F(...) loguru::log_and_abort(0, "ABORT: ", __FILE__, __LINE__, __VA_ARGS__)

#define CHECK_WITH_INFO_F(test, info, ...)                                                 \
    (((test) == true) ? (void)0                                                            \
                      : loguru::log_and_abort(0, "CHECK FAILED:  " info "  ", __FILE__,    \
                                              __LINE__, ##__VA_ARGS__))

#define CHECK_F(test, ...) CHECK_WITH_INFO_F(test, #test, ##__VA_ARGS__)
#define CHECK_NOTNULL_F(x, ...) CHECK_WITH_INFO_F((x) != nullptr, #x " != nullptr", ##__VA_ARGS__)

#endif // G3_PORT_LOGURU_HPP
