# RTAS (Run-Time Abstraction Services)

Code: `core/firmware/openfirm/rtas.*`, wired up in
`core/machine/bw_firmware.cpp`. Tests: `tests/unit/firmware/test_rtas.cpp`.

Apple's Mac OS ROM loader requires `/rtas`. Without it, it stops with
`"/rtas" not found!`. It reads every token it knows of, and it requires
`read-pci-config` and `write-pci-config`. Mac OS reads the clock through
RTAS ("Clock primitives accessed via RTAS").

## Sources

- **CHRP binding rev 1.7 §5.2**:
  - `/rtas` is a child of `/` with no reg or ranges;
  - its properties are `rtas-size`, `rtas-version` and one per
    implemented function, named after the function, whose value is a
    token (encode-int), present only if implemented;
  - the method is `instantiate-rtas ( rtas-base-address -- rtas-call )`.
- **Linux, read for facts only:**
  - The call convention (`arch/powerpc/kernel/rtas_entry.S`, 32-bit
    path): MSR IR, DR and EE off; r3 = the argument buffer's physical
    address; r4 = the RTAS base; LR = the return address.
  - The buffer is token, nargs, nret, the argument cells, then the result
    cells, with the status first (`rtas-types.h`).
  - Status values: 0, −1 hardware error, −3 parameter error (`rtas.h`).
  - Function shapes: `rtas-rtc.c` (time of day), `platforms/chrp/nvram.c`
    (nvram-fetch and nvram-store), `rtas.c` (power-off, system-reboot).
  - The config-address format is in `rtas.h`'s `rtas_config_addr`
    (PAPR 7.3.4-1).

## Implementation

- `instantiate-rtas` writes the trap word `Firmware::kTrapRtas`
  (0x00004F10) at the given address, which must be aligned, and returns
  that address as the entry.
- The trap runs `Rtas::call` on r3's buffer, read through the CPU's
  current translation (physical while RTAS runs untranslated), then
  returns to LR.

| Function | Shape | Ours |
|---|---|---|
| get-time-of-day | ( -- status y m d h min s ns ) | `rtc_seconds` from the config, plus machine time. nanoseconds 0. |
| set-time-of-day | ( y m d h min s ns -- status ) | Adjusts the offset. Invalid dates: −3. |
| nvram-fetch / nvram-store | ( index buf len -- status actual ) | mac-io's 8 KB NVRAM, clipped at its end. |
| display-character | ( char -- status ) | To stdout. |
| power-off / system-reboot | ( hi lo -- ) / ( -- ) | Recorded, and the CPU parked on the idle trap. |
| read-pci-config | ( addr size -- status value ) | Through Grackle's bus 0 and the 21154 bridge, as the CPU's own config cycles are. Sizes 1/2/4, aligned. |
| write-pci-config | ( addr size value -- status ) | Same, with byte enables. |

An unknown token returns −1.

## Choices and limits

- `rtas-size` is 4096 (one trap word).
- `rtas-version` is 1; the CHRP architecture spec defines the value
  (to verify).
- Which functions Apple's B&W firmware offers isn't known (to verify
  against a real device tree). The loader also looks for
  set/get-time-for-power-on, event-scan and check-exception, and goes on
  without them.
- Reading a missing PCI function returns status 0 and all ones, as a
  master abort does.
