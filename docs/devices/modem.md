# Virtual dial-up modem

A modem the Mac can dial out with, as with a real Blue & White's optional
internal 56K modem: Remote Access dials, the dial-up sounds play, and the
"provider" at the other end is the emulator itself, which connects the
Mac's PPP link to the network the Pi is on. Nothing is modulated; what is
reproduced is what the Mac sees.

## Pieces

| Piece | Code | State |
|---|---|---|
| Serial port (SCC receive, DCD/CTS, DTR/RTS, interrupts) | `core/devices/macio/escc.*` | done (docs/hardware/escc.md) |
| Hayes AT modem | `core/devices/modem/hayes_modem.*` | done |
| PPP framing (RFC 1662) | `platform/common/ppp_framer.*` | done |
| PPP provider end (LCP, PAP, IPCP, IP) | `platform/common/ppp_server.*` | done |
| The provider's network side (DHCP for the Mac, ARP, IP over Ethernet) | `platform/common/dialup_link.*` | done |
| Dial-up sounds, generated | | next |
| Wiring on the Pi and in g3_host, menu options | | |

## The modem (hayes_modem.h)

Behaviour follows the Hayes command set as modems of the period
implemented it (the header lists the commands, result codes and S-register
defaults it uses). Timing of a call: dial tone 1.5 s, each DTMF digit S11
(95 ms) plus the same gap, ',' S8 (2 s), pulse digits 100 ms per pulse and
700 ms between digits, ringback 4 s, answer and training 12 s; then
`CONNECT 53333` and carrier detect on (`&C1`). The sound generator follows
the modem's events, so what is heard matches the timing.

Not modelled: incoming calls (ATA answers NO CARRIER), stored numbers,
real line speeds (data is passed as fast as the serial port takes it; a
56K-like limit is planned as an option), V.42/V.44 negotiation reports.

## Tests

`tests/unit/devices/test_hayes_modem.cpp`: commands and results, the dial
sequence and its timing (to the millisecond), carrier, data in both
directions, the escape (including "+++" inside data and four pluses),
DTR and remote hang-ups. Sabotage runs (escape without the leading
silence, DCD ignoring &C, DTR drop ignored, line data passed in command
mode, no DTMF gap, V0 ignored, a key not aborting the dial, four pluses
accepted) each fail a test.

## The PPP end (ppp_server.h)

Started when the modem connects. It asks for PAP (any name and password
are accepted: the "Welcome to G3MacPi" message), ACCM 0, a magic number
and both header compressions; it acknowledges the Mac's MRU, ACCM, magic
number and compressions and rejects anything else (asking us to
authenticate to it, call-back, ...). IPCP starts once the provider knows
the Mac's address: the Mac's request for address and DNS 0.0.0.0 gets a
Nak with them, VJ compression and NBNS are rejected. After that, IP
packets pass both ways; other protocols (ATCP for AppleTalk, CCP ...)
get a Protocol-Reject. Restart timer 3 s, at most 10 requests, then the
call is ended. Tests: `tests/unit/platform/test_ppp_server.cpp` (the
test plays the Mac's side); 10 sabotage runs, two of which needed the
last two tests.

## The network side (dialup_link.h)

When the call is up, it asks the network's DHCP server for an address
as the Mac (the Mac's hardware address, the BROADCAST flag set), and gives
the PPP end the address, the router (as the PPP peer) and up to two DNS
servers. It renews at half the lease. The Mac's IPv4 packets go out in
Ethernet frames under the Mac's hardware address: to the router, or to
a host on the same subnet, after ARP (up to 3 requests a second apart;
up to 8 packets held meanwhile). It answers ARP for the Mac's address.
Only IPv4 to the Mac's address and hardware address goes back to the
Mac. On the network, the dialled-in Mac is the same station as the Mac
on Ethernet; Mac OS uses one or the other at a time. Tests:
`tests/unit/platform/test_dialup_link.cpp`; 8 sabotage runs, all caught.
