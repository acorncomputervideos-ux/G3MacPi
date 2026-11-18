# guest/ — code that runs inside the emulated Mac

Code here is compiled for the emulated PowerPC 750 with a cross-compiler
(`powerpc-linux-gnu-gcc`, Ubuntu package `gcc-powerpc-linux-gnu`). It is
not part of the emulator itself. The first real user will be our Mac OS
display driver (NDRV), packaged in Apple's PEF format by a tool of our
own.

- `smoke/` — a minimal freestanding C file. CI compiles it to check the
  toolchain: the output must be a 32-bit big-endian PowerPC ELF object.

Flags used: `-m32 -mbig-endian -mcpu=750 -ffreestanding -fno-builtin
-nostdlib -O2`.
