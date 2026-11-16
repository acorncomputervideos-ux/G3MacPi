# SPDX-License-Identifier: GPL-3.0-or-later
#
# app/circle/Makefile — builds kernel8-rpi4.img with Circle + circle-stdlib.
# Use tools/build-pi.sh rather than calling this directly: it configures
# circle-stdlib (AArch64, RASPPI=4) and builds newlib and Circle first.

STDLIBHOME = ../../third_party/circle-stdlib
CIRCLEHOME = $(STDLIBHOME)/libs/circle

# Provides CIRCLE_STDLIB_INCLUDES / CIRCLE_STDLIB_LIBS (newlib + Circle libs).
include $(STDLIBHOME)/Config.mk

# Portable code shared with the Linux test build.
PORTABLE_OBJS = ../../platform/common/hid_keyboard.o

OBJS	= main.o kernel.o teedevice.o $(PORTABLE_OBJS)

# Our code is included as "core/...", "platform/..." from the repo root.
EXTRAINCLUDE = -I ../..

# Warnings are errors for our code (a Circle macro clash was once only a warning).
WARNINGS = -Wall -Werror

G3_GIT_REV := $(shell git describe --always --dirty 2>/dev/null || echo unknown)
DEFINE	+= -DG3_GIT_REV=\"$(G3_GIT_REV)\"

include $(CIRCLEHOME)/Rules.mk

INCLUDE := $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE)
# DingusPPC + port layer (built by port/circle/Makefile) before the stdlib.
LIBS := ../../port/circle/libg3dppc.a $(CIRCLE_STDLIB_LIBS)

-include $(DEPS)
