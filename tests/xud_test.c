/***********************************************************************************************
 *
 *   xud_test.c - assert-based test harness
 *
 *   Owns : the test runner and the checks for every public function in xud.h plus the
 *          loader layer below it. Headless by default: this run must stay green on a
 *          machine with no Vulkan driver present - the loader checks degrade to a note
 *          when dlopen fails, they never fail because of a missing driver.
 *          GPU checks are opt-in via XUD_GPU_TESTS=1 (none exist before bring-up step S3).
 *   Layer: tests. Sees xud.h and the internal module headers of the layers under the public
 *          API (xud_loader.h) so the layers can be unit-tested on their own; user code
 *          includes xud.h only. xud_core.h stays private to the library.
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
#include "xud_loader.h"

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

/* The loader layer, unit-tested on its own: open, table contents, resolve, close. */
static void xud_test_loader_table(void)
{
    XudLoader *loader = NULL;
    const XudResult opened = xud_loader_open(&loader);

    if (opened == XUD_UNSUPPORTED) {
        printf("note: no Vulkan loader on this machine, loader checks skipped\n");
        return;
    }

    xud_check(opened == XUD_OK, "loader/open-ok", "loader open must succeed or say unsupported");
    xud_check(loader != NULL, "loader/non-null-out", "loader out-param must be set");
    xud_check(loader->library != NULL, "loader/handle-owned", "the dlopen handle must be owned");
    xud_check(loader->resolvedCount == XUD_LOADER_GLOBAL_SLOT_COUNT, "loader/table-populated",
              "the populated-slot count must be the global-level count at step S2");

    xud_check(loader->getInstanceProcAddr != NULL, "loader/seed-resolved", "seed PFN must be resolved");
    xud_check(loader->enumerateInstanceLayerProperties != NULL, "loader/global-slot",
              "instance-layer PFN must be resolved");
    xud_check(loader->enumerateInstanceExtensionProperties != NULL, "loader/global-slot",
              "instance-extension PFN must be resolved");

    /* Slots above step S2 must still be NULL: the table may not claim what was not resolved. */
    xud_check(loader->createInstance == NULL, "loader/later-slot-null",
              "instance-level slot must stay NULL at step S2");
    xud_check(loader->createDevice == NULL, "loader/later-slot-null",
              "device-level slot must stay NULL at step S2");
    xud_check(loader->createSwapchainKHR == NULL, "loader/later-slot-null",
              "swapchain slot must stay NULL at step S2");
    xud_check(loader->destroySurfaceKHR == NULL, "loader/later-slot-null",
              "surface teardown slot must stay NULL at step S2");

    /* An unknown entry point is reported, not guessed, and the out-param stays NULL. */
    {
        XudPfnBits bits;
        bits.function = NULL;
        xud_check(xud_loader_resolve(loader, VK_NULL_HANDLE, "vkNotAFunction", &bits) == XUD_UNSUPPORTED,
                  "loader/unknown-name", "an unknown entry point must be unsupported");
        xud_check(bits.function == NULL, "loader/unknown-name-out-null",
                  "a failed resolve must leave the out-param NULL");
    }

    /* A second open into an already-set out-param is rejected without touching it. */
    xud_check(xud_loader_open(&loader) == XUD_INVALID_ARG, "loader/open-non-null-out",
              "a non-null out-param on entry must be rejected, not clobbered");
    xud_check(loader != NULL, "loader/open-no-clobber",
              "a rejected open must leave the existing loader alone");

    xud_check(xud_loader_close(&loader) == XUD_OK, "loader/close-ok", "close must succeed");
    xud_check(loader == NULL, "loader/close-nulls", "close must set the pointer NULL");
}

