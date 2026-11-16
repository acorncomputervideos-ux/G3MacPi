// SPDX-License-Identifier: GPL-3.0-or-later
//
// port/baremetal/dppc_cpu_tests_entry.cpp — compiles DingusPPC's CPU test
// program (cpu/ppc/test/ppctests.cpp, unmodified) into the Pi kernel.
//
// Its main() is renamed to dppc_ppctests_main() so the kernel can call it.
// The test program reads ppcinttests.csv / ppcfloattests.csv /
// ppcdisasmtest.csv from the current directory and prints to std::cout; the
// kernel sets both up before calling (see app/circle/kernel.cpp).

#include <cfenv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#define main dppc_ppctests_main
#include "cpu/ppc/test/ppctests.cpp"
#undef main
