/***********************************************************************************************
 *
 *   xud_test_core.c - the context: the public query, the bring-up ladder, the one teardown order
 *
 *   Owns : xud_vulkan_supported() as a real runtime query, the argument guards of xud_init_context(), the bring-up boundary, the extent gate, the idempotent
 * close, the rejected-extent leak loop - and the GPU half that runs the whole lifecycle against the driver, 25 create+destroy cycles of a
 * real window and a real VkDevice. Layer: tests. Sees xud_core.h: the ownership step is about what a context owns, and the only way to say
 * that in a test is to read the fields it owns them through. User code still never sees them.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
#include "xud_core.h" /* Required for: XudContext, xud_init_context/xud_close_context, and every field the ownership checks read */
#include "xud_loader.h" /* Required for: xud_loader_instance_bound/device_bound/is_open, the guards the ownership checks read */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */
#include "xud_vulkan.h" /* Required for: xud_name_equal, the dedup check the merged instance list is read with */
#include "xud_window.h" /* Required for: xud_window_presentation_support, the refusal half of the ladder */
#include <limits.h> /* Required for: INT_MIN and INT_MAX, the one span too wide for an int subtraction */
#include <stdio.h> /* Required for: printf, the note this area prints when a guard has nothing to check */
#include <string.h> /* Required for: memset, the context shell the generator checks run against */

static void xud_test_init_arg_validation(void) {
    XudContext* context = NULL;

    xud_check(xud_init_context(NULL, 640, 480, "xud_test") == XUD_INVALID_ARG, "init/null-out-param",
        "a null out-param must be rejected, not dereferenced");

    xud_check(xud_init_context(&context, 0, 480, "xud_test") == XUD_INVALID_ARG, "init/zero-width", "zero width must be rejected");
    xud_check(xud_init_context(&context, 640, 0, "xud_test") == XUD_INVALID_ARG, "init/zero-height", "zero height must be rejected");
    xud_check(xud_init_context(&context, XUD_MAX_WINDOW_EXTENT + 1u, 480, "xud_test") == XUD_INVALID_ARG, "init/extent-too-large",
        "an absurd extent must be rejected host-side");
    xud_check(xud_init_context(&context, 0, XUD_MAX_WINDOW_EXTENT + 1u, "xud_test") == XUD_INVALID_ARG, "init/zero-height-over-limit",
        "both halves bad at once is still one bad argument");
    xud_check(xud_init_context(&context, 640, 480, NULL) == XUD_INVALID_ARG, "init/null-title", "a null title must be rejected");

    /* The limit itself is not checked here: it is inside the argument bound, so it reaches the
       windo w layer and a real window is opened, which is the boundary test's job, not this one's.
       The bound r elation that makes the limit accepted is checked in
       xud_test_init_bringup_boundary().
     */
    xud_check(context == NULL, "init/out-param-stays-null", "every failure path above must leave the out-param NULL");
}

/* The public runtime query: it must answer ok or unsupported, never another code, and it
   must not cache - the answer is recomputed from a fresh dlopen each call. */
static void xud_test_vulkan_supported(void) {
    const XudResult first = xud_vulkan_supported();
    const XudResult second = xud_vulkan_supported();

    xud_check((first == XUD_OK) || (first == XUD_UNSUPPORTED), "loader/supported-codes",
        "the runtime query must answer XUD_OK or XUD_UNSUPPORTED");
    xud_check(first == second, "loader/supported-stable", "two calls must agree on this machine");
    printf("note: xud_vulkan_supported() -> %s\n", xud_result_name(first));
}

/* Bring-up boundary, S8's shape: the context owns loader/diagnostics/window/instance/physica
   l/device/surface now, so a valid request completes on a machine that has both a Vulkan loader an
   d a window system, and is refused XUD_UNSUPPORTED on one that has either missing. Which of the tw
   o this machine is cannot be assumed - that is the whole point of the headless run - and what has
   t o hold either way is the out-param contract: a refused request hands back nothing, and a
   complete d one hands back a context that closes back to nothing. The full create-then-destroy
   pass, with th e ownership checks and the validation-clean requirement, is xud_test_gpu_context()
   below the gate; here stays the boundary, and at most two real windows are opened by it. */
