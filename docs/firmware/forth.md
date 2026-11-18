# The Forth interpreter — implementation notes

Code: `core/firmware/forth/forth.*`. Tests: `tests/unit/firmware/test_forth.cpp`.

Mac OS's boot file carries an Open Firmware boot script (see
`bootinfo.md`). The CHRP binding says each line of it is processed as if
typed at the Open Firmware user interface, so our firmware needs a Forth
interpreter. This one is written from the language standards; no
existing Forth's code was used.

## Sources and facts

- **ANS Forth (ANSI X3.215-1994)**, on which IEEE 1275's Forth is based.
  Semantics come from it: stack and arithmetic words, `do … loop`/`+loop`
  (leave when the index crosses the limit−1 | limit boundary),
  `begin … while … repeat`, `case … of … endof … endcase`,
  `create … does>`, `catch`/`throw` (catch restores the stack depth) and
  the throw codes −1 abort, −2 abort", −3/−4 stack overflow/underflow,
  −5/−6 return stack, −9 invalid address, −10 division by zero,
  −13 undefined word, −14 compile-only, −22 control structure mismatch.
- **IEEE 1275-1994** differences used here (to verify against the
  standard text when we get it):
  - the initial base is **16**;
  - the `d#` `h#` `o#` `b#` number prefixes, and `.d` `.h`;
  - `"` strings with `"(hex bytes)` escapes;
  - `between`, `<<` `>>`, `u2/`, `w@`/`w!`/`l@`/`l!`, `comp`;
  - cells are 32 bits (PowerPC binding);
  - word lookup ignores case.

## Design

- **Forth addresses are guest addresses.** The data space (`here`,
  `allot`, `,`, variables, values, `create` bodies, the `base` variable,
  two transient string buffers) is a region of guest memory, reached
  through `ppc::MemoryInterface`. Values a script stores can be seen by
  client programs, as on real firmware. `@`/`!` and friends are
  big-endian, through the same interface.
- Word headers and compiled colon definitions live on the host.
  Compiled code is an array of cells: word indices, or inline operations
  (literal, branches, loop control, inline strings, `does>`, `to`, `of`).
- Execution tokens are `0x00E00000 + word index`.
- Errors never use C++ exceptions. A failing word sets an error that
  unwinds to the nearest `catch` or out of `interpret()`, which then
  resets both stacks and drops a half-finished definition.
- Nesting is limited (200 calls), so runaway recursion becomes a Forth
  error instead of overflowing the host stack. A sabotage run without
  the limit crashes the test process, which shows the limit is needed.
- `interpret()` keeps compile state between calls, so a definition can
  span several lines of a boot script.
- Firmware words are added with `define_native()` and
  `define_constant()`.

Implementation choices where the standards allow several:
- `/` and `mod` truncate toward zero (as in C), so `-7 2 /` gives −3.
  `MIN_INT -1 /` gives MIN_INT instead of trapping.
- `"` handling: after a `"`, `(` starts hex pairs up to `)`; white space
  or end of input ends the string; any other character is taken
  literally, so `""` gives `"`.
- A failed definition stays hidden instead of being removed if it isn't
  the most recent word. Its slot is wasted, but behaviour is the same.

Limits: 1024 words, 32 K compiled cells, 16 KB of names, 256-cell stacks,
256-byte string literals, `evaluate` strings up to 1 KB, 4 levels deep.

## Not yet

Firmware words (device tree, packages, `load`, `boot`, `init-program`,
`go`), number output with `<# # #>`, `postpone`, double-cell words,
`key`/`accept`. Which of these the Mac OS boot script needs will be
decided from the script itself.
