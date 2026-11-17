# ADB (Apple Desktop Bus) — implementation notes

Code: `core/devices/adb/adb.*`, ADB handling in `core/devices/macio/cuda.*`,
`platform/common/hid_to_adb.*`. Tests: `tests/unit/devices/test_adb.cpp`,
`test_via_cuda.cpp`.

## Facts and sources
| Fact | Source |
|---|---|
| Command byte: addr<<4 \| code; 0 SendReset, 1 Flush, 8\|reg Listen, 0xC\|reg Talk | Linux adb.h |
| Default addresses keyboard 2, mouse 3 | Linux adb.h (ADB_KEYBOARD, ADB_MOUSE) |
| Keyboard R0: two key bytes, bit 7 = up, 0xFF = none, power = 0x7F 0x7F | QEMU adb-kbd.c, Linux adbhid.c (skips 0xFF and 0x7F7F) |
| Mouse R0: byte 0 = btn1-up \| dY, byte 1 = btn2-up \| dX, 7-bit signed, +Y down | QEMU adb-mouse.c; Linux adbhid.c **code** (REL_Y from data[1], REL_X from data[2]; its comment says otherwise — the code wins, and agrees with QEMU) |
| Deltas clamped to +-63 per report | QEMU |
| No data -> no answer to Talk R0 | QEMU; Cuda then reports flag 0x02 (Linux) |
| Keyboard handler 1 (2/3 extended), mouse handler 2 (1 = 100 cpi) | QEMU |
| ADB key codes | QEMU adb-keys.h table |
| Autopoll packets: ADB_PACKET, 0x40, cmd, data | QEMU; Linux passes flag 0x40 as "autopoll" |

## Decisions / to verify
- Talk R3 high byte has SRQ-enable (0x20) set — Apple ADB docs say SRQ is
  enabled after reset; QEMU returns only the address. **Verify.**
- Keyboard R2 (modifier/LED state) returns 0xFF 0xFF: **not implemented**.
- Address-collision resolution (handler 0xFE) moves unconditionally (one
  device per address here). Mouse handler 4 (extended) not supported.
- Autopoll polls the device list (or all devices) every rate ms and sends
  one packet per tick only when the link is idle.
- HID->ADB: Windows/GUI keys -> Command, Alt -> Option, Insert -> Help,
  PrintScreen/ScrollLock/Pause -> F13/F14/F15, NumLock -> keypad Clear.