static void xud_test_init_bringup_boundary(void) {
    XudContext* context = NULL;
    XudResult smallest = XUD_INVALID_ARG;
    const XudResult result = xud_init_context(&context, 640, 480, "xud_test");

    xud_check((result == XUD_OK) || (result == XUD_UNSUPPORTED), "init/complete-or-refused",
        "a valid request either completes to a context or is refused outright, never a"
        " half-built one");
    if (result == XUD_OK) {
        xud_check(context != NULL, "init/ok-hands-context", "a completed bring-up hands back a context");
        xud_check(xud_close_context(&context) == XUD_OK, "init/ok-closes-clean", "and it tears back down with nothing refused");
        xud_check(context == NULL, "init/ok-cleared", "and the out-param is cleared by the teardown");
    } else {
        xud_check(context == NULL, "init/no-context-on-refusal", "a refused request must not return a context");
    }

    /* The smallest accepted extent: inside the argument bound, so not a bad argument, and whatev
       er the window system makes of a 1x1 window is that system's answer, not a library failure. A
       comp letion must close back to nothing; a refusal must hand back nothing. Both are the same
       contract the 640x480 call above is checking, at the other end of the accepted range.
     */
    smallest = xud_init_context(&context, 1, 1, "xud_test");
    xud_check(smallest != XUD_INVALID_ARG, "init/min-extent-accepted", "1x1 is inside the argument bound, so it is not a bad argument");
    if (smallest == XUD_OK) {
        xud_check(context != NULL, "init/min-extent-hands-context", "a 1x1 window the compositor accepted comes back as a context");
        xud_check(xud_close_context(&context) == XUD_OK, "init/min-extent-closes", "and it tears back down with nothing refused");
    } else {
        xud_check(context == NULL, "init/min-extent-null", "a 1x1 window the compositor refused hands back nothing");
    }

    /* The extent bound, without creating a window at the limit: XUD_MAX_WINDOW_EXTENT is what the
       argument check accepts up to, XUD_WINDOW_MAX_EXTENT is what GLFW's int extents cap at, and th
       e first sitting inside the second is the whole reason the limit itself is not a bad argument.
       Th e compositor's answer to a 16384x16384 window is not a property of this library, so it is
       not aske d here.
     */
    xud_check(XUD_MAX_WINDOW_EXTENT <= XUD_WINDOW_MAX_EXTENT, "init/extent-limit-inside-glfw-bound",
        "the accepted limit must be inside GLFW's own int bound");
    xud_check(xud_init_context(&context, XUD_MAX_WINDOW_EXTENT + 1u, XUD_MAX_WINDOW_EXTENT, "xud_test") == XUD_INVALID_ARG,
        "init/limit-extent-rejected", "one above the limit is a bad argument");
    xud_check(context == NULL, "init/limit-extent-null", "and nothing was handed back");

    /* The extent gate itself, pure: the effective bound is the smaller of the public bound and the
       device's 2D image limit, so the device's answer only ever tightens the public one - and both
       halves of that, plus the per-axis form, are the rows below. */
    xud_check(xud_window_extent_ok(XUD_MAX_WINDOW_EXTENT, XUD_MAX_WINDOW_EXTENT, XUD_MAX_WINDOW_EXTENT) == true, "gate/at-both-bounds",
        "an extent at both bounds is legal");
    xud_check(xud_window_extent_ok(XUD_MAX_WINDOW_EXTENT, 1u, 32768u) == true, "gate/host-bound-governs",
        "a device limit above the public bound does not loosen it: the public bound still governs");
    xud_check(xud_window_extent_ok(XUD_MAX_WINDOW_EXTENT + 1u, 1u, 32768u) == false, "gate/above-host-bound",
        "one above the public bound is refused even where the device would allow it");
    xud_check(xud_window_extent_ok(8192u, 8192u, 8192u) == true, "gate/at-device-bound", "an extent at the device's own limit is legal");
    xud_check(xud_window_extent_ok(8193u, 1u, 8192u) == false, "gate/above-device-bound",
        "one above the device's limit is refused even though it sits inside the public bound");
    xud_check(xud_window_extent_ok(8192u, 8193u, 8192u) == false, "gate/above-device-bound-other-axis",
        "and so is the other axis at the same limit");
}

static void xud_test_close(void) {
    XudContext* context = NULL;
    bool shouldClose = true;

    xud_check(xud_close_context(NULL) == XUD_INVALID_ARG, "close/null-out-param", "a null out-param must be rejected, not dereferenced");
    xud_check(xud_close_context(&context) == XUD_ALREADY, "close/already-closed",
        "closing a NULL context is XUD_ALREADY, not a crash and not XUD_OK");
    xud_check(context == NULL, "close/leaves-null", "closing a NULL context keeps it NULL");

    /* The window's close query, the headless half: both guards run before the window is reached, and
       a refusal leaves the out-param as it found it. */
    xud_check(
        xud_window_should_close(NULL, NULL) == XUD_INVALID_ARG, "should-close/null-out", "an out-param that cannot be written is refused");
    xud_check(shouldClose == true, "should-close/refusal-untouched", "and the refusal left it alone");
    xud_check(xud_window_should_close(&shouldClose, NULL) == XUD_INVALID_ARG, "should-close/null-context", "and so is no context at all");
    xud_check(shouldClose == true, "should-close/null-context-untouched", "still as it found it");

    /* The window's move, the headless half: the only guard that runs without a context is the
       context's own, before the window rung is reached. */
    xud_check(xud_set_window_size(NULL, 640u, 480u) == XUD_INVALID_ARG, "set-size/null-context",
        "a NULL context is a bad argument, before any window is moved");
}

