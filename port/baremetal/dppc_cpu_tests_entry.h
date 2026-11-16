// SPDX-License-Identifier: GPL-3.0-or-later
//
// port/baremetal/dppc_cpu_tests_entry.h — interface to DingusPPC's CPU test
// program as compiled into the Pi kernel.

#ifndef G3_PORT_BAREMETAL_DPPC_CPU_TESTS_ENTRY_H
#define G3_PORT_BAREMETAL_DPPC_CPU_TESTS_ENTRY_H

// Runs all tests. Returns the number of DISASSEMBLER test failures (the
// upstream main() return value). CPU results are in the globals below.
int dppc_ppctests_main();

extern int ntested;   // CPU (integer + floating point) instructions tested
extern int nfailed;   // ... of which failed

#endif
