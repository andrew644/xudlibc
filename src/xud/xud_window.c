/***********************************************************************************************
 *
 *   xud_window.c - the window layer: GLFW's window half, the VkSurfaceKHR half, and the two
 *          rungs that put them back
 *
 *   Owns : the window layer's contract - glfwInit/glfwCreateWindow bring-up with the client API
 *          hinted GLFW_NO_API, GLFW's required-instance-extension list recorded before the wind
 *          ow exists, glfwGetPhysicalDevicePresentationSupport against the physical device and qu
 *          eue family the layer below chose, glfwCreateWindowSurface handing a VkSurfaceKHR to th
 *          e instance it is given, vkDestroySurfaceKHR taking it back through that same instan
 *          ce while it is still alive, glfwDestroyWindow and glfwTerminate taking the window bac
 *          k. The surface teardown is the library's, not GLFW's: 3.5.1 has no glfwDestroyVulkanS
 *          urface (grepped in the vendored glfw3.h and in src/vulkan.c, the name does not exist)
 *          and the entry point that destroys a surface is vkDestroySurfaceKHR, a row of the loade
 *          r's instance table - which is why this layer takes an XudLoader at all.
 *          The resize half is GLFW's too: glfwSetWindowSize is the programmatic resize (it
 *          returns void, so the requested extent is written into the record rather than read
 *          back), and glfwGetFramebufferSize is the extent a swapchain for this window has to
 *          be created at - GLFW's Wayland half scales wl.width / wl.height into wl.fbWidth /
 *          wl.fbHeight by the fractional or buffer scale, so on a scaled session the two pairs
 *          differ and the requested pair is a pair a swapchain at it would be called out of
 *          date on every acquire.
 *   Layer: above the instance (a surface is created FOR an instance, and its teardown takes that
 *          instance) and below the swapchain. This is the only file in src/xud/ that includes <GL
 *          FW/glfw3.h>, and the only one that calls GLFW at all: GLFW's Vulkan half resolves its
 *          own entry points - wl_window.c resolves vkCreateWaylandSurfaceKHR and vkGetPhysicalDe
 *          viceWaylandPresentationSupportKHR through vkGetInstanceProcAddr against the instance i
 *          t is handed - so the instance given here decides whether a surface can exist, and the
 *          names GLFW asks for have to be in that instance's enabled list, which is why xud_insta
 *          nce_create() takes them. Nothing here calls a Vulkan entry point of its own except thr
 *          ough a row of the loader's Volk tables, so the archive still holds no vk* symbol and th
 *          e link line still has no -lvulkan (AGENTS.md).
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_window.h"
#include "xud.h"
#include "xud_instance.h"
#include "xud_loader.h"
#include "xud_physical.h"
#include "xud_vulkan.h"

#include <assert.h> /* Required for: static_assert, the two handle-shape checks below */
#include <stdlib.h> /* Required for: calloc, free */

static_assert(sizeof(PFN_vkDestroySurfaceKHR) == sizeof(void*), "xud_window.c: the surface teardown row is one pointer-wide slot");
static_assert(sizeof(VkSurfaceKHR) == sizeof(void*), "xud_window.c: a surface handle is a pointer, so clearing it is clearing one word");

/*-----------------------------------------------------------------------------------------------
 * A leak this layer does not own, and does not paper over
 *-----------------------------------------------------------------------------------------------
 *
 *   The GPU run under -fsanitize=address,undefined reports one leak of 8 bytes in one allocatio
 *   n, and its stack is entirely outside this tree: malloc <- g_malloc <- g_strdup <- gdk_set_al
 *   lowed_backends (libgdk-3.so.0) <- libdecor's GTK plugin <- libdecor_new <- _glfwInitWayland
 *   (src/external/glfw/src/wl_init.c 877, the `if (_glfw.wl.libdecor.handle)` branch) <- glfwIni
 *   t. GLFW dlopen()s libdecor when it is present (local/libdecor 0.2.5-1 here) and libdecor's GT
 *   K plugin leaks a string it keeps forever; nothing in xudlibc allocates it, frees it, or can s
 *   ee it, so there is nothing here to fix and no suppression is installed to make the run look
 *   clean. The two runs that gate a commit are the normal one (719 checks, 0 failures, validati
 *   on-clean) and the sanitizer one for the allocations this tree DOES own (the headless run, 525
 *   checks, 0 failures, no report); the GPU sanitizer run is green apart from that one external a
 *   llocation, recorded here rather than hidden.
 */