/* Every rejected init must free what it allocated. Rejected extents only here, and that is a
   change from the run before S8: a valid request completes now, so a thousand iterations of thos
   e would be a thousand real GLFW windows and a thousand real VkDevices rather than the argument c
   heck this loop is about. The full-cycle leak verdict is xud_test_gpu_context()'s, under the gate,
   where the same loop runs a small number of times and the sanitizer report over that run is the ve
   rdict (AGENTS.md records what that run reports, including the one external libdecor allocation th
   at is not ours). Nothing above here is reached: both calls are refused before a single allocation
   is made, so the loop is deterministic and driver-free either way. */
static void xud_test_no_leak(void) {
    for (int i = 0; i < 1000; i++) {
        XudContext* context = NULL;
        (void)xud_init_context(&context, 0u, 480u, "xud_test");
        (void)xud_close_context(&context);
        (void)xud_init_context(&context, XUD_MAX_WINDOW_EXTENT + 1u, 480u, "xud_test");
        (void)xud_close_context(&context);
        (void)xud_init_context(&context, 480u, XUD_MAX_WINDOW_EXTENT + 1u, "xud_test");
        xud_check(context == NULL, "no-leak/rejected-hands-nothing", "a refused init must leave the out-param NULL, on every iteration");
        (void)xud_close_context(&context);
    }
    xud_check(1, "no-leak/ran", "1000 rejected init+close iterations completed");
}

/* How many full create+destroy cycles the GPU test below runs. Small on purpose: each one is a
   real GLFW window on this machine's Wayland session and a real VkDevice, so the count is a time an
   d a screen-flicker budget, not a coverage one - the verdict it exists for is the sanitizer report
   o ver the run, and 25 cycles of the whole ladder is 25 chances to catch a leak in it. */
#define XUD_TEST_GPU_CONTEXT_CYCLES 25

/* The seeded context shell the generator checks run against, defined below with them; declared here
   because the GPU half reads a fresh context's generator against one too.
*/
static void xud_test_random_shell(XudContext* shell, uint32_t seed);

/* GPU-gated: the whole context lifecycle against the real driver - S8's ownership step, grown by
   the swapchain rung. This is the one place in the run that creates a context, so it is the one
   place that can say what a context owns: a loader, a diagnostics struct when validation was asked
   for, a GLFW window, a VkInstance carrying that window's required instance extensions, a physical
   device queried through THAT instance, a VkDevice created from that device's recorded caps with b
   oth role queues resolved, a VkSurfaceKHR for the window inside that window, and the XudSwapchai
   n created for that surface on that device. The allocator, the descriptor layer, the batch and t
   he renderer are all in the tree now, and no field of XudContext is a sentinel. The teardown
   is xud_close_context()'s order (swapchain -> device -> surface -> window -> physical -> instance
   -> diagnostics -> loader) and the run has to be validation-clean, not just crash-free: the mess
   enger is installed, so a validation complaint would land in the callback and the error counter b
   elow must stay zero. The repeated create+destroy loop is here for the same reason xud_test_no_le
   ak() exists - the verdict is the sanitizer report over these cycles, and
   each one i s a real window and a real device, so the count is small. */
