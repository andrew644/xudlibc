/***********************************************************************************************
 *
 *   xud_test_main.c - the runner of the split test run
 *
 *   Owns : the order the areas run in and the run's verdict. Nothing else - every check lives
 *          in an area file, every counter lives in xud_test_harness.c.
 *   Layer: tests. The areas run bottom of the layer ladder up: the shared vocabulary and the
 *          loader first, the context and its lifecycle last. Each area entry point runs its own
 *          pure half and, under the gate, its own GPU half, so this list is one line per area
 *          and the headless default stays green with no driver present. The GPU ladder of the
 *          bring-up (xud_test_gpu_bringup) sits between the device area and the window area,
 *          which is where the original single-file run ran it: the rungs that need a device are
 *          tested against a device before the rungs above it are tested at all.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include <stdio.h> /* Required for: printf, the banner and the summary the run prints */

#include "xud.h" /* Required for: XUD_VERSION, XUD_GPU_TESTS, XUD_ENV_GPU_TESTS */
#include "xud_test_harness.h"

int main(void) {
    const int gpu = xud_gpu_tests_enabled();

    printf("xudlibc %s test run: gpu gate %s (compile-time default %d)\n", XUD_VERSION, XUD_ENV_GPU_TESTS, XUD_GPU_TESTS);

    xud_test_vocabulary();
    xud_test_loader();
    xud_test_diagnostics();
    xud_test_instance();
    xud_test_physical();
    xud_test_device();
    xud_test_gpu_bringup();
    xud_test_window();
    xud_test_swapchain();
    xud_test_handle();
    xud_test_shader();
    xud_test_pipeline();
    xud_test_renderpass();
    xud_test_frame();
    xud_test_allocator();
    xud_test_descriptor();
    xud_test_core();

    printf("checks: %d  failures: %d\n", xudChecks, xudFailures);
    if (gpu == 0)
        printf("gpu checks: skipped (set XUD_GPU_TESTS=1 to run them)\n");

    return (xudFailures == 0) ? 0 : 1;
}