/*-----------------------------------------------------------------------------------------------
 * The one piece of state here that is not inside XudContext
 *-----------------------------------------------------------------------------------------------
 *
 *   GLFW's own state is process-level: glfwInit() is idempotent (init.c 386-389 returns GLFW_TR
 *   UE when GLFW is already initialized, which is what lets every context call it without a flag
 *   of its own) and glfwTerminate() tears the whole library down, walking GLFW's window list and
 *   its monitors, cursors and mappings. So the last window to close has to run that rung and the
 *   others must not - and "last" is a property of every window in the process, not of the one co
 *   ntext that is closing, which is why the count sits here rather than in XudContext: AGENTS.md
 *   's "multiple windows must work" rule is exactly the case this count is for. It is library-sta
 *   te, incremented only after glfwCreateWindow answered with a window and decremented only by x
 *   ud_window_close(), so it cannot drift on a path that created nothing.
 */

static uint32_t xudGlfwLiveWindowCount = 0u;

/* Internal: is every extension GLFW's window system asked for enabled in this instance? This is
   the check-before-you-use rule for the surface rung, and it is what makes the merge above load-
   bearing rather than hopeful: glfwCreateWindowSurface resolves vkCreateWaylandSurfaceKHR through
   vkGetInstanceProcAddr against the instance it is handed, and that row answers NULL for an insta
   nce that does not have the extension enabled - probed here, on this machine's Wayland session,
   where an instance created without VK_KHR_wayland_surface answered VK_ERROR_EXTENSION_NOT_PRESEN
   T for glfwCreateWindowSurface and GLFW_FALSE for glfwGetPhysicalDevicePresentationSupport. The
   window's required list is GLFW's own answer, recorded at open time; an empty list asks for not
   hing, so this answers true and the GLFW call below is the one that refuses. */
static bool xud_window_all_required_enabled(const XudWindow* window, const XudInstance* instance) {
    uint32_t row = 0u;
    bool all = true;

    for (row = 0u; row < window->requiredInstanceExtensionCount; row++) {
        bool found = false;
        uint32_t enabled = 0u;

        for (enabled = 0u; enabled < instance->enabledExtensionCount; enabled++) {
            if (xud_name_equal(window->requiredInstanceExtensionNameStorage[row], instance->enabledExtensionNameStorage[enabled])) {
                found = true;
                break;
            }
        }
        if (!found) {
            all = false;
            break;
        }
    }
    return all;
}

/// See xud_window.h: GLFW's answer, copied out of the window's own storage. The out-array is fill
/// ed up to *outCount and left alone above it, so a caller that reads past the count reads what it
/// put there itself.
XudResult xud_window_required_instance_extensions(
    const XudWindow* window, const char* outNames[XUD_INSTANCE_EXTRA_MAX], uint32_t* outCount) {
    uint32_t row = 0u;

    if ((window == NULL) || (outNames == NULL) || (outCount == NULL))
        return XUD_INVALID_ARG;

    for (row = 0u; row < window->requiredInstanceExtensionCount; row++)
        outNames[row] = window->requiredInstanceExtensionNameStorage[row];
    *outCount = window->requiredInstanceExtensionCount;
    return XUD_OK;
}

/// See xud_window.h: the pure half of the surface gate.
XudResult xud_window_check_instance_support(bool* outSupports, const XudWindow* window, const XudInstance* instance) {
    if ((outSupports == NULL) || (window == NULL) || (instance == NULL))
        return XUD_INVALID_ARG;

    *outSupports = xud_window_all_required_enabled(window, instance);
    return XUD_OK;
}

