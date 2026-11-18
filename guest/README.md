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

- `tests/` — `kernels.c`: test functions (Fibonacci, CRC-32, sorting,
  signed arithmetic, 64-bit multiply, double and single precision,
  strings), with `start.S` as the entry. The same C file is built twice:
  for the 750 with the cross-compiler, and natively for the host.
  `tests/guest/run_kernels.cpp` (`g3_guest_tests`) loads the 750 build
  with our ELF init-program, runs every function on the emulated CPU, and
  requires the native result.

Both builds use `-ffp-contract=off`, so neither fuses multiply-adds and
the floating-point results are the same IEEE operations. The guest test
is built only when `powerpc-linux-gnu-gcc` is installed. CI's
`guest-toolchain` job installs it and runs the test.
