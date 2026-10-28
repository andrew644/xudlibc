/***********************************************************************************************
 *
 *   xud_test_window.c - the window layer: a GLFW window plus the surface it was handed
 *
 *   Owns : the required-instance-extension list, the instance-support check, the extent bounds, the surface create/destroy rungs, the
 * presentation check and the live-window count - plus the GPU half that opens a real window on this machine's Wayland session and destroys
 * its surface. Layer: tests. Sees xud_window.h, which is the one header of this rung that includes <GLFW/glfw3.h> last, after
 * <vulkan/vulkan.h> - the include-order rule AGENTS.md records for glfw3.h's Vulkan declarations.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
#include "xud_instance.h" /* Required for: XudInstance, xud_instance_create/destroy - the surface names the instance it was created for */
#include "xud_loader.h" /* Required for: XudLoader, the vkDestroySurfaceKHR row */
#include "xud_physical.h" /* Required for: XudPhysicalDevice, the chosen queue family the presentation check is asked about */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */
#include "xud_window.h" /* Required for: XudWindow, the surface rungs, the presentation check, the required-extension list */
#include <stdlib.h> /* Required for: calloc and free, the synthetic records this area builds and frees */
#include <string.h> /* Required for: memset, the synthetic rows this area zeroes and compares */

/*-----------------------------------------------------------------------------------------------
 * The window layer, headless half: every guard that runs before GLFW is called, plus the pure
 * accessors and the loader-row checks. xud_window_open() is only ever called here with bad argum
 * ents: this machine has a Wayland session, so a valid call would create a real window and the
 * default run would stop being device-free. The window that is allocated here is the synthetic st
 * ate every other layer tests - no GLFW window, no surface - which is what makes the handleless t
 * eardown rule of this layer testable without a window system.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_window_pure(void) {
    XudLoader loader;
    XudInstance instance;
    XudPhysicalDevice physical;
    XudWindow* window = NULL;
    XudWindow* synthetic = NULL;
    const VkInstance fake = (VkInstance)(uintptr_t)1u;
    const VkInstance otherFake = (VkInstance)(uintptr_t)2u;
    const char* names[XUD_INSTANCE_EXTRA_MAX] = { NULL };
    uint32_t count = 0u;
    bool supports = false;

    memset(&loader, 0, sizeof(loader));
    loader.open = true;
    loader.boundInstance = VK_NULL_HANDLE;
    memset(&instance, 0, sizeof(instance));
    instance.instance = VK_NULL_HANDLE;
    instance.enabledExtensionCount = 0u;
    memset(&physical, 0, sizeof(physical));
    physical.physicalDevice = VK_NULL_HANDLE;
    physical.caps.queueFamilyIndex = XUD_PHYSICAL_NO_QUEUE_FAMILY;

    synthetic = calloc(1, sizeof(XudWindow));
    xud_check(synthetic != NULL, "window/synthetic-alloc", "the synthetic struct must allocate");
    if (synthetic == NULL)
        return;
    synthetic->surface = VK_NULL_HANDLE;
    synthetic->surfaceInstance = VK_NULL_HANDLE;
    synthetic->glfwWindow = NULL;

    /* open: the argument guards, all of them ahead of any GLFW call. */
    xud_check(xud_window_open(NULL, 640u, 480u) == XUD_INVALID_ARG, "window/open-null-out", "a null out-param must be rejected");
    xud_check(xud_window_open(&window, 0u, 480u) == XUD_INVALID_ARG, "window/open-zero-width", "an extent of 0 is not a window");
    xud_check(xud_window_open(&window, 640u, 0u) == XUD_INVALID_ARG, "window/open-zero-height", "an extent of 0 is not a window");
    xud_check(xud_window_open(&window, XUD_WINDOW_MAX_EXTENT + 1u, 480u) == XUD_INVALID_ARG, "window/open-over-int-width",
        "an extent above glfwCreateWindow's int bound is a bad argument, not a wrap");
    xud_check(xud_window_open(&window, 640u, XUD_WINDOW_MAX_EXTENT + 1u) == XUD_INVALID_ARG, "window/open-over-int-height",
        "the same bound on the other axis");
    xud_check(window == NULL, "window/open-refusals-null", "a refused open leaves the slot empty");
    xud_check(xud_window_open(&synthetic, 640u, 480u) == XUD_INVALID_ARG, "window/open-slot-taken",
        "a slot that already owns a window is not filled twice");
    xud_check(synthetic != NULL, "window/open-slot-taken-leaves-it", "the refusal must not have freed or cleared the window in hand");

    /* The accessor over an empty required list: GLFW had no Vulkan answer at open time, so the
       window asks for nothing and the GLFW call below is the one that refuses. */
    xud_check(xud_window_required_instance_extensions(synthetic, names, &count) == XUD_OK, "window/required-empty-ok",
        "an empty required list is not an error");
    xud_check(count == 0u, "window/required-empty-count", "nothing is claimed");
    xud_check(names[0] == NULL, "window/required-empty-untouched", "nothing beyond the count is written");
    xud_check(xud_window_required_instance_extensions(NULL, names, &count) == XUD_INVALID_ARG, "window/required-null-window",
        "a null window must be rejected");
    xud_check(xud_window_required_instance_extensions(synthetic, NULL, &count) == XUD_INVALID_ARG, "window/required-null-out",
        "a null out-array must be rejected");
    xud_check(xud_window_required_instance_extensions(synthetic, names, NULL) == XUD_INVALID_ARG, "window/required-null-count-out",
        "a null out-count must be rejected");

    /* The pure support check, both ways: one required name, enabled or not. */
    xud_check(xud_window_check_instance_support(NULL, synthetic, &instance) == XUD_INVALID_ARG, "window/support-null-out",
        "a null out-param must be rejected");
    xud_check(xud_window_check_instance_support(&supports, NULL, &instance) == XUD_INVALID_ARG, "window/support-null-window",
        "a null window must be rejected");
    xud_check(xud_window_check_instance_support(&supports, synthetic, NULL) == XUD_INVALID_ARG, "window/support-null-instance",
        "a null instance must be rejected");
    xud_check(xud_window_check_instance_support(&supports, synthetic, &instance) == XUD_OK, "window/support-empty-ok",
        "an empty required list asks for nothing");
    xud_check(supports == true, "window/support-empty-true", "so the instance supports it");

    strcpy(synthetic->requiredInstanceExtensionNameStorage[0], VK_KHR_SURFACE_EXTENSION_NAME);
    synthetic->requiredInstanceExtensionCount = 1u;
    xud_check(xud_window_check_instance_support(&supports, synthetic, &instance) == XUD_OK, "window/support-missing-ok",
        "the check itself is not an error");
    xud_check(supports == false, "window/support-missing-false", "a required name the instance does not have is not supported");
    strcpy(instance.enabledExtensionNameStorage[0], VK_KHR_SURFACE_EXTENSION_NAME);
    instance.enabledExtensionCount = 1u;
    xud_check(xud_window_check_instance_support(&supports, synthetic, &instance) == XUD_OK, "window/support-enabled-ok",
        "the same check with the name enabled");
    xud_check(supports == true, "window/support-enabled-true", "the name being enabled is what flips it");

    /* presentation_support: every guard, and the refused call leaves the answer alone. The fiel
       d guards are reached one at a time, with the guards above them already satisfied. */
    supports = true;
    xud_check(xud_window_presentation_support(NULL, synthetic, &instance, &physical) == XUD_INVALID_ARG, "window/presentation-null-out",
        "a null out-param must be rejected");
    xud_check(xud_window_presentation_support(&supports, NULL, &instance, &physical) == XUD_INVALID_ARG, "window/presentation-null-window",
        "a null window must be rejected");
    xud_check(xud_window_presentation_support(&supports, synthetic, NULL, &physical) == XUD_INVALID_ARG,
        "window/presentation-null-instance", "a null instance must be rejected");
    xud_check(xud_window_presentation_support(&supports, synthetic, &instance, NULL) == XUD_INVALID_ARG,
        "window/presentation-null-physical", "a null physical device must be rejected");
    synthetic->glfwWindow = (GLFWwindow*)(uintptr_t)1u;
    xud_check(xud_window_presentation_support(&supports, synthetic, &instance, &physical) == XUD_INVALID_ARG,
        "window/presentation-handleless-instance", "GLFW asserts the instance is live, so a handle-less one is a bad argument");
    instance.instance = fake;
    xud_check(xud_window_presentation_support(&supports, synthetic, &instance, &physical) == XUD_INVALID_ARG,
        "window/presentation-handleless-physical", "and the same for the physical device");
    physical.physicalDevice = (VkPhysicalDevice)(uintptr_t)1u;
    xud_check(xud_window_presentation_support(&supports, synthetic, &instance, &physical) == XUD_INVALID_ARG,
        "window/presentation-no-family", "a physical device whose queue family was never chosen has nothing to ask about");
    physical.caps.queueFamilyIndex = 0u;
    instance.enabledExtensionCount = 0u;
    xud_check(xud_window_presentation_support(&supports, synthetic, &instance, &physical) == XUD_UNSUPPORTED, "window/presentation-refused",
        "an instance that cannot have a surface for this window is refused without a call");
    xud_check(supports == true, "window/presentation-refused-leaves-answer",
        "a refused call must not have written an answer: it was true above and stayed true");

    /* surface_create: the loader and instance guards, then the two row checks. No GLFW call is
       reached on any of these paths - the fake window handle is never dereferenced. */
    xud_check(xud_window_surface_create(NULL, &loader, &instance) == XUD_INVALID_ARG, "window/surface-null-window",
        "a null window must be rejected");
    xud_check(xud_window_surface_create(synthetic, NULL, &instance) == XUD_INVALID_ARG, "window/surface-null-loader",
        "a null loader must be rejected");
    xud_check(xud_window_surface_create(synthetic, &loader, NULL) == XUD_INVALID_ARG, "window/surface-null-instance",
        "a null instance must be rejected");
    loader.open = false;
    xud_check(xud_window_surface_create(synthetic, &loader, &instance) == XUD_INVALID_ARG, "window/surface-closed-loader",
        "a closed loader has no rows to call through");
    loader.open = true;
    strcpy(instance.enabledExtensionNameStorage[0], VK_KHR_SURFACE_EXTENSION_NAME);
    instance.enabledExtensionCount = 1u;
    xud_check(xud_window_surface_create(synthetic, &loader, &instance) == XUD_UNSUPPORTED, "window/surface-not-bound",
        "rows that are not this instance's rows cannot create its surface");
    loader.boundInstance = fake;
    xud_check(xud_window_surface_create(synthetic, &loader, &instance) == XUD_UNSUPPORTED, "window/surface-no-destroy-row",
        "a surface this layer cannot destroy is a surface it must not create");
    xud_check(synthetic->surface == VK_NULL_HANDLE, "window/surface-refusals-stay-empty",
        "a refused create leaves the surface at its zero state");

    /* surface_close: no surface is already-closed, and a surface whose instance rows are gone,
       or whose destroy row is gone, is refused without a call. */
    xud_check(
        xud_window_surface_close(NULL, &loader) == XUD_INVALID_ARG, "window/surface-close-null-window", "a null window must be rejected");
    xud_check(
        xud_window_surface_close(synthetic, NULL) == XUD_INVALID_ARG, "window/surface-close-null-loader", "a null loader must be rejected");
    loader.open = false;
    xud_check(xud_window_surface_close(synthetic, &loader) == XUD_INVALID_ARG, "window/surface-close-closed-loader",
        "a closed loader has no rows to call through");
    loader.open = true;
    xud_check(xud_window_surface_close(synthetic, &loader) == XUD_ALREADY, "window/surface-close-nothing",
        "no surface is already closed, not an error");
    synthetic->surface = (VkSurfaceKHR)(uintptr_t)1u;
    synthetic->surfaceInstance = otherFake;
    xud_check(xud_window_surface_close(synthetic, &loader) == XUD_UNSUPPORTED, "window/surface-close-other-instance-rows",
        "a surface must not be destroyed through another instance's rows");
    synthetic->surfaceInstance = fake;
    xud_check(xud_window_surface_close(synthetic, &loader) == XUD_UNSUPPORTED, "window/surface-close-no-destroy-row",
        "the row this layer needs is not in the table, so the surface stays");
    xud_check(
        synthetic->surface != VK_NULL_HANDLE, "window/surface-close-refusal-keeps-it", "a refused teardown must not clear a live surface");

    /* window_close: the surface gate, and the handleless teardown. */
    xud_check(xud_window_close(NULL) == XUD_INVALID_ARG, "window/close-null-out", "a null out-param must be rejected");
    xud_check(xud_window_close(&window) == XUD_ALREADY, "window/close-nothing", "no struct is already closed, not an error");
    xud_check(xud_window_close(&synthetic) == XUD_INVALID_ARG, "window/close-under-surface",
        "a window whose surface is still live must not be destroyed under it");
    xud_check(synthetic != NULL, "window/close-refusal-keeps-window", "the refusal must not have freed the window");
    synthetic->surface = VK_NULL_HANDLE;
    synthetic->glfwWindow = NULL;
    xud_check(xud_window_close(&synthetic) == XUD_OK, "window/close-handleless",
        "a window that never got a GLFW window frees without a GLFW call");
    xud_check(synthetic == NULL, "window/close-clears-out", "close clears the out-param");

    /* --- xud_window_resize and xud_window_framebuffer_size: the guards, every one of them before
         a GLFW call. The two calls that reach GLFW are the GPU half's, below. --- */
    xud_check(xud_window_resize(NULL, 64u, 64u) == XUD_INVALID_ARG, "window/resize-null", "no window at all is a bad argument");
    xud_check(xud_window_resize(synthetic, 0u, 64u) == XUD_INVALID_ARG, "window/resize-zero-width",
        "an extent of 0 is not a window, and GLFW's own resizeWindow clamps to 1 rather than refus"
        "ing - the bound here is what keeps a zero-extent swapchain out of reach");
    xud_check(xud_window_resize(synthetic, 64u, 0u) == XUD_INVALID_ARG, "window/resize-zero-height", "and so is a height of 0");
    xud_check(xud_window_resize(synthetic, XUD_WINDOW_MAX_EXTENT + 1u, 64u) == XUD_INVALID_ARG, "window/resize-over-bound",
        "an extent past the window layer's own int bound is refused before glfwSetWindowSize is hand"
        "ed an int that cannot hold it");
    xud_check(xud_window_resize(synthetic, 64u, 64u) == XUD_INVALID_ARG, "window/resize-no-glfw-handle",
        "a window that never got a GLFW window has nothing to resize, and the refusal is the same o"
        "ne xud_window_close() gives it above");
    xud_check(xud_window_framebuffer_size(NULL, &count, synthetic) == XUD_INVALID_ARG, "window/fb-size-null-width-out",
        "a width that cannot be written is a bad argument");
    xud_check(xud_window_framebuffer_size(&count, NULL, synthetic) == XUD_INVALID_ARG, "window/fb-size-null-height-out",
        "and so is a height that cannot be written");
    xud_check(
        xud_window_framebuffer_size(&count, &count, NULL) == XUD_INVALID_ARG, "window/fb-size-null-window", "and so is no window at all");
    xud_check(xud_window_framebuffer_size(&count, &count, synthetic) == XUD_INVALID_ARG, "window/fb-size-no-glfw-handle",
        "a window with no GLFW window behind it has no framebuffer extent to answer, which is what"
        "makes the 0/0 a platform leaves behind a refusal rather than a zero-extent swapchain");
}

