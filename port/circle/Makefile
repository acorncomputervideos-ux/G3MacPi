# SPDX-License-Identifier: GPL-3.0-or-later
#
# port/circle/Makefile — builds libg3dppc.a: DingusPPC emulation code plus
# the G3MacPi port layer, for bare-metal Circle + circle-stdlib.
#
# Built by tools/build-pi.sh; linked into app/circle. DingusPPC code is
# compiled as C++20 at its upstream warning level (not -Werror).

STDLIBHOME = ../../third_party/circle-stdlib
CIRCLEHOME = $(STDLIBHOME)/libs/circle
DPPC	   = ../../third_party/dingusppc

include $(STDLIBHOME)/Config.mk

# --- Sources -----------------------------------------------------------------
# Step P2: CPU core + DingusPPC's CPU test program (same set as the Linux
# target dppc_cpu_tests in CMakeLists.txt).
DPPC_CPU_SRCS	= $(wildcard $(DPPC)/cpu/ppc/*.cpp)
DPPC_SRCS	= $(DPPC_CPU_SRCS) \
		  $(DPPC)/core/timermanager.cpp \
		  $(DPPC)/devices/memctrl/memctrlbase.cpp \
		  $(DPPC)/cpu/ppc/test/testdisasm.cpp

PORT_SRCS	= ../loguru/loguru_shim.cpp \
		  ../baremetal/dppc_cpu_tests_entry.cpp

OBJS	= $(DPPC_SRCS:.cpp=.o) $(PORT_SRCS:.cpp=.o)

libg3dppc.a: $(OBJS)
	@echo "  AR    $@"
	@rm -f $@
	@$(AR) cr $@ $(OBJS)

# port/loguru first: <loguru.hpp> must resolve to our replacement.
EXTRAINCLUDE = -I ../loguru -I ../.. \
	       -I $(DPPC) -I $(DPPC)/core -I $(DPPC)/cpu/ppc -I $(DPPC)/devices \
	       -I $(DPPC)/debugger -I $(DPPC)/utils

DEFINE	+= -DSUPPORTS_PPC_LITTLE_ENDIAN_MODE=0 -DSUPPORTS_MEMORY_CTRL_ENDIAN_MODE=0 \
	   -DDPPC_RECURSIVE_MUTEX_HEADER='"port/baremetal/dppc_mutex.h"'

STANDARD = -std=gnu++20
WARNINGS = -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare

# DingusPPC type-puns floating-point values through pointers (ppcfpopcodes.cpp);
# without this, GCC may legally reorder those accesses at -O2.
CFLAGS += -fno-strict-aliasing

EXTRACLEAN = $(OBJS) $(OBJS:.o=.d)

include $(CIRCLEHOME)/Rules.mk

INCLUDE := $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE)

-include $(DEPS) $(OBJS:.o=.d)
