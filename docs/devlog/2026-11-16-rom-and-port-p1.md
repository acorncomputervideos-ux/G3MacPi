# 2026-11-16 — Owner's ROM identified; DingusPPC port step P1 paused

## ROM supplied by the owner (not stored in this repository)
| Field | Value |
|---|---|
| File size | 4,194,304 bytes (4 MB) |
| Stored checksum (bytes 0-3) | `0x78F57389` |
| Computed checksum (16-bit BE word sum, bytes 4..0x2FFFFF, as DingusPPC `oldworldchecksum`) | `0x78F57389` — **valid** |
| ROM version (bytes 8-9) | `0x077D` (minor `0x45F2`) |
| Boot signature | `Boot Gossamer 0.` at offset 0x30D064 |
| SHA-256 | `6e389f7f2621d0a8e0302a68946cb49eef4fe17a650a0b5da1c0314e53b5c55c` |
| DingusPPC identification | `romidentity.cpp`: version 0x077d45f2, checksum 0x78f57389 → machine **`pmg3dt`, "Power Mac G3 Beige", "Power Mac G3 (v3)"** |

This is an **Old World Beige G3** ROM, the first boot target (plan step P5).
It is not a Blue & White G3 ROM; a B&W boot ROM (version 0x11f4,
"3.1.1f4" per DingusPPC's table) is still needed for step P6.

## P1 status
Paused: the Claude session's safety policy blocked executing third-party code
(DingusPPC's test programs) and further inspection of circle-stdlib.
Waiting for the owner's permission before vendoring DingusPPC and building
circle-stdlib (which runs newlib's configure scripts).
