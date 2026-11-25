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
| PPP provider, bridged to the network | | next |
| Dial-up sounds, generated | | |
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
