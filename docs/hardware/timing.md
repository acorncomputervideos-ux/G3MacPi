# Guest timing model — implementation notes

Code: `core/sched/scheduler.*`, `core/machine/execution_loop.*`.

- Guest time is counted in picoseconds (u64, ~213 days).
- The CPU costs a fixed `cycles_per_instruction` (default 1) at `cpu_hz`.
  Real 750 timing (pipelines, caches, memory latency) is NOT modelled; this
  only has to be good enough for the OS's calibration loops and timers.
- The time base advances at `timebase_hz`, recomputed from absolute guest
  time each slice: no drift (tested: exactly 250,000 ticks in 10 ms at
  25 MHz, over thousands of irregular run_for() calls).
- CPU slices end at the next scheduler event, the requested end time, the
  next decrementer expiry, or `max_slice` instructions, whichever is first.
  The decrementer exception is therefore taken within one instruction of
  the tick at which DEC bit 0 flips (tested).
- Values for the Blue & White G3: CPU 300-450 MHz; bus 100 MHz; on 750
  systems the time base runs at bus/4 = 25 MHz. The OS learns the rates
  from the device tree (`timebase-frequency`, `clock-frequency`), so the
  machine model and the firmware must use the SAME values.