/// See xud_window.h. GLFW's required names are copied here rather than kept as GLFW's pointers:
/// glfwGetRequiredInstanceExtensions returns pointers into storage GLFW keeps only until the libra
/// ry is terminated (src/vulkan.c returns _glfw.vk.extensions, a static array of static buffers), a
/// nd this struct owns the names for as long as the window lives.
XudResult xud_window_open(XudWindow** outWindow, uint32_t width, uint32_t height) {
    XudWindow* window = NULL;
    const char** requiredNames = NULL;
    uint32_t requiredCount = 0u;
    XudResult result = XUD_INVALID_ARG;
    uint32_t row = 0u;

    if ((outWindow == NULL) || (*outWindow != NULL) || (width < XUD_WINDOW_MIN_EXTENT) || (width > XUD_WINDOW_MAX_EXTENT)
        || (height < XUD_WINDOW_MIN_EXTENT) || (height > XUD_WINDOW_MAX_EXTENT))
        return XUD_INVALID_ARG;

    window = calloc(1, sizeof(XudWindow));
    if (window == NULL)
        return XUD_OUT_OF_MEMORY;
    window->surface = VK_NULL_HANDLE;
    window->surfaceInstance = VK_NULL_HANDLE;
    window->width = width;
    window->height = height;

    if (glfwInit() != GLFW_TRUE) {
        result = XUD_UNSUPPORTED; /* no GLFW platform here: no window system to ask for a surfac
                                    e from, which is the honest answer on a machine with no Waylan
                                    d session rather than a Vulkan failure */
        goto fail;
    }

    /* GLFW's own runtime query, and the one that says whether the Vulkan half of GLFW exists at a
       ll: it finds the loader, it creates no window and no instance. Without it glfwGetRequiredIn
       stanceExtensions answers NULL/0 and the call below would be made with nothing to check. */
    if (glfwVulkanSupported() != GLFW_TRUE) {
        result = XUD_UNSUPPORTED;
        goto fail;
    }

    requiredCount = 0u;
    requiredNames = glfwGetRequiredInstanceExtensions(&requiredCount);
    if ((requiredNames == NULL) && (requiredCount != 0u)) {
        result = XUD_INTERNAL_ERROR; /* GLFW lied about its own list */
        goto fail;
    }
    if (requiredCount > XUD_INSTANCE_EXTRA_MAX) {
        result = XUD_OUT_OF_RESOURCES; /* truncating the list would mean a surface rung that silen
                                        tly cannot work */
        goto fail;
    }
    for (row = 0u; row < requiredCount; row++) {
        if (xud_name_copy(window->requiredInstanceExtensionNameStorage[row], VK_MAX_EXTENSION_NAME_SIZE, requiredNames[row]) != XUD_OK) {
            result = XUD_INTERNAL_ERROR; /* a GLFW name longer than the buffer: reported, not trun
                                          cated */
            goto fail;
        }
    }
    window->requiredInstanceExtensionCount = requiredCount;

    /* The only client API glfwCreateWindowSurface accepts: vulkan.c refuses a window whose contex
       t.client is not GLFW_NO_API with VK_ERROR_NATIVE_WINDOW_IN_USE_KHR, and window.c 400 is whe
       re the hint lands (_glfw.hints.context.client). A window hinted for GL or EGL would carry a
       GL context this library does not have and must not have. */
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window->glfwWindow = glfwCreateWindow((int)width, (int)height, XUD_WINDOW_TITLE, NULL, NULL);
    if (window->glfwWindow == NULL) {
        result = XUD_UNSUPPORTED; /* the window system refused: no surface can be created for no
                                    thing */
        goto fail;
    }

    xudGlfwLiveWindowCount += 1u;
    *outWindow = window;
    return XUD_OK;

fail:
    free(window);
    *outWindow = NULL;
    return result;
}

