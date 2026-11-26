/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * guest/cfm/demo.c — a test fragment for the elf2pef + cfm_call path.
 * Converted to PEF, prepared at an arbitrary address and called through its
 * exported transition vectors by tests/guest/run_cfm.cpp. It calls a
 * Mac OS-style routine, imported as "TestLib:Callee", through cfm_call.
 */
#include "cfm.h"

extern struct cfm_tvector imp_Callee;

static int calls;
int table[4] = {10, 20, 30, 40};
int *const table_ptr = table;           /* an absolute pointer: relocated at preparation */

static int mix(int x) { return x * 7 + 1; }  /* a local call */

int demo_entry(int a, int b)
{
    volatile int keep[4];               /* stack contents that must survive the call */
    keep[0] = a;
    keep[1] = b;
    keep[2] = a + b;
    keep[3] = a - b;
    int r = (int)cfm_call(&imp_Callee, a, b, 3, 4, 5, 6, 7);
    calls++;
    return r + keep[0] + keep[1] * 2 + keep[2] * 3 + keep[3] * 4 + table_ptr[calls & 3] + mix(a);
}

int demo_calls(void) { return calls; }
