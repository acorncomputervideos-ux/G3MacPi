# SPDX-License-Identifier: GPL-3.0-or-later
#
# app/circle/Makefile — builds kernel8-rpi4.img with Circle.
# Use tools/build-pi.sh rather than calling this directly: it configures
# Circle (AArch64, RASPPI=4) and builds the Circle libraries first.

CIRCLEHOME = ../../third_party/circle

OBJS	= main.o kernel.o teedevice.o

LIBS	= $(CIRCLEHOME)/lib/libcircle.a

# The portable emulator core is included as "core/..." from the repo root.
EXTRAINCLUDE = -I ../..

G3_GIT_REV := $(shell git describe --always --dirty 2>/dev/null || echo unknown)
DEFINE	+= -DG3_GIT_REV=\"$(G3_GIT_REV)\"

include $(CIRCLEHOME)/Rules.mk

-include $(DEPS)
