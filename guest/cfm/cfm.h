/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * guest/cfm/cfm.h — calling Mac OS (Code Fragment Manager) routines from
 * guest C code built with the SVR4 compiler (powerpc-linux-gnu-gcc).
 *
 * Mac OS code follows the AIX-derived PowerPC runtime conventions (Inside
 * Macintosh: Mac OS Runtime Architectures, chapter 1): a routine is
 * reached through a transition vector {code address, TOC}; the caller
 * loads r2 from it, keeps r12 = the vector, and the callee may write the
 * caller's linkage area (CR at 4(SP), LR at 8(SP)) and the 32-byte
 * parameter area at 24(SP). SVR4 code doesn't provide those, so every call
 * to Mac OS goes through cfm_call (cfm_glue.S), never a direct call.
 *
 * Restrictions, in both directions (calls into Mac OS and our exports
 * called by it): integer and pointer arguments only, at most 7 through
 * cfm_call and 8 for exports; a 32-bit integer or pointer result. (The two
 * conventions place floating-point and stacked arguments differently.)
 *
 * Import a routine by declaring its transition vector as data,
 *     extern struct cfm_tvector imp_SynchronizeIO;
 * and naming it at conversion: --import imp_SynchronizeIO=DriverServicesLib:SynchronizeIO
 */
#ifndef G3_GUEST_CFM_H
#define G3_GUEST_CFM_H

struct cfm_tvector {
    unsigned long code;
    unsigned long toc;
};

unsigned long cfm_call(const struct cfm_tvector *tv, ...);

#endif /* G3_GUEST_CFM_H */
