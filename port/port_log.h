// SPDX-License-Identifier: GPL-3.0-or-later
//
// port/port_log.h — where DingusPPC's log messages go.
//
// DingusPPC logs through the loguru macros (LOG_F, ABORT_F, ...). In
// G3MacPi those macros come from port/loguru/loguru.hpp, which formats the
// message and hands it to the sink installed here: Circle's logger on the
// Raspberry Pi, stderr on Linux.

#ifndef G3_PORT_PORT_LOG_H
#define G3_PORT_PORT_LOG_H

namespace g3port {

// verbosity uses loguru's scale: -3 FATAL, -2 ERROR, -1 WARNING, 0 INFO, 1..9 debug.
using LogSink = void (*)(int verbosity, const char* file, unsigned line, const char* message);

// Called after an ABORT_F / failed CHECK_F message was logged. Must not return.
using AbortHandler = void (*)(const char* message);

void set_log_sink(LogSink sink);           // nullptr restores the default (stderr)
void set_abort_handler(AbortHandler handler);  // nullptr restores the default (abort())

// Messages with verbosity above this are dropped before formatting.
// Default 0 (INFO): debug levels 1..9 are off.
void set_max_verbosity(int verbosity);

} // namespace g3port

#endif // G3_PORT_PORT_LOG_H
