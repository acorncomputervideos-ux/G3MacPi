# 2026-11-17 — PCI-to-PCI bridge, VIA 6522, Cuda

- DEC 21154 bridge (type 1 header) + configuration routing through bridges.
- VIA 6522: registers, timers on guest time (783.333 kHz), IFR/IER, port B,
  shift register hooks.
- Cuda: handshake, time, PRAM, autopoll flag, auto-rate, device list,
  power-down/reset callbacks, error packets, ADB "no response".
- Tested with a re-implementation of the Linux driver's algorithm as host.
- One sabotage was initially written so it did not change behaviour
  (`set_treq(out_pos_ > 1000)` is still `false`); rewritten, it is caught.
- 137/137 tests (gcc + ASan/UBSan, clang); check-core passes.
