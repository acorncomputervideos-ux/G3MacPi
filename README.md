# G3MacPi

A **bare-metal PowerPC Macintosh G3 (Blue & White) emulator** for the
**Raspberry Pi 400**: a port of the [DingusPPC](https://github.com/dingusdev/dingusppc)
emulator (GPL-3.0) to the [Circle](https://github.com/rsta2/circle) bare-metal
framework, with Pi-specific host code and improvements developed here.

## Status

**Milestone 0 (foundation).** Research and architecture are done; the
repository, host test build, CI and a first bare-metal kernel (startup screen
only) exist. **No Macintosh emulation exists yet.** The first kernel **boots on a real
Pi 400** — see [docs/devlog](docs/devlog/).

| Document | Contents |
|---|---|
| [docs/01-feasibility.md](docs/01-feasibility.md) | Research findings, obstacles, decisions needed |
| [docs/02-architecture.md](docs/02-architecture.md) | Proposed software architecture |
| [docs/03-repository-structure.md](docs/03-repository-structure.md) | Proposed repository layout |
| [docs/04-roadmap.md](docs/04-roadmap.md) | Milestones and acceptance criteria |
| [docs/05-dingusppc-port.md](docs/05-dingusppc-port.md) | **Current plan:** porting DingusPPC, feasibility results |
| [docs/references.md](docs/references.md) | Documentation consulted / still needed |
| [docs/building.md](docs/building.md) | **How to build, flash the SD card from an iPad, and what to expect** |
| [docs/decisions/](docs/decisions/README.md) | Accepted architecture decisions |

## Legal

No Apple ROMs, firmware or system software are, or will be, included in this
repository. Users must supply their own legally obtained Mac OS installation.

## Licence

GPL-3.0-or-later (see `LICENSE`). Circle, which this project links with, is GPL-3.0.