static void xud_test_gpu_context(void) {
    XudContext* context = NULL;
    XudResult result = XUD_OK;
    int cycle = 0;

    result = xud_init_context(&context, 320u, 240u, "xud_test");
    xud_check(result == XUD_OK, "gpu-context/bringup-completes",
        "this machine has a Vulkan loader and a Wayland session, so the ladder must complete");
    if (context == NULL)
        return;

    xud_check(context->width > 0u, "context/extent-width", "the extent is live, not zeroed");
    xud_check(context->height > 0u, "context/extent-height", "and so is the other half of it");
    xud_check(context->width == context->swapchain->createInfo.imageExtent.width
            && context->height == context->swapchain->createInfo.imageExtent.height,
        "context/extent-matches-swapchain",
        "the recorded extent is the swapchain's: a compositor that resizes the window before the first "
        "present (a tiling layout) leaves the swapchain at the compositor's size, and the ui fold and "
        "the camera's aspect divide by the recorded extent, so a record that kept the requested size "
        "would draw at the old scale while input names the new one");
    xud_check(context->validate == XUD_VALIDATE, "context/validate-snapshot",
        "the context carries the XUD_VALIDATE build switch it was built with, as a snapshot");

    /* The generator a context is BORN on: the documented default seed, read the only two ways it can
       be read - the field bring-up wrote, and the sequence the first draw of a fresh context answers
       against the first draw of a shell put on that seed by hand. A context that never got its seed
       would answer the shell's number for neither.
    */
    xud_check(context->randomSeed == XUD_RANDOM_DEFAULT_SEED, "context/generator-on-default-seed",
        "bring-up leaves the generator on the seed xud.h states, not on zero");
    {
        XudContext shell;
        int born = 0;
        int handSeeded = 0;

        xud_test_random_shell(&shell, XUD_RANDOM_DEFAULT_SEED);
        xud_check(xud_get_random_value(&born, context, INT_MIN, INT_MAX) == XUD_OK, "context/generator-draws",
            "a live context answers a draw from its own generator");
        xud_check(xud_get_random_value(&handSeeded, &shell, INT_MIN, INT_MAX) == XUD_OK, "context/generator-shell-draws",
            "and so does a shell handed the same seed");
        xud_check(born == handSeeded, "context/generator-sequence-is-the-seeds",
            "and the two first draws are one number, so a fresh context's sequence is the default seed's sequence");
    }

    xud_check(context->loader != NULL, "context/owns-loader", "the context owns its loader");
    xud_check(
        xud_loader_is_open(context->loader), "context/loader-open", "and it is open, which is the invariant every layer above checks");

    xud_check(context->window != NULL, "context/owns-window", "a context that completed owns a GLFW window");
    xud_check(context->window->glfwWindow != NULL, "context/window-live", "and GLFW's own window handle is live in it");
    xud_check(context->window->width == 320u, "context/window-extent", "the window was opened at the context's extent");
    xud_check(context->window->requiredInstanceExtensionCount == 2u, "context/window-required-names",
        "GLFW's Wayland backend asked for exactly two instance-level names, recorded at open time");

    xud_check(context->instance != NULL, "context/owns-instance", "and an instance");
    xud_check(context->instance->instance != VK_NULL_HANDLE, "context/instance-live", "whose VkInstance handle is live");
    xud_check(xud_loader_instance_bound(context->loader, context->instance->instance), "context/loader-bound-to-own-instance",
        "and the loader's instance rows are this instance's rows, not another context's");

    /* The load-bearing fact from S7, one rung up: glfwCreateWindowSurface resolves vkCreateWayland
       SurfaceKHR through vkGetInstanceProcAddr against the instance it is handed, so the instance a
        surface is created for has to carry GLFW's window-system name. VK_KHR_surface is wanted by
       the instance layer already, so it must appear exactly once - the dedup is what makes a merged
       insta nce enable four names here, not five.
     */
    {
        bool waylandName = false;
        uint32_t surfaceRows = 0u;
        uint32_t row = 0u;
        for (row = 0u; row < context->instance->enabledExtensionCount; row++) {
            if (xud_name_equal(context->instance->enabledExtensionNameStorage[row], "VK_KHR_wayland_surface"))
                waylandName = true;
            if (xud_name_equal(context->instance->enabledExtensionNameStorage[row], VK_KHR_SURFACE_EXTENSION_NAME))
                surfaceRows++;
        }
        xud_check(waylandName, "context/wayland-name-enabled", "the window system's own name is in the instance's enabled list");
        xud_check(surfaceRows == 1u, "context/surface-name-not-duplicated",
            "VK_KHR_surface is wanted by the instance layer and by GLFW, and is enabled once");
        xud_check(context->instance->enabledExtensionCount >= 4u, "context/enabled-count-merged",
            "this layer's three names plus GLFW's wayland name, at least");
    }

    xud_check(context->physicalDevice != NULL, "context/owns-physical", "and a physical device");
    xud_check(context->physicalDevice->physicalDevice != VK_NULL_HANDLE, "context/physical-live", "whose handle is live");
    xud_check(context->physicalDevice->caps.queueFamilyIndex != XUD_PHYSICAL_NO_QUEUE_FAMILY, "context/queue-family-chosen",
        "and the physical layer chose a graphics family, which is what the device row below uses");

    xud_check(context->device != NULL, "context/owns-device", "and a VkDevice");
    xud_check(context->device->device != VK_NULL_HANDLE, "context/device-live", "whose handle is live");
    xud_check(context->device->queueRows[0].queueFamilyIndex == context->physicalDevice->caps.queueFamilyIndex,
        "context/device-row-is-the-chosen-family", "the device was created for the family the physical layer chose, not another one");
    xud_check(context->device->queueRows[0].pQueuePriorities == context->device->queuePriorities, "context/priorities-live",
        "the row's pointer points into the owning struct, which is what keeps it valid past the "
        "call");
    xud_check(context->device->graphicsQueue.queue != VK_NULL_HANDLE, "context/graphics-queue-resolved", "the graphics role resolved");
    xud_check(context->device->presentQueue.queue != VK_NULL_HANDLE, "context/present-queue-resolved",
        "and so did the present role - the same handle when the family rowed one queue, which is "
        "the case here");
    xud_check(xud_loader_device_bound(context->loader, context->device->device), "context/loader-bound-to-own-device",
        "and the loader's device rows are this device's rows");

    xud_check(
        context->window->surface != VK_NULL_HANDLE, "context/surface-live", "the context owns a VkSurfaceKHR, inside the window it owns");
    xud_check(context->window->surfaceInstance == context->instance->instance, "context/surface-names-own-instance",
        "and it names the context's own instance, which is the handle vkDestroySurfaceKHR needs "
        "alive");

    xud_check(context->swapchain != NULL, "context/swapchain-owned",
        "the context owns the XudSwapchain the surface was created for, and it is live");
    xud_check(context->swapchain->swapchain != VK_NULL_HANDLE, "context/swapchain-handle-live",
        "and the VkSwapchainKHR inside it is live, so the bring-up completed all the way down");
    xud_check(context->swapchain->imageCount >= 2u, "context/swapchain-images",
        "and its images were queried, so the renderer above has something to bind");
    xud_check(context->swapchain->createInfo.surface == context->window->surface, "context/swapchain-names-own-surface",
        "the swapchain names the surface inside the window the context owns");
    xud_check(context->handleTable != NULL, "context/handle-table-owned",
        "the context owns the handle table the public XudHandle of xud.h indexes, and it is the top of the tree");
    xud_check(context->handleTable->slots[XUD_HANDLE_KIND_TEXTURE] == NULL, "context/handle-table-empty",
        "and no kind of it was ever allocated, because no public call hands a handle out yet");
    xud_check(context->allocator != NULL, "context/allocator-owned",
        "the context owns the GPU-memory allocator, so no field of XudContext is a sentinel any more");
    xud_check(context->descriptor != NULL, "context/descriptor-owned",
        "and the descriptor-layout rung above it, so the pipeline layout the renderer built names re"
        "al sets and a real push row");
    xud_check(context->descriptor->pool != VK_NULL_HANDLE, "context/descriptor-pool-live",
        "the pool is live, and so are the two set layouts and set 0 allocated from it");
    xud_check(context->descriptor->sets[XUD_DESCRIPTOR_SET_MATERIAL] != VK_NULL_HANDLE, "context/descriptor-set-1-allocated",
        "and set 1 is allocated too, from the same pool against the same layout: an allocated sampler array whose entries were never "
        "written is an invalid descriptor, so the set and the white default behind every entry of it are this rung's, and the texture step "
        "is what puts a different view into a different row of it");
    xud_check(context->descriptor->whiteImageView != VK_NULL_HANDLE, "context/white-default-live",
        "and the one view behind all 17 rows of that array is live, which is what makes the array a slot axis and not a texture axis");
    xud_check(context->descriptor->uniformBuffer != VK_NULL_HANDLE, "context/descriptor-buffer-live",
        "the per-frame uniform buffer is live and bound to the allocator's memory");
    xud_check(context->allocator->blockCount == 7u, "context/allocator-five-blocks-now",
        "and the allocator owns seven VkDeviceMemory now: the descriptor rung's uniform buffer, the render pass's depth image - the "
        "image is the first thing above the allocator that asked for an IMAGE's requirements, and its alignment (65536 on this driver) "
        "does not fit a hole in the buffer's block, so it is a second block rather than a suballocation - the white default every "
        "entry of set 1's sampler array names, a second image whose alignment does not fit a hole in the depth image's block either, "
        "the renderer's scratch image and the scratch pass's own depth image - two more images, two more alignments - and the two "
        "host-visible arenas, the batch's and the descriptor's staging arena, which are blocks of their own because vkMapMemory maps a "
        "VkDeviceMemory rather than a range. The batch's five data buffers are the carves that do share a block, which is the "
        "suballocation half of the repo's allocate-once rule seen from above");
    xud_check(context->allocator->ringBlockIndex == XUD_ALLOCATOR_NO_BLOCK, "context/allocator-no-ring-yet",
        "and no ring block either, because nothing has asked for transient memory");

    if (context->validate != 0u) {
        xud_check(context->diagnostics != NULL, "context/validate-opens-diagnostics",
            "validation was asked for, so the diagnostics struct is there");
        xud_check(context->diagnostics->errorMessages == 0u, "context/validation-clean-so-far",
            "the run must be validation-clean, not just crash-free");
    } else {
        xud_check(context->diagnostics == NULL, "context/release-has-no-diagnostics",
            "a release build asks for no layer at all, and has no diagnostics struct");
    }

    /* GLFW's presentation query, asked again about the live triple so the gate that refused part-wa
       y through the ladder above is visible here rather than only implied by the fact that the
       ladder c ompleted. It is asked about the CHOSEN family, which is the present half of the
       one-family rule.
     */
    {
        bool supports = false;
        xud_check(xud_window_presentation_support(&supports, context->window, context->instance, context->physicalDevice) == XUD_OK,
            "gpu-context/presentation-query-answers", "GLFW's query runs for this window/device pair");
        xud_check(supports == true, "gpu-context/presentation-true",
            "this machine's window system can present to this device, which is why the ladder "
            "above completed");
    }

    /* The resize rung, last of the field checks because its recreation re-allocates the depth image
       and therefore moves the allocator's block count the checks above read. The frame boundary is
       where the recreation runs (the end renders the recreateWanted the acquire left), and the
       recorded extent has to follow the swapchain the resize rebuilt - a record that stayed at the
       resized-from size would scale the ui by the old extent while the input names the new one. The
       compositor is free to answer a resize with a different size than the one asked (a tiling
       layout re-tiles the window), so the check is the invariant against the swapchain's own extent
       rather than against 160x120. */
    {
        int frame = 0;
        for (frame = 0; frame < 6; frame++) {
            if (xud_begin_drawing(context) != XUD_OK)
                break;
            if (xud_end_drawing(context) != XUD_OK)
                break;
            xud_check(context->width == context->swapchain->createInfo.imageExtent.width
                    && context->height == context->swapchain->createInfo.imageExtent.height,
                "context/extent-follows-resize",
                "after every frame boundary the recorded extent is the extent the swapchain now renders "
                "at, so the ui fold divides by the size the input names");
            if (frame == 1) {
                (void)xud_set_window_size(context, 160u, 120u);
                xud_check(xud_set_window_size(context, 160u, 120u) == XUD_ALREADY, "context/set-size-identity",
                    "a second request at the extent the window record already has is a no-op, "
                    "not a refusal");
            }
        }
    }

    /* The extent gate against the machine's own caps: the extent the frame just rendered at sits
       inside it, one above the public bound is a bad argument at the public entry, and - on a
       machine whose 2D image limit sits below the public bound - one above the limit is a machine
       refusal at both entry points, the window staying at the extent it had. */
    {
        const uint32_t deviceLimit = context->physicalDevice->caps.properties.properties.limits.maxImageDimension2D;
        xud_check(deviceLimit >= 1u, "gpu-context/device-limit-recorded",
            "the physical layer recorded the device's own 2D image limit, which the gate below reads");
        xud_check(xud_window_extent_ok(context->width, context->height, deviceLimit), "gpu-context/rendered-extent-inside-gate",
            "the extent the frame just rendered at is legal under the machine's own limit");
        xud_check(xud_set_window_size(context, XUD_MAX_WINDOW_EXTENT + 1u, 120u) == XUD_INVALID_ARG,
            "gpu-context/set-size-over-public-bound",
            "one above the public bound is a bad argument at the public entry, before the window layer's int ceiling is reached");
        if (deviceLimit < XUD_MAX_WINDOW_EXTENT) {
            XudContext* oversized = NULL;
            xud_check(xud_set_window_size(context, deviceLimit + 1u, deviceLimit + 1u) == XUD_UNSUPPORTED,
                "gpu-context/set-size-over-device-bound", "above the device's 2D image limit is a machine refusal, not a bad argument");
            xud_check(context->window->width == 160u, "gpu-context/set-size-refusal-untouched",
                "and the refusal moved nothing: the window record still names the extent the resize above left");
            xud_check(xud_init_context(&oversized, deviceLimit + 1u, deviceLimit + 1u, "xud_test") == XUD_UNSUPPORTED,
                "gpu-context/init-over-device-bound",
                "and the same refusal at bring-up: the extent gate answers before the swapchain would have");
            xud_check(oversized == NULL, "gpu-context/init-over-device-bound-null", "and nothing was handed back");
        }
    }

    result = xud_close_context(&context);
    xud_check(result == XUD_OK, "context/close-clean", "the whole ladder tears back down with nothing refused");
    xud_check(context == NULL, "context/close-clears", "and the out-param is cleared");
    xud_check(xud_close_context(&context) == XUD_ALREADY, "context/close-twice", "a second teardown is reported, not a double free");

    /* Repeated full cycles, for the leak verdict rather than for an assertion: each one is a real
       wi ndow and a real device, created and destroyed. A cycle that this machine cannot complete
       is repor ted, not assumed - the loop's job is to run the ladder over and over, not to
       re-prove the checks abo ve.
     */
    for (cycle = 0; cycle < XUD_TEST_GPU_CONTEXT_CYCLES; cycle++) {
        XudContext* once = NULL;
        (void)xud_init_context(&once, 64u, 64u, "xud_test");
        if (once != NULL)
            (void)xud_close_context(&once);
        xud_check(once == NULL, "gpu-context/cycle-hands-nothing-back",
            "a cycle hands back nothing once it is closed, whether it completed or was refused");
    }
    xud_check(1, "gpu-context/cycles-ran", "the create+destroy cycles completed");
}

