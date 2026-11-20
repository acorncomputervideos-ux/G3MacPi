// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/cpu/ppc750/disasm.h — PowerPC 750 disassembler (for the debugger,
// trace logs and tests).
//
// Output: mnemonic padded to 8 columns (at least one space before
// operands), operands separated by ", ". Simplified mnemonics follow the
// PEM appendix F conventions (li, mr, nop, slwi, beq, bdnz, ...). Immediates
// are hexadecimal ("0x10", "-0x8"), branch targets are 8-digit hex
// addresses. Encodings that are not valid 750 instructions — including
// reserved fields that are not zero and POWER/601-only or 603-only
// instructions — are shown as "dc.l 0xXXXXXXXX".
//
// Freestanding: no C library use.

#ifndef G3_CORE_CPU_PPC750_DISASM_H
#define G3_CORE_CPU_PPC750_DISASM_H

#include "core/common/types.h"

namespace g3::ppc {

// Writes a NUL-terminated disassembly of 'insn' located at 'addr' to 'out'
// (truncated to out_size - 1 characters). Returns true if the encoding is a
// valid 750 instruction, false if it was shown as "dc.l".
bool disassemble(u32 addr, u32 insn, char* out, size_t out_size);

} // namespace g3::ppc

#endif // G3_CORE_CPU_PPC750_DISASM_H
