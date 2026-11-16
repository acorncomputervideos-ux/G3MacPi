# Architecture Decision Records

Accepted by the project owner on 2026-11-16 (reply "Ok" to the recommendations
in `docs/01-feasibility.md` §9).

| ADR | Decision | Status |
|---|---|---|
| 0001 | Build the bare-metal image as **AArch64** with Circle (`RASPPI=4`, `kernel8-rpi4.img`) | Accepted |
| 0002 | Boot Mac OS 9 with **our own high-level Open Firmware** in C++; booting a user-dumped real boot ROM is a later, optional path | Accepted |
| 0003 | **Stage hardware**: Paddington ATA, ADB via Cuda, simple framebuffer + own NDRV first; CMD646, guest USB (OHCI) and ATI Rage 128 later | Accepted |
| 0004 | Golden CPU test vectors **may be generated offline** on real PowerPC hardware or by an existing emulator used only as a reference; such tools are never linked into or copied into this project | Accepted |
| 0005 | Licence: **GPL-3.0-or-later** (required by Circle's GPL-3.0) | Accepted |
| 0006 | Builds come from **GitHub Actions** (artifact `g3macpi-sdcard`); SD-card copy first, Circle HTTP bootloader to be evaluated in M1 | Accepted |
| 0007 | Portable core rules: `core/` uses only `<stdint.h>`/`<stddef.h>`, no Circle headers, no exceptions/RTTI; enforced by `tools/check-core.sh` in CI | Accepted |
| 0008 | Own minimal unit-test framework (`tests/framework/g3test.h`) instead of a third-party one: no vendored dependency, ~150 lines | Accepted |
| 0009 | Circle pinned as a git submodule at tag **Step51.1.1**; toolchain pinned to Arm GNU **15.2.rel1** (the version Circle documents as tested) | Accepted |
