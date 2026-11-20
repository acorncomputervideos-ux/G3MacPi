# 2026-11-17 — Physical bus, scheduler, execution loop

- `core/mem/physical_bus`: RAM/ROM/MMIO regions, straddling accesses,
  unmapped-access policies, DMA host pointers. 7 tests (incl. the CPU
  executing from bus-mapped RAM).
- `core/sched/scheduler`: picosecond guest-time events, FIFO ties,
  cancellation with generation-checked handles, rescheduling from callbacks.
- `core/machine/execution_loop`: runs the CPU in slices up to the next event
  or decrementer expiry; drift-free time base.
- Bug found by the drift test: each run_for() measured its end from the
  actual (overshot) time, so the up-to-one-instruction overshoot accumulated
  (69 extra time-base ticks in 10 ms). Fixed by accumulating the requested
  end time; the test now requires an exact tick count.
- 116/116 tests (gcc + ASan/UBSan, clang); check-core passes.
