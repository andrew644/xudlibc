/***********************************************************************************************
 *
 *   xud_core.c - XudContext lifecycle
 *
 *   Owns : argument validation, the context shell allocation, the error-code names, the runtime
 *          Vulkan query, and teardown. Every layer below it is reached from here, and only through
 *          its own header: loader, diagnostics, window, instance, physical, device, swapchain,
 *          handle table. Nothing in this file calls a Vulkan or a GLFW entry point directly - each
 *          one of them is called by the rung that owns it, the Vulkan ones through a row of the loa
 *          der's Volk tables - which is what keeps this file free of any Vulkan or GLFW symbol the
 *          linker could see, and what keeps a machine with no driver and a machine with no window
 *          system both only ever getting XUD_UNSUPPORTED, which is what keeps the headless test run
 *          green. The handle layer is the one rung here that calls nothing at all: a handle is an
 *          index into this layer's own arrays, so opening and closing the table is host work.
 *   Layer: core (bottom of the public API). Sees xud.h, xud_core.h and every layer below it (loader,
 *          diagnostics, window, instance, physical, device, swapchain, handle table). The diagnostics
 *          struct is opened only when XUD_VALIDATE asked for validation, and it sits between the
 *          loader and the instance in both the open order and the teardown order for the reason
 *          the extension gives it: its messenger is destroyed through the VkInstance that owns it,
 *          which must still be alive at that call. The window sits above the instance for the same
 *          kind of reason - glfwCreateWindowSurface resolves the window system's surface command
 *          through vkGetInstanceProcAddr against the instance it is handed, so the instance cannot
 *          exist before the window says which extensions it needs. The handle table sits ABOVE the
 *          swapchain in the open order and therefore FIRST in the teardown order: it is the storage
 *          rung for the objects a public handle names, and no such object can exist before the
 *          context that will own it does, so the table is the last rung opened and the first closed.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include "xud_core.h"
#include "xud.h"
#include "xud_device.h"
#include "xud_diagnostics.h"
#include "xud_handle.h"
#include "xud_instance.h"
#include "xud_loader.h"
#include "xud_physical.h"
#include "xud_swapchain.h"
#include "xud_window.h"

#include <stdbool.h>
#include <stdlib.h>

/* Internal: allocate a zeroed context shell. Not part of the public API - the only way
   user code may obtain a context is xud_init_context(), so xud_close_context() can rely
   on every context it frees being heap-allocated. */
static XudResult xud_context_create(XudContext** outContext, uint32_t width, uint32_t height) {
    XudContext* context = NULL;

    if (outContext == NULL || *outContext != NULL)
        return XUD_INVALID_ARG;
    if (width == 0u || height == 0u)
        return XUD_INVALID_ARG;
    if (width > XUD_MAX_WINDOW_EXTENT || height > XUD_MAX_WINDOW_EXTENT)
        return XUD_INVALID_ARG;

    context = calloc(1, sizeof(XudContext));
    if (context == NULL)
        return XUD_OUT_OF_MEMORY;

    context->width = width;
    context->height = height;
    context->validate = XUD_VALIDATE;

    *outContext = context;
    return XUD_OK;
}

const char* xud_result_name(XudResult result) {
    switch (result) {
    case XUD_OK:
        return "XUD_OK";
    case XUD_UNSUPPORTED:
        return "XUD_UNSUPPORTED";
    case XUD_INVALID_ARG:
        return "XUD_INVALID_ARG";
    case XUD_OUT_OF_MEMORY:
        return "XUD_OUT_OF_MEMORY";
    case XUD_OUT_OF_RESOURCES:
        return "XUD_OUT_OF_RESOURCES";
    case XUD_ALREADY:
        return "XUD_ALREADY";
    case XUD_LOST:
        return "XUD_LOST";
    case XUD_VULKAN_ERROR:
        return "XUD_VULKAN_ERROR";
    case XUD_INTERNAL_ERROR:
        return "XUD_INTERNAL_ERROR";
    }
    return "XUD_OUT_OF_RANGE";
}

