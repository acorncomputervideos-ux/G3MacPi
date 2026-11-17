# 2026-11-17 — First Blue & White devices: Grackle config space, Heathrow PIC

- Research (facts only, no code copied): Linux powermac pic.c and
  sysdev/grackle.c; QEMU heathrow_pic.c / macio.c. Two sources agree on the
  PIC register model; Grackle facts come from Linux's setup code. Details
  and open points: docs/hardware/heathrow-pic.md, docs/hardware/grackle.md.
- `core/devices/le_register.h`: byte-lane mapping for little-endian
  registers accessed by the big-endian CPU (any size/offset).
- `core/devices/macio/heathrow_pic`: 64-input interrupt controller.
- `core/devices/pci/pci` + `grackle`: configuration mechanism, generic
  config header with BAR sizing, bus 0.
- End-to-end test: scheduled device pulse -> PIC -> CPU external
  interrupt -> guest handler (lwbrx/stwbrx ack, rfi) -> back to main loop.
  While writing it, the CPU correctly took a decrementer exception the test
  had not provided for (DEC starts at 0); the test now sets DEC like
  firmware would.
- Sabotage: swapping the PIC banks and breaking config-data byte lanes are
  caught. 128/128 tests (gcc + ASan/UBSan, clang); check-core passes.
