# Ethernet: BMAC+

`core/devices/macio/bmac.{h,cpp}`; tests `tests/unit/devices/test_bmac.cpp`.

## Facts and sources

| What | Value | Source |
|---|---|---|
| Cell offset in mac-io | 0x11000 | DingusPPC MacIoTwo |
| Registers | 16-bit little-endian at byte offsets (XIFC 0x000, TXFIFOCSR 0x100, RXFIFOCSR 0x120, XCVRIF 0x160, CHIPID 0x170, MIFCSR 0x180, SROMCSR 0x190, STATUS 0x200 (read clears), INTDISABLE 0x210, TXRST 0x420, TXCFG 0x430, RXRST 0x620, RXCFG 0x630, MADD2/1/0 0x660/0x670/0x680, BHASH3-0 0x700-0x730) | Linux `drivers/net/ethernet/apple/bmac.h`, bmac.c (`in_le16`/`out_le16`) |
| Status bits | FrameReceived 0x0001, FrameSent 0x0100 | Linux bmac.h |
| Config bits | TXCFG 1 enable; RXCFG 1 enable, 0x40 promiscuous, 0x100 keep CRC, 0x200 reject own, 0x800 hash filter | Linux bmac.h |
| Chip ID | 0xC7 on Paddington | DingusPPC BigMac |
| MII | MIFCSR bit 0 clock, 1 data out, 2 drive, 3 data in; clause 22 frames; PHY ID1 0x7810 (LXT970) | DingusPPC; Linux's bit patterns; IEEE 802.3 clause 22 |
| DMA | transmit at mac-io + 0x8200, receive + 0x8300 | DingusPPC |
| Interrupts | 42 (cell), 32 (tx DMA), 33 (rx DMA); the DMA ones are latched like the other DMA lines | a real B&W's Linux log; DingusPPC |
| Device tree | `ethernet`, type `network`, compatible `bmac+`, three reg and three interrupts, `mac-address`/`local-mac-address` | Linux bmac.c |
| Receive | INPUT_LAST descriptors; the length is taken from the residual count; the 4-byte CRC stays when RXCFG 0x100 is set | Linux bmac.c |

## Model

- Transmit: the bytes of a descriptor chain up to OUTPUT_LAST make a frame,
  handed to the `NetLink` when the transmitter is on; FrameSent is raised.
- Receive: every 0.5 ms of guest time while the receiver is on, frames are
  taken from the `NetLink`, filtered (own address, broadcast, multicast
  when the hash filter has any bit set, everything in promiscuous mode;
  own frames dropped when asked), padded to 60 bytes, given a CRC when
  RXCFG keeps it, and delivered into the next INPUT_LAST descriptor with
  the residual count (DBDMA `dma_input_packet_end`). FrameReceived raised.
- The PHY answers MII reads at address 0: link up, negotiation done.
- Not modelled: the serial EEPROM (the address comes from the device
  tree), counters, collisions, the exact multicast hash.

## On the Pi

`platform/common/net_bridge` passes frames between the emulation core and
core 3, which polls Circle's driver for the Pi's Ethernet adapter
(BCM54213, frames in without interrupts) and sends frames out; it checks
the PHY every 2 s. The Mac is given the **Pi's own Ethernet address**:
Circle's TCP/IP stack is not running, so the adapter's address filter
(own address, broadcast, AppleTalk's 09:00:07:FF:FF:FF) passes the Mac's
traffic without promiscuous mode.

## Status

- Unit tests: registers and PHY as Linux's driver drives them, transmit
  ring, receive ring with lengths from the residual and CRC, waiting for
  descriptors, filters; PIC handling of IRQs 32/33; sabotage runs fail them.
- Mac OS 9.2.1: at start-up it writes the PHY's control register twice
  (MII). Turning AppleTalk on (AppleTalk control panel, from the CD) gives
  "An error occurred attempting to use Ethernet" **without any access to
  the cell's registers, its DMA channels or its interrupt enables** — the
  failure is in Mac OS's software before the hardware; to be found.
- The Pi side is built but not yet run on hardware.
