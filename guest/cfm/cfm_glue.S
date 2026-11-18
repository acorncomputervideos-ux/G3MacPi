/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * guest/cfm/cfm_glue.S — cfm_call(tv, a1..a7): calls the Code Fragment
 * Manager routine whose transition vector is tv with a1..a7 in r3..r9.
 * See cfm.h for the conventions bridged here.
 *
 * Our frame (64 bytes, 16-byte aligned), as a CFM callee sees its caller's:
 *     0(r1)  back chain            4(r1)  callee may save CR here
 *     8(r1)  callee may save LR   20(r1)  our r2 (the TOC save slot)
 *    24(r1)..55(r1) parameter area the callee may write
 * Nothing of ours lives in 4, 8 or 24..55. Our own return address is kept
 * in the SVR4 LR save word of OUR caller's frame (4 bytes above it), which
 * the callee can't reach. r12 = the transition vector, as CFM callers set.
 */
    .section .text
    .globl  cfm_call
    .type   cfm_call, @function
cfm_call:
    mflr    0
    stw     0, 4(1)
    stwu    1, -64(1)
    stw     2, 20(1)
    lwz     0, 0(3)
    lwz     2, 4(3)
    mr      12, 3
    mtctr   0
    mr      3, 4
    mr      4, 5
    mr      5, 6
    mr      6, 7
    mr      7, 8
    mr      8, 9
    mr      9, 10
    bctrl
    lwz     2, 20(1)
    addi    1, 1, 64
    lwz     0, 4(1)
    mtlr    0
    blr
    .size   cfm_call, . - cfm_call

    .section .note.GNU-stack,"",@progbits
