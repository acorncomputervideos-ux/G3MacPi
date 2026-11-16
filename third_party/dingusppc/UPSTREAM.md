# Vendored DingusPPC

- Upstream: https://github.com/dingusdev/dingusppc
- Imported commit: `d0fb711842f6e9851394ea036b049668b3119d52` (2026-11-10),
  exported with `git archive`.
- Licence: GPL-3.0 (see `LICENSE`); authors in `CREDITS.md`.
- Imported on 2026-11-16 for G3MacPi (ADR-0010).

## Left out
- `thirdparty/cubeb`, `thirdparty/capstone` (git submodules upstream: desktop
  audio and a 68k disassembler, not used on the Pi), `.gitmodules`.
- Upstream's `.github/` is kept for reference but does not run here (GitHub
  only runs workflows from the repository root).

## Changes made in G3MacPi
Every change to files in this directory is its own commit with a message
starting `dingusppc:`. List them with:

    git log --oneline -- third_party/dingusppc

Bare-metal code lives OUTSIDE this directory (in `port/`) wherever possible,
to keep merging upstream changes simple. General fixes should be offered back
upstream.