/// See xud_window.h. glfwSetWindowSize is GLFW's own resize: on Wayland it runs resizeWindow ->
/// resizeFramebuffer, which wl_egl_window_resize's the EGL window and writes wl.fbWidth / fbHeight
/// synchronously, and on X11 it X11-resizes the window and re-asks for its pixmap. Neither backend
/// returns a VkResult, so the zero-state rule applies and the extent written back is the one asked
/// for - the framebuffer's own extent is a separate question, answered below.
XudResult xud_window_resize(XudWindow* window, uint32_t width, uint32_t height) {
    if ((window == NULL) || (window->glfwWindow == NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((width < XUD_WINDOW_MIN_EXTENT) || (width > XUD_WINDOW_MAX_EXTENT) || (height < XUD_WINDOW_MIN_EXTENT)
        || (height > XUD_WINDOW_MAX_EXTENT)) {
        return XUD_INVALID_ARG;
    }
    if ((width == window->width) && (height == window->height)) {
        /* The state asked for is the state present, so there is nothing to resize - the rule
           xud_swapchain_recreate() follows one rung up, and the reason a resize step that acts on
           a recreate ask can be handed an extent that no longer needs one. */
        return XUD_ALREADY;
    }

    glfwSetWindowSize(window->glfwWindow, (int)width, (int)height);
    window->width = width;
    window->height = height;
    return XUD_OK;
}

/// See xud_window.h. The framebuffer's extent, not the window's: GLFW keeps its own fbWidth /
/// fbHeight per backend (wl_window.c's resizeFramebuffer scales wl.width / wl.height by the
/// fractional scale or the buffer scale; x11_window.c reads the pixmap XGetWindowPixmap gave back),
/// and a swapchain whose extent is not that one is a swapchain the compositor calls out of date.
XudResult xud_window_framebuffer_size(uint32_t* outWidth, uint32_t* outHeight, const XudWindow* window) {
    int width = 0;
    int height = 0;

    if ((outWidth == NULL) || (outHeight == NULL) || (window == NULL) || (window->glfwWindow == NULL)) {
        return XUD_INVALID_ARG;
    }

    /* glfwGetFramebufferSize zeroes both out-params before it dispatches (window.c 715-727), so a
       platform that answers nothing leaves 0/0 behind rather than garbage. */
    glfwGetFramebufferSize(window->glfwWindow, &width, &height);

    /* GLFW reports the extent in ints, and a non-positive one is not a window. The layer's own
       XUD_WINDOW_MAX_EXTENT is INT_MAX (asserted in the header above), so a positive int is inside
       it by construction and no widening cast can wrap. */
    if ((width <= 0) || (height <= 0)) {
        return XUD_INVALID_ARG;
    }

    *outWidth = (uint32_t)width;
    *outHeight = (uint32_t)height;
    return XUD_OK;
}

/// See xud_window.h. The instance and the physical device, not a loader, are the arguments here:
/// glfwGetPhysicalDevicePresentationSupport asserts both are live (vulkan.c 6470) and resolves the
/// window system's own query through GLFW's vkGetInstanceProcAddr, against the instance it is hand
/// ed.
XudResult xud_window_presentation_support(
    bool* outSupports, const XudWindow* window, const XudInstance* instance, const XudPhysicalDevice* physicalDevice) {
    if ((outSupports == NULL) || (window == NULL) || (instance == NULL) || (physicalDevice == NULL) || (window->glfwWindow == NULL)
        || (instance->instance == VK_NULL_HANDLE) || (physicalDevice->physicalDevice == VK_NULL_HANDLE))
        return XUD_INVALID_ARG;

    /* The queue family the physical layer chose is what presentation is asked about, and a physica
       l device whose family was never chosen has nothing to ask about - the sentinel is outside ev
       ery index the query can return (xud_physical.h asserts it), so it is a bad argument here. */
    if (physicalDevice->caps.queueFamilyIndex == XUD_PHYSICAL_NO_QUEUE_FAMILY)
        return XUD_INVALID_ARG;

    if (!xud_window_all_required_enabled(window, instance))
        return XUD_UNSUPPORTED; /* the call is refused, not made: GLFW would answer GLFW_FALSE thr
                                  ough a NULL row */

    *outSupports = (glfwGetPhysicalDevicePresentationSupport(
                        instance->instance, physicalDevice->physicalDevice, physicalDevice->caps.queueFamilyIndex)
        == GLFW_TRUE);
    return XUD_OK;
}

/// See xud_window.h. The destroy row is checked before the create, not at teardown: a surface this
/// layer cannot destroy is a surface it must not create, and unlike vkDestroyInstance - which xud_i
/// nstance.c resolves at destroy time because vkGetInstanceProcAddr answers NULL for it until an in
/// stance exists - this row is filled in bulk by xud_loader_bind_instance, so checking it is free.
XudResult xud_window_surface_create(XudWindow* window, XudLoader* loader, const XudInstance* instance) {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    XudResult result = XUD_INVALID_ARG;
    VkResult vk = VK_SUCCESS;

    if ((window == NULL) || (loader == NULL) || (instance == NULL) || (window->glfwWindow == NULL) || (window->surface != VK_NULL_HANDLE)
        || (instance->instance == VK_NULL_HANDLE) || (!xud_loader_is_open(loader)))
        return XUD_INVALID_ARG;

    if (!xud_window_all_required_enabled(window, instance))
        return XUD_UNSUPPORTED;

    /* The rows must be this instance's rows: glfwCreateWindowSurface resolves through the instance
       it is handed, and a surface created through one instance's rows must be destroyed through th
       e same one - the bound-handle rule of the loader layer. */
    if (!xud_loader_instance_bound(loader, instance->instance))
        return XUD_UNSUPPORTED;
    if (loader->instanceTable.vkDestroySurfaceKHR == NULL)
        return XUD_UNSUPPORTED;

    vk = glfwCreateWindowSurface(instance->instance, window->glfwWindow, NULL, &surface);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        surface = VK_NULL_HANDLE;
        return result;
    }

    window->surface = surface;
    window->surfaceInstance = instance->instance;
    return XUD_OK;
}

/// See xud_window.h. vkDestroySurfaceKHR returns void, so this is the zero-state rule the other vo
/// id entry points in this tree follow (vkDestroyDevice, vkGetDeviceQueue, vkFreeMemory): the state
/// is checked before the call and cleared after it, and there is no VkResult to map.
XudResult xud_window_surface_close(XudWindow* window, XudLoader* loader) {
    if ((window == NULL) || (loader == NULL) || (!xud_loader_is_open(loader)))
        return XUD_INVALID_ARG;

    if (window->surface == VK_NULL_HANDLE)
        return XUD_ALREADY; /* already closed - reported, never a double free */

    if (!xud_loader_instance_bound(loader, window->surfaceInstance))
        return XUD_UNSUPPORTED; /* a surface created for one instance must not be destroyed through
                                  another's rows */
    if (loader->instanceTable.vkDestroySurfaceKHR == NULL)
        return XUD_UNSUPPORTED;

    loader->instanceTable.vkDestroySurfaceKHR(window->surfaceInstance, window->surface, NULL);
    window->surface = VK_NULL_HANDLE;
    window->surfaceInstance = VK_NULL_HANDLE;
    return XUD_OK;
}

/// See xud_window.h. A window that never got a GLFW window frees without a GLFW call - the handle
/// less-teardown rule the instance and device layers apply to a struct whose handle is VK_NULL_HAND
/// LE, and what makes a synthetic XudWindow in a test headless-testable.
XudResult xud_window_close(XudWindow** window) {
    XudWindow* closed = NULL;

    if (window == NULL)
        return XUD_INVALID_ARG;

    closed = *window;
    if (closed == NULL)
        return XUD_ALREADY; /* already closed - reported, never a double free */

    if (closed->surface != VK_NULL_HANDLE)
        return XUD_INVALID_ARG; /* the surface belongs to this window: destroying the window under
                                  it would leave a surface whose window is gone, and xud_window_sur
                                  face_close() is the rung that destroys it first */

    if (closed->glfwWindow != NULL) {
        glfwDestroyWindow(closed->glfwWindow);
        if (xudGlfwLiveWindowCount != 0u)
            xudGlfwLiveWindowCount -= 1u;
        if (xudGlfwLiveWindowCount == 0u)
            glfwTerminate(); /* GLFW's own last rung: its state is process-level, so the last wind
                               ow to close is the one that tears it down - see the count above */
    }

    free(closed);
    *window = NULL;
    return XUD_OK;
}
