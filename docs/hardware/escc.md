# ESCC serial (Z85C30) — implementation notes

Code: `core/devices/macio/escc.*`. Tests: `tests/unit/devices/test_escc.cpp`.

A real Blue & White has an ESCC. Linux there reports ttyPZ0 at 0x80813020
(interrupt 15) and ttyPZ1 at 0x80813000 (16). Mac OS's 68K start-up waits
forever without the SCC's interrupt vectors. With the device-tree nodes
but no model, it took thousands of machine checks reading 0x80812000.

## Facts

- **Z8530 register access** (Linux `pmac_zilog.h`, read for facts):
  - WR0 selects a register, and the "point high" command adds 8; the next
    control access uses it.
  - WR0 commands, WR1's Tx interrupt enable, and WR9's resets and master
    interrupt enable.
  - RR0's Tx-empty and Rx-available bits, RR1's all-sent, RR2's vector,
    RR3's pending bits (read on channel A).
- **MacRISC layout** (`pmac_zilog`): control at the channel base, data at
  +0x10. Channel B is at mac-io + 0x13000, channel A at + 0x13020.
- **Legacy layout at + 0x12000** (`escc-legacy`): the 68K Mac SCC
  arrangement, B control +0, A control +2, B data +4, A data +6 (to
  verify for Paddington).

## Model: nothing connected

- Transmit is immediate, to an optional sink, and sets Tx-interrupt
  pending when WR1 enables it.
- Nothing is ever received.
- An interrupt is asserted while a channel has one pending and WR9's MIE
  is set. Channel A is on input 15, B on 16.
- Not modelled: baud timing, receive, DMA, modem lines (DCD and CTS read
  0), and the vector modified by status.
