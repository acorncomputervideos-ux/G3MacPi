# VIA 6522 and Cuda — implementation notes

Code: `core/devices/macio/{via6522,cuda}.*`. Tests: `tests/unit/devices/test_via_cuda.cpp`.

## Sources (facts only, no code copied)
- Linux `drivers/macintosh/via-cuda.c` — host side of the protocol.
- Linux `include/uapi/linux/{cuda,adb}.h` — packet types, command codes.
- Linux `arch/powerpc/platforms/powermac/time.c` — VIA timer clock.
- QEMU `hw/misc/macio/cuda.c` — device side (independent description).
- MOS/Rockwell 6522 data sheet behaviour for IFR/IER, timers, ports.

## VIA
| Item | Value | Source |
|---|---|---|
| Register spacing | 0x200 (16 regs, 0x2000 window) | Linux `RS 0x200`, ioremap 0x2000 |
| Timer clock | 4.7 MHz / 6 = 783.333 kHz | Linux `VIA_TIMER_FREQ_6 4700000`; QEMU `CUDA_TIMER_FREQ (4700000 / 6)` |
| IER | write bit7=1 sets, bit7=0 clears; reads with bit7=1 | 6522 |
| IFR | write 1 clears; bit7 = any enabled flag set | 6522 |
| SR interrupt bit | 0x04; ACR SR_EXT 0x0C, SR_OUT 0x10 | Linux |

Simplifications: timer interrupt after N+1 ticks (6522: N+1.5 one-shot,
N+2 free-running); port A, CA/CB lines, PB7 output and shift modes other
than external clock are not modelled.

## Cuda
| Item | Value | Source |
|---|---|---|
| Port B: TREQ bit 3 (in), TACK bit 4, TIP bit 5 (out), all active low | | Linux comments + code |
| Packet types ADB 0, CUDA 1, ERROR 2, TIMER 3, POWER 4 | | Linux adb.h |
| Commands AUTOPOLL 1, GET_TIME 3, GET_PRAM 7, SET_TIME 9, POWERDOWN 0x0A, SET_PRAM 0x0C, RESET_SYSTEM 0x11, SET_AUTO_RATE 0x14, GET_AUTO_RATE 0x16, SET/GET_DEVICE_LIST 0x19/0x1A, ... | | Linux cuda.h |
| Reply: CUDA_PACKET, 0, cmd, data; error: ERROR_PACKET, code, CUDA_PACKET, cmd (2 unknown, 5 bad params) | | QEMU; Linux parses reply[3..6] for time |
| Time: seconds since 1904-01-01 | | Linux (`seconds since 1904`), QEMU |
| Transfer handshake | see cuda.h | Linux state machine; QEMU device side |

Verification: the test host re-implements Linux's `cuda_init_via`,
`cuda_start` and `cuda_interrupt` states and runs them against our Cuda
through the VIA registers on the bus with guest time: init sync, time,
PRAM, autopoll, error and ADB replies, power commands, 200 back-to-back
requests. Sabotage of TREQ-with-last-byte, idle sync and IFR clearing is
caught.

## Open points (to verify)
- Response delay (20 us, QEMU's default) — real Cuda timing unknown.
- PRAM size (256) and the GET_PRAM reply length.
- Autopoll reset rate; ADB devices (keyboard/mouse) not attached yet —
  ADB commands answer "no response" (flag 0x02, as Linux interprets it).
- Which PIC input the VIA interrupt uses on the B&W (machine model).
- Collision handling (Cuda wants to send while the host starts): the host
  side is implemented by Linux; the Cuda only starts unsolicited packets
  when idle — autopoll packets will need this when ADB arrives.
