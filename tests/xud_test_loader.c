/***********************************************************************************************
 *
 *   xud_test_loader.c - the loader layer: Volk's two tables behind one per-context wrapper
 *
 *   Owns : the loader's own open/close, the contents of its two Volk tables, the two bind rungs, and the guard every call site relies on -
 * a nil entry-point row is reported, never called. Layer: tests. Sees xud_loader.h: the layer below every other area, and the one area
 * whose checks degrade to a note when dlopen() finds no driver.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
#include "xud_loader.h" /* Required for: XudLoader, xud_loader_open/close, the two bind rungs and the bound-handle queries */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */
#include <stdio.h> /* Required for: printf, the note this area prints when a guard has nothing to check */
#include <string.h> /* Required for: memset, the synthetic rows this area zeroes and compares */

/* The loader layer, unit-tested on its own: open, table contents, the two bind rungs, close. */
static void xud_test_loader_table(void) {
    XudLoader* loader = NULL;
    const XudResult opened = xud_loader_open(&loader);

    if (opened == XUD_UNSUPPORTED) {
        printf("note: no Vulkan loader on this machine, loader checks skipped\n");
        return;
    }

    xud_check(opened == XUD_OK, "loader/open-ok", "loader open must succeed or say unsupported");
    xud_check(loader != NULL, "loader/non-null-out", "loader out-param must be set");
    xud_check(loader->open == true, "loader/open-flag", "an opened loader says it is open");
    xud_check(xud_loader_is_open(loader) == true, "loader/is-open", "an opened loader is open");
    xud_check(loader->boundInstance == VK_NULL_HANDLE, "loader/no-instance-yet", "no instance-level rows before an instance exists");
    xud_check(loader->boundDevice == VK_NULL_HANDLE, "loader/no-device-yet", "and no device-level rows before a device exists");

    /* The three loader-level rows: commands with no handle in hand, so volkInitialize() an-
       swered them before xud_loader_open() accepted the loader. The count check that used to s
       it in this test lives in the rung now, and this test reads the three rows it has to read.
       vkGetInstanceProcAddr, the seed of the hand-rolled bring-up this replaced, is Volk's own
       global and is not a row of this struct - nothing in xudlibc calls through it. */
    xud_check(loader->createInstance != NULL, "loader/loader-row-create", "the create row is resolved at loader open");
    xud_check(loader->enumerateInstanceLayerProperties != NULL, "loader/loader-row-layers", "instance-layer row must be resolved");
    xud_check(loader->enumerateInstanceExtensionProperties != NULL, "loader/loader-row-exts", "instance-extension row must be resolved");

    /* Both tables are still all-NULL: opening the loader may not claim what was not resolved t
       hrough a handle. The instance layer fills the instance table at xud_loader_bind_instance,
       the device layer fills the device table at xud_loader_bind_device - not here. */
    xud_check(loader->instanceTable.vkDestroyInstance == NULL, "loader/instance-row-null-at-open",
        "instance-level row must stay NULL at loader open");
    xud_check(loader->instanceTable.vkCreateDevice == NULL, "loader/create-row-null-at-open",
        "the create row is instance-level and stays NULL until the instance bind");
    xud_check(loader->deviceTable.vkCreateSwapchainKHR == NULL, "loader/swapchain-row-null-at-open",
        "swapchain row must stay NULL at loader open");
    xud_check(loader->instanceTable.vkDestroySurfaceKHR == NULL, "loader/surface-row-null-at-open",
        "surface teardown row must stay NULL at loader open");

    /* A bind with no handle in hand is an argument error, and the tables stay untouched: the t
       wo rungs replace the per-name resolve that used to report an unknown entry point. */
    xud_check(xud_loader_bind_instance(loader, VK_NULL_HANDLE) == XUD_INVALID_ARG, "loader/bind-instance-null",
        "a bind with no handle must be rejected");
    xud_check(xud_loader_bind_device(loader, VK_NULL_HANDLE) == XUD_INVALID_ARG, "loader/bind-device-null",
        "a bind with no device must be rejected");
    xud_check(xud_loader_bind_device(loader, (VkDevice)(uintptr_t)1u) == XUD_UNSUPPORTED, "loader/bind-device-needs-instance",
        "a device table cannot be filled before an instance table exists - the device rung res"
        "olves through this loader's own vkGetDeviceProcAddr row, so no instance means no row");
    xud_check(loader->deviceTable.vkDestroyDevice == NULL, "loader/bind-device-left-rows-null",
        "and a refused device bind must leave every device-level row NULL");
    xud_check(
        loader->instanceTable.vkDestroyInstance == NULL, "loader/bind-rejected-untouched", "a rejected bind must leave the table alone");

    /* A second open into an already-set out-param is rejected without touching it. */
    xud_check(xud_loader_open(&loader) == XUD_INVALID_ARG, "loader/open-non-null-out",
        "a non-null out-param on entry must be rejected, not clobbered");
    xud_check(loader != NULL, "loader/open-no-clobber", "a rejected open must leave the existing loader alone");

    xud_check(xud_loader_close(&loader) == XUD_OK, "loader/close-ok", "close must succeed");
    xud_check(loader == NULL, "loader/close-nulls", "close must set the pointer NULL");
}