/* The guard every call site relies on: a nil PFN is reported, never called. */
static void xud_test_loader_nil_guard(void)
{
    XudLoader loader;
    XudPfnBits bits;

    memset(&loader, 0, sizeof(loader)); /* a loader whose seed never resolved */
    bits.function = NULL;

    xud_check(xud_loader_resolve(&loader, VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties", &bits)
              == XUD_UNSUPPORTED, "loader/nil-pfn-guard", "a nil PFN must be reported, never called");
    xud_check(bits.function == NULL, "loader/nil-pfn-out-null", "out-param must stay NULL");

    xud_check(xud_loader_resolve(NULL, VK_NULL_HANDLE, "vkX", &bits) == XUD_INVALID_ARG,
              "loader/null-loader", "a null loader must be rejected, not dereferenced");
    xud_check(xud_loader_resolve(&loader, VK_NULL_HANDLE, NULL, &bits) == XUD_INVALID_ARG,
              "loader/null-name", "a null name must be rejected");
    xud_check(xud_loader_resolve(&loader, VK_NULL_HANDLE, "", &bits) == XUD_INVALID_ARG,
              "loader/empty-name", "an empty name must be rejected, not resolved to the seed");
    xud_check(xud_loader_resolve(&loader, VK_NULL_HANDLE, "vkX", NULL) == XUD_INVALID_ARG,
              "loader/null-out", "a null out-param must be rejected");

    {
        XudLoader *existing = NULL;
        xud_check(xud_loader_open(NULL) == XUD_INVALID_ARG, "loader/open-null-out",
                  "a null out-param must be rejected");
        xud_check(xud_loader_close(NULL) == XUD_INVALID_ARG, "loader/close-null",
                  "a null pointer must be rejected");
        xud_check(xud_loader_close(&existing) == XUD_ALREADY, "loader/close-already",
                  "closing an already-closed loader is XUD_ALREADY");
        xud_check(existing == NULL, "loader/close-leaves-null", "close must leave the pointer NULL");
    }
}

/* The public runtime query: it must answer ok or unsupported, never another code, and it
   must not cache - the answer is recomputed from a fresh dlopen each call. */
static void xud_test_vulkan_supported(void)
{
    const XudResult first = xud_vulkan_supported();
    const XudResult second = xud_vulkan_supported();

    xud_check((first == XUD_OK) || (first == XUD_UNSUPPORTED), "loader/supported-codes",
              "the runtime query must answer XUD_OK or XUD_UNSUPPORTED");
    xud_check(first == second, "loader/supported-stable", "two calls must agree on this machine");
    printf("note: xud_vulkan_supported() -> %s\n", xud_result_name(first));
}

/* Step S2 boundary: the loader opens and its table is filled, but instance / device / queue
   / surface do not exist yet - so a valid request must still report XUD_UNSUPPORTED rather
   than hand back a context that cannot draw. */
static void xud_test_init_bringup_boundary(void)
{
    XudContext *context = NULL;
    const XudResult result = xud_init_context(&context, 640, 480);

    xud_check(result == XUD_UNSUPPORTED, "init/bringup-incomplete",
              "valid args must yield XUD_UNSUPPORTED until bring-up step S3 lands");
    xud_check(context == NULL, "init/no-context-on-unsupported",
              "an incomplete bring-up must not return a context");

    /* The same must hold for the smallest and the largest accepted extents. */
    xud_check(xud_init_context(&context, 1, 1) == XUD_UNSUPPORTED, "init/min-extent",
              "1x1 must be accepted then reported unsupported");
    xud_check(context == NULL, "init/min-extent-null", "out-param must stay NULL");
    xud_check(xud_init_context(&context, XUD_MAX_WINDOW_EXTENT, XUD_MAX_WINDOW_EXTENT) ==
              XUD_UNSUPPORTED, "init/extent-at-limit", "the limit itself must be accepted");
    xud_check(context == NULL, "init/limit-extent-null", "out-param must stay NULL");
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

/* Every rejected and every incomplete init must free what it allocated - including the
   loader handle it opened before reporting the bring-up incomplete. The verdict is the
   sanitizer/valgrind report over these 1000 iterations (each one a dlopen/dlsym/dlclose
   cycle), not an assertion here. */
static void xud_test_no_leak(void)
{
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
    xud_test_loader_nil_guard();
    xud_test_loader_table();
    xud_test_vulkan_supported();
    xud_test_init_arg_validation();
    xud_test_init_bringup_boundary();
    xud_test_close();
    xud_test_no_leak();

    /* GPU-gated section: still empty at step S2 - the first GPU check arrives when step
       S3 creates an instance. The loader itself is not GPU-gated: dlopen needs no device,
       so the default headless run already exercises it. */
    if (gpu) printf("gpu checks: none yet (bring-up step S3 is the first)\n");
    else     printf("gpu checks: skipped (set XUD_GPU_TESTS=1 to run them)\n");

    printf("run complete: %d checks, %d failures\n", xudChecks, xudFailures);

    return xudFailures == 0 ? 0 : 1;
}
