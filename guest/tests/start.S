/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * guest/tests/start.S — entry of the guest test program. Reads the command
 * block at 0x8000 (kernel number, a, b), calls k_table[number](a, b),
 * stores the result at 0x800C and stops with the emulator trap word
 * 0x00004F99 (primary opcode 0, claimed by the test runner).
 * r1 is set by the runner (Firmware::start_client style).
 */
    .section .text
    .globl _start
_start:
    lis     11, 0x0000
    ori     11, 11, 0x8000
    lwz     4, 0(11)            /* kernel number */
    slwi    4, 4, 2
    lis     12, k_table@ha
    addi    12, 12, k_table@l
    lwzx    12, 12, 4
    mtctr   12
    lwz     3, 4(11)            /* a */
    lwz     4, 8(11)            /* b */
    bctrl
    lis     11, 0x0000
    ori     11, 11, 0x8000
    stw     3, 12(11)           /* result */
    .long   0x00004F99          /* stop */
1:  b       1b

    .section .note.GNU-stack,"",@progbits