XudResult xud_vulkan_supported(void) {
    XudLoader* loader = NULL;
    XudResult result = XUD_INVALID_ARG;

    result = xud_loader_open(&loader);
    if (result != XUD_OK) {
        if (loader != NULL)
            (void)xud_loader_close(&loader);
        return result;
    }

    /* xud_loader_open() already refused a loader that did not answer all three loader-level row
       s, so reaching here is the answer: a Vulkan loader is present and reachable. The rows are
       not counted again here - the count check lives in one place, in the rung that fills them.
       The loader this query opened is closed straight away: it is a runtime question, not a cont
       ext, and leaving it open would be a second, leaked owner of Volk's globals. */
    (void)xud_loader_close(&loader);
    return XUD_OK;
}

XudResult xud_init_context(XudContext** outContext, uint32_t width, uint32_t height) {
    XudContext* context = NULL;
    XudResult result = XUD_INVALID_ARG;
    /* GLFW's own required instance names, read off the window this bring-up opens and handed to th
       e instance layer below. XUD_INSTANCE_EXTRA_MAX is the shared bound both headers state
       (xud_win dow.h sizes its own storage by it, xud_instance.h declares the parameter by it, and
       both heade rs static_assert the wanted names of every layer fit XUD_MAX_INSTANCE_EXTENSIONS),
       so the stora ge here is that bound and nothing else.
     */
    const char* requiredNames[XUD_INSTANCE_EXTRA_MAX] = { NULL };
    uint32_t requiredCount = 0u;
    /* GLFW's answer to "can this physical device present to this window's system" - the gate below
       r eads it, and it is a local rather than state because it is a question about the pair, not
       someth ing the context owns.
     */
    bool presents = false;

    if (outContext == NULL || *outContext != NULL)
        return XUD_INVALID_ARG;

    result = xud_context_create(&context, width, height);
    if (result != XUD_OK)
        return result;

    /* Loader bring-up: volkInitialize() dlopens libvulkan.so.1 by name and resolves the loader-
       level rows. XUD_UNSUPPORTED here means "no Vulkan loader on this machine". */
    result = xud_loader_open(&context->loader);
    if (result != XUD_OK)
        goto fail;

    /* The diagnostics layer, before the instance: its selected layer list and its
       VkValidationFeaturesEXT are what VkInstanceCreateInfo carries, so it has to be opened
       before vkCreateInstance runs. Gated on `validate`, the XUD_VALIDATE snapshot
       xud_context_create took: a release build has no diagnostics struct at all, which is the "no
       layer requested, no validation request, no messenger" state, and context->diagnostics stays
       NULL through the whole teardown ladder below. XUD_UNSUPPORTED from this call means the loader
       is open but does not answer vkEnumerateInstanceLayerProperties - a different reason from "no
       loader", and worth telling apart. */
    if (context->validate != 0u) {
        result = xud_diagnostics_open(&context->diagnostics, context->loader);
        if (result != XUD_OK)
            goto fail;
    }

    /* The window rung, BEFORE the instance and for a reason that is load-bearing: glfwCreateWindow
       Surface resolves vkCreateWaylandSurfaceKHR through vkGetInstanceProcAddr against the instance
       i t is handed (src/external/glfw/src/wl_window.c 145), so the instance a surface is created
       for has to carry GLFW's own required instance extensions, and those are only knowable once a
       window exi sts. That is what grew xud_instance_create()'s extraNames/extraCount parameter.
       glfwCreateWindow takes int extents, so the window layer refuses anything above INT_MAX as
       XUD_INVALID_ARG rather t han wrapping it, and it asks glfwVulkanSupported() first - GLFW's
       loader query, which answers GLFW_ FALSE when GLFW cannot find a loader and is why a machine
       with no loader refuses here rather than cr eating a window it could never give a surface.
       XUD_UNSUPPORTED from either call means this machine h as no window system for a context to be
       shown on, which is a fourth reason to tell apart from "no lo ader", "no instance" and "no
       device".
     */
    result = xud_window_open(&context->window, width, height);
    if (result != XUD_OK)
        goto fail;
    result = xud_window_required_instance_extensions(context->window, requiredNames, &requiredCount);
    if (result != XUD_OK)
        goto fail;

    /* The instance layer: vkCreateInstance at the VK_API_VERSION_1_0 floor, with only the
       advertised extensions enabled and, when the diagnostics struct is there, its layers,
       its wanted extensions and its validation-feature request handed over - the instance
       layer checks every one of those against what the driver advertised and enables nothing
       that was not. The window layer's required names come along now: an instance without VK_KH
       R_wayland_surface enabled is an instance glfwCreateWindowSurface will answer VK_ERROR_EXTENS
       ION_NOT_PRESENT for, so the merged list is what makes a surface possible at all. VK_KHR_surfa
       ce is one of the three this layer already wants, so the merged instance enables four names
       her e, not five - the instance layer's dedup is what makes that so. XUD_UNSUPPORTED here
       means the l oader is present but refuses to create an instance - a different reason from "no
       loader", and wort h telling apart. */
    result = xud_instance_create(&context->instance, context->loader, context->diagnostics, requiredNames, requiredCount);
    if (result != XUD_OK)
        goto fail;

    /* The messenger, after the instance: vkCreateDebugUtilsMessengerEXT is an instance-level
       command, so it needs a live VkInstance and cannot be created inside xud_diagnostics_open().
       vkGetInstanceProcAddr answers NULL for it unless VK_EXT_debug_utils was enabled on the
       instance in hand, and the instance layer enabled it only after checking what the driver
       advertised - so XUD_UNSUPPORTED is the honest "this driver has no log side for us" answer,
       not a failure: the validation layers are still enabled and still print their own output,
       only our counting callback is missing. Any other failure is torn down below. */
    if (context->diagnostics != NULL) {
        result = xud_diagnostics_create_messenger(context->diagnostics, context->loader, context->instance->instance);
        if (result == XUD_UNSUPPORTED)
            result = XUD_OK; /* no log side on this driver, see the comment above */
        if (result != XUD_OK)
            goto fail;
    }

    /* The physical layer, after the instance and before anything that needs a device: it binds
       this layer's slots through the live instance, enumerates the devices, ranks one and recor
       ds its caps - properties, features, memory, the advertised device extensions, the chosen
       queue family, apiVersion read back. XUD_UNSUPPORTED here means the loader is open and an
       instance exists but the driver has no device this layer can take: no device enumerated, no
       graphics queue family, or a driver below the instance floor. That is a third reason to tel
       l apart from "no loader" and "no instance", and it is why a machine with no driver and a d
       river that cannot be driven both stay off the GPU-test path. The handle is a query result,
       so the teardown below frees memory and calls no driver function: Vulkan has no vkDestroyP
       hysicalDevice (grepped in these headers, the name does not exist).
     */
    result = xud_physical_open(&context->physicalDevice, context->loader, context->instance->instance);
    if (result != XUD_OK)
        goto fail;

    /* GLFW's presentation query, asked about the physical layer's CHOSEN queue family - the presen
       t half of the one-family rule the physical layer's graphics check is the other half of (there
       is no VK_QUEUE_PRESENT_BIT, grepped: the name does not exist in these headers, so
       presentation is a per-window-system question and not a caps bit). A refusal here is a window
       system this physical d evice cannot present to, which is XUD_UNSUPPORTED rather than a bad
       argument: nothing about the req uest was wrong, the machine cannot do it. Refusing is also
       what the swapchain step below would have had to do anyway - a swapchain on a surface this
       device cannot present to is a swapchain nothing ca n present through.
     */
    result = xud_window_presentation_support(&presents, context->window, context->instance, context->physicalDevice);
    if (result != XUD_OK)
        goto fail;
    if (!presents) {
        result = XUD_UNSUPPORTED;
        goto fail;
    }

    /* The device layer: one VkDeviceQueueCreateInfo row for the family the physical layer chose,
       VkD eviceCreateInfo carrying that layer's FEATURE INTERSECTION and only the device extensions
       that laye r saw advertised, and the two role queues resolved once here rather than on every
       frame. XUD_UNSUPPO RTED from here means the device extensions or features this layer wants
       were not advertised, or a row this layer needs is NULL; XUD_VULKAN_ERROR means the driver
       answered nothing for a queue pair the row itself asked for. A device with no queue in it is
       refused by that layer, and its own fail ladder destroy s the device it just made, so a
       refusal below here still leaks nothing.
     */
    result = xud_device_open(&context->device, context->loader, context->physicalDevice, context->instance->instance);
    if (result != XUD_OK)
        goto fail;

    /* The surface, last of the two rungs that need both the instance's rows and a window for: it is
       the one Vulkan object this bring-up needs both the instance's rows and a window for, and
       xud_window_surface_create() refuses rather than creating a surface this layer cannot destroy
       - the destroy row is checked before the create. It is last in creation order among the rungs
       that name a surface and second in teardown order (swapchain -> device -> surface -> window),
       which is the reason the order below is what it is. */
    result = xud_window_surface_create(context->window, context->loader, context->instance);
    if (result != XUD_OK)
        goto fail;

    /* The swapchain, last of the ladder and the only rung that needs a device AND a surface: it is
       created for the surface the window owns and on the device the ladder above opened, so it is
       the one bring-up step that sits below both. Its inputs are the three surface queries' answers
       (minImageCount, the advertised (format, colorSpace) pairs, the advertised present modes) and
       the window's own extent - nothing here is a constant this layer hoped for, and the layer
       answers XUD_UNSUPPORTED for an extent outside the surface's own minImageExtent/maxImageExtent
       rather than asking the driver. XUD_UNSUPPORTED from here means the surface queries refused,
       the surface advertises no format / no present mode / no COLOR_ATTACHMENT usage, or a
       swapchain row of the loader's device table is nil - VK_KHR_swapchain not enabled on the
       instance is one of the reasons. The presentation check above already refused a window system
       this physical device cannot present to, which is the gate this rung would otherwise have had
       to restate. */
    result = xud_swapchain_open(&context->swapchain, context->loader, context->physicalDevice, context->window, context->device);
    if (result != XUD_OK)
        goto fail;

    /* The handle table, above the swapchain: it is the storage rung for the object kinds a public
       XudHandle of xud.h names, and no such object can exist before the context that will own it
       does, so this is the last rung opened and xud_close_context() closes it FIRST. It is pure -
       one host allocation, no driver call, no Vulkan or GLFW symbol - so it cannot fail for a
       driver reason: the only answer here is XUD_OUT_OF_MEMORY, and XUD_INVALID_ARG cannot happen
       because the shell this bring-up allocated is fresh and its field is NULL. Opening it changes
       nothing about what a caller can do: xud.h declares the handle TYPE and no function that takes
       or fills one, so no handle exists in a caller's hands yet, and the object steps (textures,
       fonts, meshes, materials, models) are what will hand one out. The table is opened now rather
       than on the first object because it is per context - two contexts have two tables, and a
       handle from one is refused by the other - and a table that only some contexts own would make
       that refusal depend on which objects a context happened to create.
     */
    result = xud_handle_table_open(&context->handleTable);
    if (result != XUD_OK)
        goto fail;

    *outContext = context;
    return XUD_OK;

fail:
    /* One teardown for every refusal, and it is xud_close_context()'s own order: the ladder above
       is the crea tion order, so the rung that just refused left a context holding exactly the
       rungs that succeeded, and closi ng it frees them all. The answer that travels is the refusing
       rung's, not the teardown's - the init failure i s the more informative of the two, and a
       teardown refusal on a partial context is a broken loader, which xud_ instance.h already
       documents as not something this layer papers over. The context struct itself is freed eith er
       way and the out-param is cleared, so a rejected request hands back nothing and a valid one
       never hands back a half-built context.
     */
    (void)xud_close_context(&context);
    *outContext = NULL;
    return (result != XUD_OK) ? result : XUD_INTERNAL_ERROR;
}

