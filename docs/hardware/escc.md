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

## Model

- Transmit is immediate, to an optional sink (a device attached to the
  channel), and sets Tx-interrupt pending when WR1 enables it.
- Receive: a 3-character FIFO (the Z85C30's), filled by the attached
  device while WR3's receiver enable is set; a character arriving with it
  full is lost and sets RR1's overrun bit (cleared by WR0's error reset).
  WR1's receive interrupt modes: every character (0x10), the first one
  (0x08: after the mode is selected or WR0 command 4), special conditions
  only (0x18; here only overrun).
- Modem lines: the Mac drives DTR (WR5 0x80) and RTS (WR5 0x02); the
  attached device is told when they change. It drives DCD and CTS, read in
  RR0 (0x08, 0x20; on Macs the CTS bit reads inverted, 0 = asserted, as
  Linux pmac_zilog.c reads it); a change raises the external/status interrupt when WR1
  bit 0 and the line's WR15 enable (0x08 DCD, 0x20 CTS) are set; WR0
  command 2 clears it.
- RR3 on channel A: 0x01/0x02/0x04 channel B ext/Tx/Rx pending,
  0x08/0x10/0x20 channel A (pmac_zilog.h).
- Baud rate from WR12/13, WR4's clock mode and WR14's generator enable:
  3 686 400 / (2 × mode × (TC + 2)) (clock from QEMU's mac-io, ESCC_CLOCK).
- An interrupt is asserted while a channel has one pending and WR9's MIE
  is set. Channel A is on input 15, B on 16.
- Not modelled: baud timing on the Mac's side (transmit is instant), DMA,
  sync/SDLC, parity and framing errors, the status in RR2's vector, the
  IUS daisy chain.

## The internal modem (to verify)

The Blue & White offered an optional internal 56K modem in a modem slot
(Apple's specifications; third-party serial cards such as Griffin's gPort
used the same slot). Linux treats an SCC channel whose device-tree node
has `slot-names` "Modem" (or `compatible` "cobalt") as an internal modem
and resets it through Heathrow's modem-reset GPIO (`heathrow_modem_enable`;
on Yosemite, the B&W, it leaves HRW_SCC_TRANS_EN_N alone). Which channel
the B&W's slot used, and its real `slot-names`, are not yet confirmed; the
virtual modem goes on channel A, the traditional modem port.
