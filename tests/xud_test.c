/***********************************************************************************************
 *
 *   xud_test.c - assert-based test harness
 *
 *   Owns : the test runner and the checks for every public function in xud.h. Headless by
 *          default: this run must stay green on a machine with no Vulkan driver present.
 *          GPU checks are opt-in via XUD_GPU_TESTS=1 (none exist before bring-up step S3).
 *   Layer: tests. Sees the public API only - xud.h, never xud_core.h.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xud.h"

static int xudChecks = 0;
static int xudFailures = 0;

/* One named check. Failures are counted rather than fatal, so the whole file runs. */
static void xud_check(int condition, const char *name, const char *detail)
{
    xudChecks++;
    if (condition) return;
    xudFailures++;
    printf("FAIL %-36s %s\n", name, detail);
}

/* The GPU gate: compile-time default XUD_GPU_TESTS, overridden by the environment. */
static int xud_gpu_tests_enabled(void)
{
    const char *env = getenv(XUD_ENV_GPU_TESTS);
    if (env == NULL) return XUD_GPU_TESTS;
    return (*env != '0') && (*env != '\0');
}

static void xud_test_version(void)
{
    xud_check(XUD_VERSION_MAJOR == 0, "version/major", "XUD_VERSION_MAJOR is not 0");
    xud_check(XUD_VERSION_MINOR == 1, "version/minor", "XUD_VERSION_MINOR is not 1");
    xud_check(XUD_VERSION_PATCH == 0, "version/patch", "XUD_VERSION_PATCH is not 0");
    xud_check(strlen(XUD_VERSION) > 0, "version/string", "XUD_VERSION is empty");
    xud_check(strcmp(XUD_VERSION, "0.1") == 0, "version/consistent",
              "XUD_VERSION does not match the major.minor macros");
}

static void xud_test_result_names(void)
{
    const XudResult all[] = { XUD_OK, XUD_UNSUPPORTED, XUD_INVALID_ARG, XUD_OUT_OF_MEMORY,
                              XUD_OUT_OF_RESOURCES, XUD_ALREADY, XUD_LOST, XUD_VULKAN_ERROR,
                              XUD_INTERNAL_ERROR };
    const size_t count = sizeof(all) / sizeof(all[0]);

    xud_check(XUD_OK == 0, "result/ok-is-zero", "XUD_OK is not 0");

    for (size_t i = 0; i < count; i++) {
        const char *name = xud_result_name(all[i]);
        xud_check(name != NULL && name[0] != '\0', "result/name-non-null", "empty name");
        xud_check(strcmp(name, "XUD_OUT_OF_RANGE") != 0, "result/name-known",
                  "an enum member falls through to the out-of-range name");
        for (size_t j = i + 1; j < count; j++) {
            xud_check(strcmp(name, xud_result_name(all[j])) != 0, "result/names-unique",
                      "two error codes share a name");
        }
    }
}

static void xud_test_init_arg_validation(void)
{
    XudContext *context = NULL;

    xud_check(xud_init_context(NULL, 640, 480) == XUD_INVALID_ARG, "init/null-out-param",
              "a null out-param must be rejected, not dereferenced");

    xud_check(xud_init_context(&context, 0, 480) == XUD_INVALID_ARG, "init/zero-width",
              "zero width must be rejected");
    xud_check(xud_init_context(&context, 640, 0) == XUD_INVALID_ARG, "init/zero-height",
              "zero height must be rejected");
    xud_check(xud_init_context(&context, XUD_MAX_WINDOW_EXTENT + 1u, 480) == XUD_INVALID_ARG,
              "init/extent-too-large", "an absurd extent must be rejected host-side");
    xud_check(xud_init_context(&context, XUD_MAX_WINDOW_EXTENT, XUD_MAX_WINDOW_EXTENT) ==
              XUD_UNSUPPORTED, "init/extent-at-limit", "the limit itself must be accepted");

    xud_check(context == NULL, "init/out-param-stays-null",
              "every failure path must leave the out-param NULL");
}

static void xud_test_init_stub_boundary(void)
{
    XudContext *context = NULL;
    const XudResult result = xud_init_context(&context, 640, 480);

    /* Step S1 boundary: the shell allocates, but nothing below it exists (no loader, no
       instance, no device, no surface) - so a valid request must report XUD_UNSUPPORTED
       rather than hand back a context that cannot draw. */
    xud_check(result == XUD_UNSUPPORTED, "init/stub-boundary",
              "valid args must yield XUD_UNSUPPORTED until bring-up step S2 lands");
    xud_check(context == NULL, "init/no-context-on-unsupported",
              "an unsupported bring-up must not return a context");

    /* The same must hold for the smallest and the largest accepted extents. */
    xud_check(xud_init_context(&context, 1, 1) == XUD_UNSUPPORTED, "init/min-extent",
              "1x1 must be accepted then reported unsupported");
    xud_check(context == NULL, "init/min-extent-null", "out-param must stay NULL");
}

static void xud_test_close(void)
{
    XudContext *context = NULL;

    xud_check(xud_close_context(NULL) == XUD_INVALID_ARG, "close/null-out-param",
              "a null out-param must be rejected, not dereferenced");
    xud_check(xud_close_context(&context) == XUD_ALREADY, "close/already-closed",
              "closing a NULL context is XUD_ALREADY, not a crash and not XUD_OK");
    xud_check(context == NULL, "close/leaves-null", "closing a NULL context keeps it NULL");
}

static void xud_test_no_leak(void)
{
    /* Every rejected and every stubbed init must free what it allocated. The verdict is
       the sanitizer/valgrind report over these 1000 iterations, not an assertion here. */
    for (int i = 0; i < 1000; i++) {
        XudContext *context = NULL;
        (void)xud_init_context(&context, 64u + (uint32_t)i, 480u);
        (void)xud_close_context(&context);
    }
    xud_check(1, "no-leak/ran", "1000 init+close iterations completed");
}

int main(void)
{
    const int gpu = xud_gpu_tests_enabled();

    printf("xudlibc %s test run: gpu gate %s (compile-time default %d)\n",
           XUD_VERSION, XUD_ENV_GPU_TESTS, XUD_GPU_TESTS);

    xud_test_version();
    xud_test_result_names();
    xud_test_init_arg_validation();
    xud_test_init_stub_boundary();
    xud_test_close();
    xud_test_no_leak();

    /* GPU-gated section: empty until bring-up step S3 creates an instance. */
    if (gpu) printf("gpu checks: none yet (bring-up step S3 is the first)\n");
    else     printf("gpu checks: skipped (set XUD_GPU_TESTS=1 to run them)\n");

    printf("run complete: %d checks, %d failures\n", xudChecks, xudFailures);

    return xudFailures == 0 ? 0 : 1;
}
