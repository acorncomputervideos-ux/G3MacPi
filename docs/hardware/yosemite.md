# Yosemite (Power Macintosh G3 Blue & White) machine — wiring

Code: `core/machine/yosemite.*`.

| Part | Where | Confidence |
|---|---|---|
| RAM | physical 0 | standard PowerPC Mac layout |
| Boot ROM (optional, 1 MB) | 0xFFF00000 | DingusPPC machine file; PowerPC reset vector region |
| Grackle config ports | 0xFEC00000 / 0xFEE00000 | Linux setup_grackle |
| Grackle | 00:00.0 | Linux grackle.c |
| DEC 21154 | 00:0D.0 | DingusPPC only — **verify** |
| Paddington | device 5 behind the 21154 | DingusPPC only — **verify** |
| VIA -> interrupt controller | input 18 | QEMU Old World — **verify** |
| Interrupt controller -> CPU | external interrupt | architecture |
| CPU / bus / time base | 350 MHz / 100 MHz / 25 MHz | Apple specs; TB = bus/4 on 750 systems |

**Most valuable missing reference: a device-tree dump from a real B&W G3**
(Open Firmware `dev / ls` + `.properties`, or Linux `/proc/device-tree`).
It would settle every "verify" item above.

Verified by a whole-machine test: a PowerPC program enumerates PCI via the
Grackle, programs the 21154's bus numbers, finds Paddington (106B:0017),
assigns its BAR, reads the VIA, enables IRQ 18, starts VIA timer 2 and
handles the resulting external interrupt.
