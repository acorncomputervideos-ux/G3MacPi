# 2026-11-26 — 256 colours: a real vertical-blank interrupt

## The problem

Mac OS 9.2.1 started from a hard disk in 256 colours sometimes stopped at
"Starting Up" for good, with our display driver. Thousands and millions of
colours started. The default depth had been moved to millions because of it.

## How it was found

- `G3_TIME=n` (g3_host) fixes the Mac's clock at power-on, so a run repeats
  exactly; `G3_MHZ` changes the trajectory. With the driver's 16 ms timer
  the stop happened at the default speed and at 350 MHz, not at 12 other
  speeds; 15, 17 and 50 ms periods and 16.667 ms reached the desktop on the
  trajectories tried. So: a race, not a fixed fault.
- A per-instruction trace (a temporary step hook, removed) showed:
  - the Ticks counter (low memory 0x16A, read through the MMU) stopped at
    12.95 s while our VBL timer kept running;
  - the Mac OS timer task sleeps with NanoKernel call 55 (MPDelayUntilSys
    in the powermac-rom NanoKernel sources' labels) until the earliest
    interrupt timer and runs the due ones, ours included, which re-arms
    itself with SetInterruptTimer;
  - at the stop it slept twice at the same instant, then only ever waited
    for our timer: every other timer (Ticks, the Ethernet driver's
    polling) had gone, and the main task went idle waiting for them.
- The same run stops with the JIT off.

Why the other timers were lost is not known (Apple's guide allows
SetInterruptTimer at secondary interrupt level). Real video cards don't
use a timer for vertical blanking, so the fix is the hardware's way.

## The fix

- **Device** (docs/hardware/framebuffer.md, version 2): IRQ_ENABLE (0x34)
  and IRQ_STATUS (0x38, write 1 to clear); a vertical blank every 1/60 s of
  guest time while enabled; a level line on Paddington input 22 (our
  choice).
- **Device tree**: the display node has `interrupts` 22 and
  `interrupt-parent`, so Mac OS makes its `driver-ist`.
- **Driver**: installs its handler on driver-ist member 0
  (GetInterruptFunctions / InstallInterruptFunctions from
  DriverServicesLib, then the existing enabler); the handler acknowledges
  the device and calls VSLDoInterruptService. No interrupt timers any more.

## Results

- 13 of 13 runs at different CPU speeds (including the two that stopped)
  reach the desktop in 256 colours; thousands and millions too.
- 60 acknowledgements a second; the pointer moves and menus open in 256
  colours.
- Tests: devices/framebuffer/vertical_blank_interrupt; the display node's
  interrupt properties; the driver test now plays GetInterruptFunctions,
  InstallInterruptFunctions and the enabler, and calls the handler as Mac
  OS would. Sabotages (driver not acknowledging, device not clearing, no
  `interrupts` property) each fail a test.
- Also fixed: tests/unit/platform/test_wifi_bridge.cpp used a 3-byte array
  as a 6-byte MAC address (AddressSanitizer).
