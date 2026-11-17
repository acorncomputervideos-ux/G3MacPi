# Architecture Decision Records

Accepted by the project owner on 2026-11-16 (reply "Ok" to the recommendations
in `docs/01-feasibility.md` §9).

| ADR | Decision | Status |
|---|---|---|
| 0001 | Build the bare-metal image as **AArch64** with Circle (`RASPPI=4`, `kernel8-rpi4.img`) | Accepted |
| 0002 | Boot Mac OS 9 with **our own high-level Open Firmware** (was superseded by 0010; **reinstated by 0011**) in C++; booting a user-dumped real boot ROM is a later, optional path | Accepted |
| 0003 | **Stage hardware**: Paddington ATA, ADB via Cuda, simple framebuffer + own NDRV first; CMD646, guest USB (OHCI) and ATI Rage 128 later | Accepted |
| 0004 | Golden CPU test vectors **may be generated offline** on real PowerPC hardware or by an existing emulator used only as a reference; such tools are never linked into or copied into this project | Accepted |
| 0005 | Licence: **GPL-3.0-or-later** (required by Circle's GPL-3.0) | Accepted |
| 0006 | Builds come from **GitHub Actions** (artifact `g3macpi-sdcard`); SD-card copy first, Circle HTTP bootloader to be evaluated in M1 | Accepted |
| 0007 | Portable core rules: `core/` uses only `<stdint.h>`/`<stddef.h>`, no Circle headers, no exceptions/RTTI; enforced by `tools/check-core.sh` in CI | Accepted |
| 0008 | Own minimal unit-test framework (`tests/framework/g3test.h`) instead of a third-party one: no vendored dependency, ~150 lines | Accepted |
| 0009 | Circle pinned as a git submodule at tag **Step51.1.1**; toolchain pinned to Arm GNU **15.2.rel1** (the version Circle documents as tested) | Accepted |
| 0010 | **Build on DingusPPC (GPL-3.0)**: port its emulation code to bare-metal Circle via circle-stdlib and improve it, instead of writing every component from scratch. Owner chose option B on 2026-11-16. Real Apple boot ROM dumps become required (Beige G3 first, then B&W G3). See `docs/05-dingusppc-port.md`. | **Superseded by 0011** |
| 0011 | **Back to the original plan: independently developed emulator** (owner's request 2026-11-17). DingusPPC port reverted (code identical to 02b7989 again); ADR-0002/0003 apply again. DingusPPC's CPU **test vectors** are kept as reference data in `tests/vectors/dingusppc/` (GPL-3.0, ADR-0004). Work that needs no Pi testing is prioritised: CPU (Milestone 2) on Linux/CI. | Accepted |