/*-----------------------------------------------------------------------------------------------
 * The context's own generator: one seed, one sequence, one range
 *
 * The generator is host arithmetic on two fields of the context, so every fact about it is
 * answerable with no device present - which is the same reason the camera and shapes areas keep
 * their geometry out of the GPU half. The one fact that needs a real context is that a freshly
 * brought-up context starts on the documented default seed, and that is the GPU half's.
 *
 * The checks are stated against the CONTRACT (a seed names one sequence; a value is inside the range
 * it was asked for; a reversed range is the same range) and never against the numbers one run of
 * xorshift happened to produce: a check that pinned "seed 7 answers 1937441" would pass a generator
 * that ignored the range and would fail the day the recurrence is replaced, and neither is a fact
 * about xud_get_random_value().
 *-----------------------------------------------------------------------------------------------*/

#define XUD_TEST_RANDOM_DRAWS 200

/* A shell on one seed: the two generator fields are the only ones these checks touch, and the rest
   of the struct stays zero because nothing below reaches for it. The seed is set rather than written,
   so the shell arrives on it the way a caller puts a real context on it.
*/
static void xud_test_random_shell(XudContext* shell, uint32_t seed) {
    memset(shell, 0, sizeof(*shell));
    (void)xud_set_random_seed(shell, seed);
}

