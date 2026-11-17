# Paddington mac-io — implementation notes

Code: `core/devices/macio/paddington.*`. Tests: `tests/unit/machine/test_yosemite.cpp`.

| Fact | Value | Source / confidence |
|---|---|---|
| PCI IDs | 106B:0017 "Paddington Mac I/O" | PCI ID database — confirmed |
| Heathrow (relative) | 106B:0010 | PCI ID database |
| BAR 0 | 512 KB memory | QEMU mac-io model (`memory_region_init(&s->bar, ..., 0x80000)`) |
| Class code | 0xFF0000 | QEMU (`PCI_CLASS_OTHERS << 8`) — to verify |
| Interrupt controller | BAR + 0x00000 | QEMU Old World layout — **to verify for Paddington** |
| VIA (Cuda) | BAR + 0x16000 | QEMU Old World layout — **to verify** |
| VIA interrupt | controller input 18 (0x12) | QEMU `OLDWORLD_CUDA_IRQ` — **to verify** |
| Not modelled yet | DBDMA (+0x08000), SCC (+0x13000), IDE (+0x20000/+0x21000), NVRAM, audio, floppy | QEMU layout for reference |

The cells appear on the physical bus when BAR 0 is non-zero and memory
decoding is enabled, and move/disappear when software changes either.