XudResult xud_close_context(XudContext** context) {
    XudContext* victim = NULL;
    XudResult first = XUD_OK;
    XudResult rung = XUD_OK;

    if (context == NULL)
        return XUD_INVALID_ARG;
    if (*context == NULL)
        return XUD_ALREADY;

    victim = *context;

    /* Destroy order is the reverse of creation: handle table -> swapchain -> device -> surface ->
       window -> physical -> instance -> diagnostics -> loader. The handle table goes FIRST because
       it is the top of the tree: a handle records an object of every rung below it, so a table still
       holding live slots is a table naming objects the rungs below are about to retire, and it is
       the last rung opened. It calls nothing - a slot's record is still the sentinel the handle laye
       r opens it with, because the rungs that would fill it are not in the tree - so freeing the
       table frees records and retires nothing of the driver's, and the rungs below still have to
       tear their own objects down after it. The swapchain goes next because it is
       presented through a queue of the device below it and its own close runs that device's
       wait-idle guard - a presentation may still be in flight against the swapchain's images, so
       the guarantee that nothing is in flight has to come before the images go, and it is the last
       Vulkan rung created, so it is the first torn down. The device goes next because a VkDevice is
       created FROM the physical device below it and everything that submits through it belongs to
       it, and its close is the wait-idle guard then vkDestroyDevice through a row of the loader's
       device table. The surface goes next because vkDestroySurfaceKHR takes the owning VkInstance,
       which is still alive at this point; the window goes after its surface because
       xud_window_close() refuses a window whose surface is live and may run glfwTerminate(), GLFW's
       own teardown, which walks GLFW's window list and must not do that while a surface still names
       a window. The physical rung is the free-only one: a VkPhysicalDevice is a query result, not
       an object the driver allocated for us, so it frees the caps' extension array and the struct
       and calls no driver function at all, which is why it can run before the instance goes away
       without owing it anything. The diagnostics struct is the one entry that is not strictly in
       that slot - its messenger belongs to the VkInstance, and vkDestroyDebugUtilsMessengerEXT
       takes that instance while it is still alive, so xud_instance_destroy() tears the messenger
       down first, through the instance it is about to free, and xud_diagnostics_close() runs after
       it and only frees the struct (it finds the messenger already VK_NULL_HANDLE there, and the
       borrowed instance handle cleared, so nothing is called through a dying instance). Every rung
       is XUD_ALREADY on a field that was never filled, which is why a context rejected part-way
       through bring-up is closed by this same code path, and every one of them clears its out
       handle before freeing, so a context torn down twice cannot double-free. */
    rung = xud_handle_table_close(&victim->handleTable);
    if ((rung != XUD_OK) && (rung != XUD_ALREADY) && (first == XUD_OK))
        first = rung;
    rung = xud_swapchain_close(&victim->swapchain, victim->device, victim->loader);
    if ((rung != XUD_OK) && (rung != XUD_ALREADY) && (first == XUD_OK))
        first = rung;
    rung = xud_device_close(&victim->device, victim->loader);
    if ((rung != XUD_OK) && (rung != XUD_ALREADY) && (first == XUD_OK))
        first = rung;
    rung = xud_window_surface_close(victim->window, victim->loader);
    if ((rung != XUD_OK) && (rung != XUD_ALREADY) && (first == XUD_OK))
        first = rung;
    rung = xud_window_close(&victim->window);
    if ((rung != XUD_OK) && (rung != XUD_ALREADY) && (first == XUD_OK))
        first = rung;
    rung = xud_physical_close(&victim->physicalDevice);
    if ((rung != XUD_OK) && (rung != XUD_ALREADY) && (first == XUD_OK))
        first = rung;
    rung = xud_instance_destroy(&victim->instance, victim->loader, victim->diagnostics);
    if ((rung != XUD_OK) && (rung != XUD_ALREADY) && (first == XUD_OK))
        first = rung;
    rung = xud_diagnostics_close(&victim->diagnostics, victim->loader);
    if ((rung != XUD_OK) && (rung != XUD_ALREADY) && (first == XUD_OK))
        first = rung;
    rung = xud_loader_close(&victim->loader);
    if ((rung != XUD_OK) && (rung != XUD_ALREADY) && (first == XUD_OK))
        first = rung;

    free(victim);
    *context = NULL;

    /* The first rung that refused, if any did. XUD_ALREADY is not a refusal - it says the field was
       nev er filled, which is the state a context rejected part-way through bring-up is in - and is
       filtere d out above. XUD_LOST is not filtered: the device was still destroyed and still
       freed, and the an swer that travels is the guard's, so a caller sees that the driver said the
       device was gone rat her than a clean OK. A rung that refused because a row was NULL
       (XUD_UNSUPPORTED) left its struc t alive and un-freed - that is a broken loader, and
       reporting it is the honest answer, which is why the answer is propagated here rather than
       cast away as it was until this step.
     */
    return first;
}
