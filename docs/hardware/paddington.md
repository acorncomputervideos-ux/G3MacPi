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
| DBDMA controller | BAR + 0x08000 (32 channels × 0x80), see dbdma.md | QEMU Old World layout — **to verify** |
| NVRAM | BAR + 0x60000, 8 KB at stride 16, see nvram.md | QEMU Old World layout — **to verify** |
| IDE0 / IDE1 | BAR + 0x20000 / + 0x21000, DBDMA channels 0x16 / 0x1A, IRQs 13 / 14 (DMA 2 / 3), see ata.md | QEMU Old World — **to verify** |
| Not modelled yet | SCC (+0x13000), audio, floppy | QEMU layout for reference |

The cells appear on the physical bus when BAR 0 is non-zero and memory
decoding is enabled, and move/disappear when software changes either.
If any cell would overlap another bus region, mac-io stays unmapped and only
the cells it had already mapped are rolled back; the other region is left
untouched.
