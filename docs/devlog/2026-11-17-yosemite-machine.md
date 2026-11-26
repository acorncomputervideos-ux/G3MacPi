# 2026-11-17 — First assembled machine (Yosemite)

- PhysicalBus::unmap (devices move when software reassigns a BAR).
- Paddington mac-io PCI function (106B:0017, confirmed in the PCI ID
  database) containing the interrupt controller and VIA/Cuda at QEMU's Old
  World offsets (to verify).
- YosemiteMachine wiring RAM/ROM, Grackle, DEC 21154, Paddington, IRQs,
  CPU and the execution loop.
- Whole-machine test: guest program does PCI enumeration through the
  bridge, BAR assignment, VIA access and a VIA timer interrupt.
- Sabotage (wrong IRQ input, no remap on BAR change) caught.
- The session's worker restarted mid-research; repository, toolchain and
  research files survived, work continued without loss.
- 140/140 tests (gcc + ASan/UBSan, clang); check-core passes.