static void xud_test_random(void) {
    XudContext a;
    XudContext b;
    int draw = 0;
    int first[XUD_TEST_RANDOM_DRAWS];
    int second[XUD_TEST_RANDOM_DRAWS];
    int index = 0;
    int low = 0;
    uint32_t inRange = 0u;
    uint32_t distinct = 0u;
    uint32_t same = 0u;
    uint32_t seen = 0u;

    xud_check(xud_get_random_value(NULL, &a, 0, 1) == XUD_INVALID_ARG, "random/null-out", "an out-param that cannot be written is refused");
    xud_check(xud_get_random_value(&draw, NULL, 0, 1) == XUD_INVALID_ARG, "random/null-context", "and so is no context at all");
    xud_check(xud_set_random_seed(NULL, 7u) == XUD_INVALID_ARG, "random/seed-null-context", "and a seed with nowhere to go");

    xud_test_random_shell(&a, 7u);
    xud_check(xud_set_random_seed(&a, 7u) == XUD_ALREADY, "random/seed-already-set",
        "asking for the seed the context is already on is the state-already question, not a restart");
    xud_check(xud_set_random_seed(&a, 9u) == XUD_OK, "random/seed-changed", "a different seed is accepted");

    /* One seed, two contexts: the whole determinism rule in one comparison. */
    xud_test_random_shell(&a, 7u);
    xud_test_random_shell(&b, 7u);
    for (index = 0; index < XUD_TEST_RANDOM_DRAWS; index++) {
        (void)xud_get_random_value(&first[index], &a, -7, 13);
        (void)xud_get_random_value(&second[index], &b, -7, 13);
    }
    for (index = 0; index < XUD_TEST_RANDOM_DRAWS; index++) {
        if (first[index] == second[index]) {
            same++;
        }
        if ((first[index] >= -7) && (first[index] <= 13)) {
            inRange++;
        }
    }
    xud_check(same == (uint32_t)XUD_TEST_RANDOM_DRAWS, "random/one-seed-one-sequence",
        "two contexts on one seed answer the same 200 numbers in the same order, which is the rule the seed exists for");
    xud_check(inRange == (uint32_t)XUD_TEST_RANDOM_DRAWS, "random/values-in-range",
        "every draw of a [-7, 13] range is inside [-7, 13], the inclusive bound raylib states");

    distinct = 0u;
    for (index = -7; index <= 13; index++) {
        for (seen = 0u; seen < (uint32_t)XUD_TEST_RANDOM_DRAWS; seen++) {
            if (first[seen] == index) {
                distinct++;
                break;
            }
        }
    }
    xud_check(distinct >= 15u, "random/sequence-moves",
        "at least 15 of the 21 values of the range appear in 200 draws, which is what a stalled or shortened generator fails");

    /* A different seed is a different sequence, or the seed is decoration. */
    xud_test_random_shell(&a, 7u);
    xud_test_random_shell(&b, 11u);
    (void)xud_get_random_value(&draw, &a, 0, 1000000);
    low = draw;
    (void)xud_get_random_value(&draw, &b, 0, 1000000);
    xud_check(low != draw, "random/seeds-differ", "the first draw of seed 7 is not the first draw of seed 11");

    /* Re-seeding back to a seed restarts its sequence - the restart a caller resets a scene with. */
    xud_test_random_shell(&a, 7u);
    for (index = 0; index < 50; index++) {
        (void)xud_get_random_value(&draw, &a, 0, 100);
    }
    xud_check(xud_set_random_seed(&a, 9u) == XUD_OK, "random/reseed-away", "the context is moved off 7 ...");
    xud_check(xud_set_random_seed(&a, 7u) == XUD_OK, "random/reseed-back", "... and back onto it again");
    (void)xud_get_random_value(&draw, &a, -7, 13);
    xud_check(draw == first[0], "random/reseed-restarts-sequence",
        "and the next draw is the first draw of seed 7 again, so the sequence is the seed's and not the context's history");

    /* raylib's swap: the range the wrong way round is the same range, draw for draw. */
    xud_test_random_shell(&a, 3u);
    xud_test_random_shell(&b, 3u);
    same = 0u;
    for (index = 0; index < 100; index++) {
        int forward = 0;
        int reversed = 0;
        (void)xud_get_random_value(&forward, &a, 2, 9);
        (void)xud_get_random_value(&reversed, &b, 9, 2);
        if ((forward == reversed) && (forward >= 2) && (forward <= 9)) {
            same++;
        }
    }
    xud_check(same == 100u, "random/range-swapped",
        "(9, 2) answers what (2, 9) answers at the same point of the same sequence - raylib's swap, kept");

    /* A range of one is that number, and a range below zero stays below it. */
    xud_test_random_shell(&a, 5u);
    inRange = 0u;
    for (index = 0; index < 20; index++) {
        (void)xud_get_random_value(&draw, &a, -4, -4);
        if (draw == -4) {
            inRange++;
        }
    }
    xud_check(inRange == 20u, "random/range-of-one", "a range whose ends are equal answers that end every time");

    xud_test_random_shell(&a, 13u);
    inRange = 0u;
    for (index = 0; index < 100; index++) {
        (void)xud_get_random_value(&draw, &a, -5, -1);
        if ((draw >= -5) && (draw <= -1)) {
            inRange++;
        }
    }
    xud_check(inRange == 100u, "random/range-below-zero", "a wholly negative range never answers zero or a positive");

    /* The whole int range is the SPAN that overflows an int subtraction on the way to its own size
       (2^32 values), so it is the case the widening of the endpoints is there for. */
    xud_test_random_shell(&a, 17u);
    inRange = 0u;
    distinct = 0u;
    for (index = 0; index < 100; index++) {
        (void)xud_get_random_value(&draw, &a, INT_MIN, INT_MAX);
        if ((draw >= INT_MIN) && (draw <= INT_MAX)) {
            inRange++;
        }
        if (draw != 0) {
            distinct++;
        }
    }
    xud_check(inRange == 100u, "random/range-whole-int", "the full INT_MIN..INT_MAX span answers a value inside itself");
    xud_check(distinct >= 90u, "random/range-whole-int-moves", "and answers something other than zero for it");
}

