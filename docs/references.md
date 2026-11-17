# References

Status key: **have** = consulted in this repo's research; **need** = must obtain before the dependent work.

## Host
| Document | Used for | Status |
|---|---|---|
| Circle README, samples, `doc/memorymap.txt` (github.com/rsta2/circle) | Host capabilities | have |
| BCM2711 ARM Peripherals (Raspberry Pi Ltd) | Low-level host details if Circle is insufficient | need |
| Arm Cortex-A72 TRM, Arm ARM (ARMv8-A) | JIT, cache maintenance, performance | need (M8) |

## PowerPC
| Document | Used for | Status |
|---|---|---|
| PowerPC Microprocessor Family: The Programming Environments Manual for 32-bit Implementations | Instruction semantics, MMU, exceptions | need (M2) |
| MPC750 RISC Microprocessor User's Manual | 750-specific SPRs, HID0/1, L2CR, timing | need (M2) |
| IBM PowerPC 750 errata | Corner cases | need (M2) |

## Macintosh / Blue & White G3
| Document | Used for | Status |
|---|---|---|
| Apple Developer Note: Power Macintosh G3 (Blue and White) | Board architecture | need (M4) |
| Apple TN1167 "NewWorld Architecture" | Boot process | have (summary) |
| MPC106 PCI Bridge/Memory Controller User's Manual | Grackle | need (M4) |
| DEC 21154 PCI-PCI bridge datasheet | Bridge | need (M4) |
| Real B&W G3 Open Firmware device-tree dump | Firmware, device fidelity | **need — highest priority** |
| *Designing PCI Cards and Drivers for Power Macintosh Computers* (Apple) | NDRV, Name Registry, PCI | need (M5) |
| IEEE 1275-1994 + PowerPC and CHRP bindings | Open Firmware | need (M5) |
| CMD646 / PCI0646 datasheet | Ultra ATA | need (M9) |
| ATA/ATAPI-5 and -6 standards (T13 1321D, 1410D) | ATA protocol (`core/devices/ata`) | used for facts (2026-11-17) |
| Inside Macintosh / ADB & Cuda documentation | ADB, Cuda | need (M4) |
| ATI Rage 128 register reference | Graphics | need (M9) |

## Behavioural references (read for facts only — no code copied)
| Source | Licence note |
|---|---|
| Linux `arch/powerpc/platforms/powermac/*`, `drivers/macintosh/via-cuda.c`, `pmac_pic.c`, `sound/ppc/burgundy.c`, `drivers/ata/pata_macio.c` | GPL-2.0-only — incompatible with GPL-3 code reuse; facts only |
| DingusPPC (`machines/machineyosemite.cpp` etc.) | Read for facts only |
| OpenBIOS / QEMU `mac99` | Read for facts only |
| MAME Mac tech-info wiki | Facts |