/*-----------------------------------------------------------------------------------------------
 * The window layer against the driver: a real GLFW window on this machine's Wayland session, an
 * instance that carries GLFW's required names, the window system's presentation answer, and a su
 * rface created and destroyed through that instance. The teardown order is the one xud_close_con
 * text() will use: surface before window, window before instance.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_gpu_window(void) {
    XudLoader* loader = NULL;
    XudInstance* instance = NULL;
    XudPhysicalDevice* physical = NULL;
    XudWindow* window = NULL;
    XudWindow* first = NULL;
    XudWindow* second = NULL;
    const char* required[XUD_INSTANCE_EXTRA_MAX] = { NULL };
    uint32_t requiredCount = 0u;
    uint32_t fbWidth = 0u;
    uint32_t fbHeight = 0u;
    uint32_t fbMovedWidth = 0u;
    uint32_t fbMovedHeight = 0u;
    bool supports = false;
    XudResult result;

    result = xud_loader_open(&loader);
    xud_check(result == XUD_OK, "gpu-window/loader-open", "the GPU run needs an open loader");
    if (loader == NULL)
        return;

    result = xud_window_open(&window, 320u, 240u);
    xud_check(result == XUD_OK, "gpu-window/open", "this machine has a Wayland session, so GLFW must create a window");
    if (window == NULL) {
        (void)xud_loader_close(&loader);
        return;
    }

    xud_check(window->glfwWindow != NULL, "gpu-window/glfw-handle", "the window half of the struct is live");
    xud_check(
        (window->width == 320u) && (window->height == 240u), "gpu-window/extent-recorded", "the extent asked for is the extent recorded");
    xud_check(window->surface == VK_NULL_HANDLE, "gpu-window/surface-null-at-open", "opening a window creates no surface");
    xud_check(window->surfaceInstance == VK_NULL_HANDLE, "gpu-window/surface-instance-null", "and records no instance for one");

    /* The FRAMEBUFFER's extent, which is what a swapchain for this window has to be created at, an
       d the programmatic resize. GLFW's Wayland half scales wl.width / wl.height into wl.fbWidth /
       wl.fbHeight by the fractional scale or the buffer scale, so on a scaled session the two exten
       ts differ and the only fact that holds at both of them is that the framebuffer extent follows
       the window - which is the half that matters, because a swapchain that kept the old extent is
       a swapchain the compositor calls out of date on every acquire, forever. */
    xud_check(xud_window_framebuffer_size(&fbWidth, &fbHeight, window) == XUD_OK, "gpu-window/fb-size",
        "the window system answers a framebuffer extent for the window it made");
    xud_check((fbWidth > 0u) && (fbHeight > 0u), "gpu-window/fb-size-positive",
        "and it is a positive one, so a swapchain for this window has an extent to be created at");
    xud_check(xud_window_framebuffer_size(NULL, &fbHeight, window) == XUD_INVALID_ARG, "gpu-window/fb-size-null-out",
        "a width that cannot be written is refused even against a live window");
    xud_check(xud_window_resize(window, 320u, 240u) == XUD_ALREADY, "gpu-window/resize-already",
        "the window is already at that extent, so there is nothing to resize - the rule xud_swapcha"
        "in_recreate() follows one rung up");
    xud_check(xud_window_resize(window, 0u, 240u) == XUD_INVALID_ARG, "gpu-window/resize-zero",
        "and an extent of 0 is refused before GLFW is asked at all");
    xud_check(xud_window_resize(window, 400u, 300u) == XUD_OK, "gpu-window/resize-ok", "GLFW took the new extent");
    xud_check((window->width == 400u) && (window->height == 300u), "gpu-window/resize-recorded",
        "the window's requested extent is the extent asked for, written rather than read back - glF"
        "WSetWindowSize returns void, so the zero-state rule applies here");
    xud_check(xud_window_framebuffer_size(&fbMovedWidth, &fbMovedHeight, window) == XUD_OK, "gpu-window/fb-size-after",
        "the framebuffer extent is read again after the resize");
    xud_check((fbMovedWidth > 0u) && (fbMovedHeight > 0u), "gpu-window/fb-size-after-positive", "and it is still positive");
    xud_check((fbMovedWidth != fbWidth) || (fbMovedHeight != fbHeight), "gpu-window/fb-follows-window",
        "and it moved with the window, which is the one fact that holds at every scale factor: a s"
        "wapchain that kept the old extent would be a swapchain the compositor calls out of date on "
        "every acquire, which is a resize loop rather than a resize");
    xud_check(xud_window_resize(window, 320u, 240u) == XUD_OK, "gpu-window/resize-back",
        "the window is resized back so the rest of this area runs at the extent it opened at");

    xud_check(xud_window_required_instance_extensions(window, required, &requiredCount) == XUD_OK, "gpu-window/required-accessor",
        "the recorded list is readable");
    xud_check(requiredCount == 2u, "gpu-window/required-count", "GLFW's Wayland backend asks for exactly two instance-level names");
    xud_check(xud_name_equal(required[0], VK_KHR_SURFACE_EXTENSION_NAME) == true, "gpu-window/required-first",
        "the surface extension, which this layer already wants");
    xud_check(xud_name_equal(required[1], "VK_KHR_wayland_surface") == true, "gpu-window/required-second",
        "the window system's own name, spelled out here: VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME "
        "is in vulkan_wayland.h, which this tree does not include");

    /* GLFW's names have to be part of THIS instance's enabled list: glfwCreateWindowSurface resol
       ves vkCreateWaylandSurfaceKHR through vkGetInstanceProcAddr against the instance it is hand
       ed, and the row answers NULL for an instance that does not have the extension enabled. VK_KH
       R_surface is wanted by the instance layer already, so it is not enabled twice. */
    result = xud_instance_create(&instance, loader, NULL, required, requiredCount);
    xud_check(result == XUD_OK, "gpu-window/instance-create-merged", "the merged instance must create");
    if (instance == NULL) {
        (void)xud_window_close(&window);
        (void)xud_loader_close(&loader);
        return;
    }
    xud_check(instance->enabledExtensionCount == 4u, "gpu-window/enabled-count",
        "three wanted names plus GLFW's wayland name, VK_KHR_surface not twice");
    xud_check(xud_window_check_instance_support(&supports, window, instance) == XUD_OK, "gpu-window/support-check",
        "the pure check runs against the merged instance");
    xud_check(supports == true, "gpu-window/support-true", "the merge worked: GLFW's required names are enabled here");

    result = xud_physical_open(&physical, loader, instance->instance);
    xud_check(result == XUD_OK, "gpu-window/physical-open", "the device is queried through it");
    if (physical == NULL) {
        (void)xud_window_close(&window);
        (void)xud_instance_destroy(&instance, loader, NULL);
        (void)xud_loader_close(&loader);
        return;
    }

    xud_check(xud_window_presentation_support(&supports, window, instance, physical) == XUD_OK, "gpu-window/presentation-call",
        "the query itself is not an error");
    xud_check(supports == true, "gpu-window/support-presented",
        "the Wayland window system presents to this physical device, through the chosen family");

    xud_check(loader->instanceTable.vkDestroySurfaceKHR != NULL, "gpu-window/destroy-row-live",
        "the row that tears a surface down exists here, so a surface may be created");

    result = xud_window_surface_create(window, loader, instance);
    xud_check(result == XUD_OK, "gpu-window/surface-create", "glfwCreateWindowSurface hands this window a surface for that instance");
    xud_check(window->surface != VK_NULL_HANDLE, "gpu-window/surface-live", "the surface is live");
    xud_check(window->surfaceInstance == instance->instance, "gpu-window/surface-instance-recorded",
        "and the instance it belongs to is recorded, for the teardown call that needs it");
    xud_check(xud_window_surface_create(window, loader, instance) == XUD_INVALID_ARG, "gpu-window/surface-create-twice",
        "a window does not get two surfaces");

    xud_check(xud_window_close(&window) == XUD_INVALID_ARG, "gpu-window/close-refused-under-surface",
        "the window must not be destroyed while its surface is still live");
    xud_check(window != NULL, "gpu-window/close-refusal-leaves-window", "and the refusal left it alone");

    xud_check(xud_window_surface_close(window, loader) == XUD_OK, "gpu-window/surface-close",
        "vkDestroySurfaceKHR through the instance it belongs to, while that instance is alive");
    xud_check(window->surface == VK_NULL_HANDLE, "gpu-window/surface-cleared", "the handle cleared");
    xud_check(window->surfaceInstance == VK_NULL_HANDLE, "gpu-window/surface-instance-cleared", "and so did the borrowed instance");
    xud_check(xud_window_surface_close(window, loader) == XUD_ALREADY, "gpu-window/surface-close-twice",
        "a second teardown is reported, not a double free");

    xud_check(xud_window_close(&window) == XUD_OK, "gpu-window/window-close", "the window half now");
    xud_check(window == NULL, "gpu-window/window-null-after-close", "and the slot is cleared");
    xud_check(xud_window_close(&window) == XUD_ALREADY, "gpu-window/window-close-twice", "a second close is reported too");

    /* Two windows alive at once: GLFW's state is process-level, and the count that decides when g
       lwfTerminate runs is the one thing that makes the second window's teardown safe. */
    result = xud_window_open(&first, 64u, 64u);
    xud_check(result == XUD_OK, "gpu-window/open-first", "a second run opens one more window");
    result = xud_window_open(&second, 64u, 64u);
    xud_check(result == XUD_OK, "gpu-window/open-second", "and a third at the same time");
    xud_check((first != NULL) && (second != NULL), "gpu-window/two-live", "both windows are alive together");
    if ((first != NULL) && (second != NULL)) {
        xud_check(first->glfwWindow != second->glfwWindow, "gpu-window/two-distinct", "they are two windows, not one window twice");
        xud_check(xud_window_close(&first) == XUD_OK, "gpu-window/close-first-of-pair",
            "closing one of the pair leaves GLFW alive for the other");
        xud_check(xud_window_close(&second) == XUD_OK, "gpu-window/close-second-of-pair", "and the last one runs GLFW's own teardown");
    }

    xud_check(xud_physical_close(&physical) == XUD_OK, "gpu-window/physical-close", "free-only");
    xud_check(physical == NULL, "gpu-window/physical-null", "and cleared");
    xud_check(xud_instance_destroy(&instance, loader, NULL) == XUD_OK, "gpu-window/instance-destroy",
        "the instance the surface was created for goes after the surface went");
    xud_check(instance == NULL, "gpu-window/instance-null", "and cleared");
    xud_check(xud_loader_close(&loader) == XUD_OK, "gpu-window/loader-close", "the loader closes");
    xud_check(loader == NULL, "gpu-window/loader-null", "and cleared");
}

void xud_test_window(void) {
    xud_test_window_pure();
    if (xud_gpu_tests_enabled()) {
        xud_test_gpu_window();
    }
}