/* The guard every call site relies on: a nil PFN is reported, never called. */
static void xud_test_loader_nil_guard(void) {
    XudLoader loader;
    const VkInstance fake = (VkInstance)(uintptr_t)1u;
    const VkDevice fakeDevice = (VkDevice)(uintptr_t)1u;

    memset(&loader, 0, sizeof(loader)); /* a loader whose rows never resolved */

    xud_check(xud_loader_is_open(NULL) == false, "loader/is-open-null", "a null loader is not open");
    xud_check(xud_loader_is_open(&loader) == false, "loader/is-open-zeroed", "a loader whose rows never resolved is not open");

    /* The handles below are NOT real VkInstance/VkDevice handles and must never be dereferenc
       ed: the point of every check here is that the guard answers before the call, and a mi
       ssing guard would call through a NULL row and crash the run. */
    xud_check(xud_loader_instance_bound(&loader, fake) == false, "loader/bound-nothing", "a zeroed loader is bound to no instance");
    xud_check(xud_loader_device_bound(&loader, fakeDevice) == false, "loader/device-bound-nothing", "and to no device");
    xud_check(xud_loader_instance_bound(&loader, VK_NULL_HANDLE) == false, "loader/bound-not-null-handle", "and never to the null handle");
    xud_check(xud_loader_bind_instance(&loader, fake) == XUD_INVALID_ARG, "loader/bind-closed",
        "a closed loader cannot bind, before anything else");
    xud_check(xud_loader_bind_device(&loader, fakeDevice) == XUD_INVALID_ARG, "loader/bind-device-closed",
        "and neither can it fill a device table");
    xud_check(xud_loader_bind_instance(NULL, fake) == XUD_INVALID_ARG, "loader/bind-null-loader",
        "a null loader must be rejected, not dereferenced");
    xud_check(xud_loader_bind_device(NULL, fakeDevice) == XUD_INVALID_ARG, "loader/bind-device-null-loader",
        "the device rung guards the same way");
    xud_check(loader.instanceTable.vkDestroyInstance == NULL, "loader/bind-left-rows-null",
        "a refused bind must leave every instance-level row NULL");
    xud_check(loader.deviceTable.vkDestroyDevice == NULL, "loader/bind-left-device-rows-null", "...including the device-level ones");

    {
        XudLoader* existing = NULL;
        xud_check(xud_loader_open(NULL) == XUD_INVALID_ARG, "loader/open-null-out", "a null out-param must be rejected");
        xud_check(xud_loader_close(NULL) == XUD_INVALID_ARG, "loader/close-null", "a null pointer must be rejected");
        xud_check(xud_loader_close(&existing) == XUD_ALREADY, "loader/close-already", "closing an already-closed loader is XUD_ALREADY");
        xud_check(existing == NULL, "loader/close-leaves-null", "close must leave the pointer NULL");
    }
}

void xud_test_loader(void) {
    xud_test_loader_nil_guard();
    xud_test_loader_table();
}