/* The two extent getters: the extent a frame renders at is the swapchain's, held on the context,
   and NOT the size the window record says it asked the compositor for - the two differ the moment a
   compositor answers a different size, which is the reason the pair exists beside the window's own
   members. The window record is filled with a DIFFERENT extent than the context so a getter that
   read the wrong one cannot pass.
*/
static void xud_test_screen_extent(void) {
    XudWindow window;
    XudContext shell;
    uint32_t width = 0u;
    uint32_t height = 0u;

    memset(&window, 0, sizeof(window));
    memset(&shell, 0, sizeof(shell));
    shell.window = &window;
    shell.width = 800u;
    shell.height = 600u;
    window.width = 1024u;
    window.height = 768u;

    xud_check(xud_get_screen_width(NULL, &shell) == XUD_INVALID_ARG, "extent/width-no-out-param",
        "the width getter refuses a place to write nothing to");
    xud_check(xud_get_screen_height(NULL, &shell) == XUD_INVALID_ARG, "extent/height-no-out-param", "and the height getter the same");
    xud_check(
        xud_get_screen_width(&width, NULL) == XUD_INVALID_ARG, "extent/width-no-context", "a missing context has no extent to answer");
    xud_check(xud_get_screen_height(&height, NULL) == XUD_INVALID_ARG, "extent/height-no-context", "and the same for the rows");

    shell.window = NULL;
    xud_check(xud_get_screen_width(&width, &shell) == XUD_UNSUPPORTED, "extent/width-no-window",
        "a context that never completed bring-up has no swapchain extent, which is the state the pair "
        "refuses rather than answering the asked-for size");
    xud_check(xud_get_screen_height(&height, &shell) == XUD_UNSUPPORTED, "extent/height-no-window", "and the same on the rows");
    xud_check((width == 0u) && (height == 0u), "extent/refusals-write-nothing",
        "and a refusal leaves the caller's own numbers alone rather than writing zeros over them");

    shell.window = &window;
    width = 0u;
    height = 0u;
    xud_check(xud_get_screen_width(&width, &shell) == XUD_OK, "extent/width-answers", "with the window live the pair answers");
    xud_check(xud_get_screen_height(&height, &shell) == XUD_OK, "extent/height-answers", "on both axes");
    xud_check((width == 800u) && (height == 600u), "extent/answers-the-rendered-extent",
        "and the numbers are the context's swapchain extent, not the window record's requested size");
}

void xud_test_core(void) {
    xud_test_init_arg_validation();
    xud_test_vulkan_supported();
    xud_test_init_bringup_boundary();
    xud_test_close();
    xud_test_no_leak();
    xud_test_random();
    xud_test_screen_extent();
    if (xud_gpu_tests_enabled()) {
        xud_test_gpu_context();
    }
}
