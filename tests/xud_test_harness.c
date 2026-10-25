/***********************************************************************************************
 *
 *   xud_test_harness.c - the harness the split test areas share
 *
 *   Owns : the run's two counters, the one named check, and the GPU gate. No test of its own -
 *          every check lives in an area file, and every area reaches these three through
 *          xud_test_harness.h.
 *   Layer: tests. This is the single owner of the state the split moved out of one file: with
 *          seventeen TUs there is one place that counts checks and one place that decides
 *          whether the driver-backed half of an area runs. The gate keeps its original shape -
 *          a compile-time default (XUD_GPU_TESTS, from xud.h) overridden by the environment
 *          (XUD_ENV_GPU_TESTS) - so the headless default stays green on a machine with no
 *          driver and the GPU run stays opt-in.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include <stdio.h> /* Required for: printf, the FAIL line a refused check prints */
#include <stdlib.h> /* Required for: getenv, the environment half of the GPU gate */

#include "xud.h" /* Required for: XUD_GPU_TESTS, XUD_ENV_GPU_TESTS */
#include "xud_test_harness.h"

int xudChecks = 0;
int xudFailures = 0;

/* One named check. Failures are counted rather than fatal, so the whole run finishes. */
void xud_check(int condition, const char* name, const char* detail) {
    xudChecks++;
    if (condition)
        return;
    xudFailures++;
    printf("FAIL %-36s %s\n", name, detail);
}

/* The GPU gate: compile-time default XUD_GPU_TESTS, overridden by the environment. */
int xud_gpu_tests_enabled(void) {
    const char* env = getenv(XUD_ENV_GPU_TESTS);
    if (env == NULL)
        return XUD_GPU_TESTS;
    return (*env != '0') && (*env != '\0');
}
