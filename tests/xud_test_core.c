/***********************************************************************************************
 *
 *   xud_test_core.c - the context: the public query, the bring-up ladder, the one teardown order
 *
 *   Owns : xud_vulkan_supported() as a real runtime query, the argument guards of xud_init_context(), the bring-up boundary, the idempotent
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
#include <stdio.h> /* Required for: printf, the note this area prints when a guard has nothing to check */

static void xud_test_init_arg_validation(void) {
    XudContext* context = NULL;

    xud_check(
        xud_init_context(NULL, 640, 480) == XUD_INVALID_ARG, "init/null-out-param", "a null out-param must be rejected, not dereferenced");

    xud_check(xud_init_context(&context, 0, 480) == XUD_INVALID_ARG, "init/zero-width", "zero width must be rejected");
    xud_check(xud_init_context(&context, 640, 0) == XUD_INVALID_ARG, "init/zero-height", "zero height must be rejected");
    xud_check(xud_init_context(&context, XUD_MAX_WINDOW_EXTENT + 1u, 480) == XUD_INVALID_ARG, "init/extent-too-large",
        "an absurd extent must be rejected host-side");
    xud_check(xud_init_context(&context, 0, XUD_MAX_WINDOW_EXTENT + 1u) == XUD_INVALID_ARG, "init/zero-height-over-limit",
        "both halves bad at once is still one bad argument");

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
    const XudResult result = xud_init_context(&context, 640, 480);

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
    smallest = xud_init_context(&context, 1, 1);
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
    xud_check(xud_init_context(&context, XUD_MAX_WINDOW_EXTENT + 1u, XUD_MAX_WINDOW_EXTENT) == XUD_INVALID_ARG,
        "init/limit-extent-rejected", "one above the limit is a bad argument");
    xud_check(context == NULL, "init/limit-extent-null", "and nothing was handed back");
}

static void xud_test_close(void) {
    XudContext* context = NULL;

    xud_check(xud_close_context(NULL) == XUD_INVALID_ARG, "close/null-out-param", "a null out-param must be rejected, not dereferenced");
    xud_check(xud_close_context(&context) == XUD_ALREADY, "close/already-closed",
        "closing a NULL context is XUD_ALREADY, not a crash and not XUD_OK");
    xud_check(context == NULL, "close/leaves-null", "closing a NULL context keeps it NULL");
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
        (void)xud_init_context(&context, 0u, 480u);
        (void)xud_close_context(&context);
        (void)xud_init_context(&context, XUD_MAX_WINDOW_EXTENT + 1u, 480u);
        (void)xud_close_context(&context);
        (void)xud_init_context(&context, 480u, XUD_MAX_WINDOW_EXTENT + 1u);
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

/* GPU-gated: the whole context lifecycle against the real driver - S8's ownership step, grown by
   the swapchain rung. This is the one place in the run that creates a context, so it is the one
   place that can say what a context owns: a loader, a diagnostics struct when validation was asked
   for, a GLFW window, a VkInstance carrying that window's required instance extensions, a physical
   device queried through THAT instance, a VkDevice created from that device's recorded caps with b
   oth role queues resolved, a VkSurfaceKHR for the window inside that window, and the XudSwapchai
   n created for that surface on that device. The allocator is the one rung still not in the tree,
   so its field stays a sentinel and the checks below say so rather than assuming it. The teardown
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

    result = xud_init_context(&context, 320u, 240u);
    xud_check(result == XUD_OK, "gpu-context/bringup-completes",
        "this machine has a Vulkan loader and a Wayland session, so the ladder must complete");
    if (context == NULL)
        return;

    xud_check(context->width == 320u, "context/extent-width", "the extent asked for is the extent recorded");
    xud_check(context->height == 240u, "context/extent-height", "and so is the other half of it");
    xud_check(context->validate == XUD_VALIDATE, "context/validate-snapshot",
        "the context carries the XUD_VALIDATE build switch it was built with, as a snapshot");

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
    xud_check(context->device->queueRow.queueFamilyIndex == context->physicalDevice->caps.queueFamilyIndex,
        "context/device-row-is-the-chosen-family", "the device was created for the family the physical layer chose, not another one");
    xud_check(context->device->queueRow.pQueuePriorities == context->device->queuePriorities, "context/priorities-live",
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
    xud_check(context->descriptor->sets[XUD_DESCRIPTOR_SET_MATERIAL] == VK_NULL_HANDLE, "context/descriptor-set-1-open-question",
        "and set 1 is not allocated: the layout is declared and the set is the texture step's");
    xud_check(context->descriptor->uniformBuffer != VK_NULL_HANDLE, "context/descriptor-buffer-live",
        "the per-frame uniform buffer is live and bound to the allocator's memory");
    xud_check(context->allocator->blockCount == 2u, "context/allocator-two-blocks-now",
        "and the allocator owns two VkDeviceMemory now: the descriptor rung's uniform buffer and the "
        "render pass's depth image - the image is the first thing above the allocator that asked for a"
        "n IMAGE's requirements, and its alignment (65536 on this driver) does not fit a hole in the "
        "buffer's block, so it is a second block rather than a suballocation");
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
        (void)xud_init_context(&once, 64u, 64u);
        if (once != NULL)
            (void)xud_close_context(&once);
        xud_check(once == NULL, "gpu-context/cycle-hands-nothing-back",
            "a cycle hands back nothing once it is closed, whether it completed or was refused");
    }
    xud_check(1, "gpu-context/cycles-ran", "the create+destroy cycles completed");
}

void xud_test_core(void) {
    xud_test_init_arg_validation();
    xud_test_vulkan_supported();
    xud_test_init_bringup_boundary();
    xud_test_close();
    xud_test_no_leak();
    if (xud_gpu_tests_enabled()) {
        xud_test_gpu_context();
    }
}
