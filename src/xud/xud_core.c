/***********************************************************************************************
 *
 *   xud_core.c - XudContext lifecycle
 *
 *   Owns : argument validation, the context shell allocation, the error-code names, the runtime
 *          Vulkan query, and teardown. Every layer below it is reached from here, and only through
 *          its own header: loader, diagnostics, window, instance, physical, device, swapchain, re
 *          nderer, handle table. Nothing in this file calls a Vulkan or a GLFW entry point directly
 *          - each one of them is called by the rung that owns it, the Vulkan ones through a row of t
 *          he loader's Volk tables - which is what keeps this file free of any Vulkan or GLFW symbol
 *          the linker could see, and what keeps a machine with no driver and a machine with no windo
 *          w system both only ever getting XUD_UNSUPPORTED, which is what keeps the headless test ru
 *          n green. The handle layer is the one rung here that calls nothing at all: a handle is an
 *          index into this layer's own arrays, so opening and closing the table is host work. Also
 *          owned from here: the public frame pair xud_begin_drawing()/xud_end_drawing(), which cal
 *          ls no driver function at all - it reaches the renderer rung's one frame call and the
 *          batch rung's one reset, and both of those rungs own the driver calls.
 *   Layer: core (bottom of the public API). Sees xud.h, xud_core.h and every layer below it (loader,
 *          diagnostics, window, instance, physical, device, swapchain, renderer, handle table and
 *          batch. The
 *          diagnostics struct is opened only when XUD_VALIDATE asked for validation, and it sits bet
 *          ween the loader and the instance in both the open order and the teardown order for the re
 *          ason the extension gives it: its messenger is destroyed through the VkInstance that owns
 *          it, which must still be alive at that call. The window sits above the instance for the sa
 *          me kind of reason - glfwCreateWindowSurface resolves the window system's surface command
 *          through vkGetInstanceProcAddr against the instance it is handed, so the instance cannot e
 *          xist before the window says which extensions it needs. The renderer sits above the swap
 *          chain because the frame loop inside it submits and presents through that device's queues
 *          and against that swapchain's images, and it sits below the handle table because a handle
 *          names an object drawn WITH it, not one inside it. The handle table sits ABOVE every Vulkan
 *          rung in the open order and therefore FIRST in the teardown order: it is the storage rung f
 *          or the objects a public handle names, and no such object can exist before the context tha
 *          t will own it does, so the table is the last rung opened and the first closed.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include "xud_core.h"
#include "xud.h"
#include "xud_batch.h" /* Required for: xud_batch_reset, the public frame boundary's opening half runs on, and xud_batch_growth_asked/xud_batch_grow, the capacity ask the same half reads and the recreate that answers it */
#include "xud_camera.h" /* Required for: xud_camera_validate, the public camera pair's refusal */
#include "xud_descriptor.h"
#include "xud_device.h"
#include "xud_diagnostics.h"
#include "xud_handle.h"
#include "xud_instance.h"
#include "xud_loader.h"
#include "xud_physical.h"
#include "xud_renderer.h"
#include "xud_swapchain.h"
#include "xud_window.h"
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

    /* The allocator, above the device and below the renderer: the one GPU-memory rung, and the one
       owner of every VkDeviceMemory this tree creates. It sits here because its whole input is alrea
       dy above it - the memory rows the physical layer recorded (VkPhysicalDeviceMemoryProperties is
       embedded in the caps, so the type rule and the heap-size rule are read off a record, not asked
       of the driver) and the device whose table the allocate/free rows come from. It is opened befor
       e the renderer because the renderer is what will name the buffers and images carved out of it,
       and it is opened here rather than on the first buffer because it is per context: two contexts
       have two allocators, and a range from one must be refused by the other rather than index the
       wrong block array - the same reason the handle table above is opened eagerly. Opening it costs
       nothing: xud_allocator_open() is one host allocation, calls no driver function, and creates no
       block, so a context that comes back XUD_OK owns an allocator with no VkDeviceMemory in it at
       all - there is still no buffer, no vertex stream, no descriptor set and no batch above it, and
       this rung is the storage and the rules those steps will ask for, not their allocation. A refus
       al here is XUD_OUT_OF_MEMORY only, for the same reason xud_handle_table_open() refuses.
     */
    result = xud_allocator_open(&context->allocator);
    if (result != XUD_OK)
        goto fail;

    /* The descriptor rung, above the allocator and below the renderer: the two set layouts of the
       plan's descriptor table (set 0 per-frame, set 1 per-material), the pool sized for both, set 0
       itself, the uniform buffer that holds the per-frame block and the write that binds it into t
       he set. It sits here because of what it needs and what needs it: the allocator below for the
       memory the buffer is carved out of, the caps for the limits the layouts and the push range a
       re gated on, and the renderer above for the pipeline layout that names them. It is opened eag
       erly rather than on the first draw for the same reason the allocator and the handle table are
       : the layouts and the pipeline layout are created ONCE at init (the repo's rule), so a set la
       yout created per object would be a layout recreated per frame, and the sampler array's bound
       is a ceiling that cannot grow without recreating the pipeline. What is live after this rung i
       s set 0 and its buffer; set 1's layout is declared and its set is not, because the texture st
       ep is what allocates it from the same pool. A refusal here is a driver refusal (a nil row, a
       refused create) or the caps gate above refusing a layout this machine's limits cannot have.
     */
    result
        = xud_descriptor_open(&context->descriptor, context->loader, context->device, &context->physicalDevice->caps, context->allocator);
    if (result != XUD_OK)
        goto fail;

    /* The renderer, above the swapchain: the pipeline, the render pass, one framebuffer per swap
       chain image and the frame loop that draws one triangle into them. It sits here because every
       thing it needs is above it already - the device whose queues it submits through, and the swap
       chain whose chosen format and image count the pass and the framebuffer array are built from -
       and because the handle table above it is the only rung that can name an object drawn with it.
       It is the rung that makes the first triangle exist: a context that comes back XUD_OK has a pi
       peline, a render pass, a framebuffer per image and a command buffer, and xud_renderer_draw() i
       s the entry that runs one frame through them (see xud_renderer.h - the public entry above it i
       s what the shapes/text steps will call). A refusal here is a driver refusal: a nil row of the
       loader's device table for one of the create/destroy names this rung needs, or a swapchain wit
       h no chosen format.
     */
    result = xud_renderer_open(&context->renderer, context->loader, context->device, &context->physicalDevice->caps, context->descriptor,
        context->allocator, context->swapchain);
    if (result != XUD_OK)
        goto fail;

    /* The handle table, above the renderer and the swapchain: it is the storage rung for the object
       kinds a public XudHandle of xud.h names, and no such object can exist before the context that
       will own it does, so this is the last rung opened and xud_close_context() closes it FIRST. It
       is pure - one host allocation, no driver call, no Vulkan or GLFW symbol - so it cannot fail fo
       r a driver reason: the only answer here is XUD_OUT_OF_MEMORY, and XUD_INVALID_ARG cannot happe
       n because the shell this bring-up allocated is fresh and its field is NULL. Opening it change
       s nothing about what a caller can do: xud.h declares the handle TYPE and no function that take
       s or fills one, so no handle exists in a caller's hands yet, and the object steps (textures,
       fonts, meshes, materials, models) are what will hand one out - the renderer below it owns the
       pipeline and the batch-shaped rungs, but nothing that a handle can name yet. The table is open
       ed now rather than on the first object because it is per context - two contexts have two tabl
       es, and a handle from one is refused by the other - and a table that only some contexts own wo
       uld make that refusal depend on which objects a context happened to create.
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

    /* Destroy order is the reverse of creation: handle table -> renderer -> allocator -> swapchain
       -> device -> surface -> window -> physical -> instance -> diagnostics -> loader. The handle
       table goes FIRST because it is the top of the tree: a handle records an object of every rung below it, so a
       table still holding live slots is a table naming objects the rungs below are about to retire,
       and it is the last rung opened. It calls nothing - a slot's record is still the sentinel the h
       andle layer opens it with, because the object steps that would fill it (a texture, a font, a
       mesh, a material, a model) are not in the tree even though the renderer rung below it is - so
       freeing the table frees records and retires nothing of the driver's, and the rungs below still
       have to tear their own objects down after it. The renderer goes next because the frame loop in
       side it is the one thing in this context that can still have a presentation in flight: its ow
       n close waits the fence of the image that presentation used before it destroys the command bu
       ffer, the semaphore and the fences, and only then can the swapchain whose images were presen
       ted go. The allocator goes next because the renderer above it is what will name the buffers and
       images carved out of it, and a VkDeviceMemory is created FROM the VkDevice below it - so it die
       s after the renderer and before the swapchain and the device, and its own close runs that device
       's wait-idle guard first for the same reason the swapchain's close gives it: a submission or a
       presentation may still be reading the very blocks being freed. The swapchain goes next because
       it is presented through a queue of the device below it, and its own close runs that device's
       wait-idle guard - a presentation may still be in fligh
       t against the swapchain's images, so the guarantee that nothing is in flight has to come befo
       re the images go, and it is the last Vulkan rung created below the allocator, so it is the fi
       rst of the swapchain/device pair torn down. The device goes next because a VkDevice is created FROM the physical device below
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
    rung = xud_renderer_close(&victim->renderer, victim->device, victim->loader, victim->allocator);
    if ((rung != XUD_OK) && (rung != XUD_ALREADY) && (first == XUD_OK))
        first = rung;
    rung = xud_descriptor_close(&victim->descriptor, victim->loader, victim->device, victim->allocator);
    if ((rung != XUD_OK) && (rung != XUD_ALREADY) && (first == XUD_OK))
        first = rung;
    rung = xud_allocator_close(&victim->allocator, victim->loader, victim->device);
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

/*-----------------------------------------------------------------------------------------------
 * The public frame pair, above the context
 *
 * Both halves reach the renderer rung and nothing else, which is the point of the pair: a caller
 * names a frame boundary and a shape, never a pipeline, a pass, a framebuffer row, a command buf
 * fer or a fence. The opening half is the batch's reset and the closing half is the renderer's one
 * frame call, so the order the frame loop states - appends, then the flush - is the order the pub
 * lic pair states, and it is raylib's own order for BeginDrawing/EndDrawing (raylib.h 1100 says
 * EndDrawing() is what draws everything and swaps buffers).
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_begin_drawing(XudContext* context) {
    XudResult result = XUD_INVALID_ARG;

    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if (context->renderer == NULL) {
        /* A context with no renderer is a context that never completed bring-up: xud_init_context()
           hands back NULL for that case, so this is a caller holding on rather than a state the li
           brary leaves behind. Reported, not ignored - and XUD_UNSUPPORTED is the code the public
           surface already uses for "this machine cannot host what was asked for". */
        return XUD_UNSUPPORTED;
    }

    /* The reset is the frame boundary, and it is idempotent: xud_frame_draw() already reset the
       batch at the end of the frame it completed, so a frame that ran to the end arrives here emp
       ty already. The reset is still the whole point of this call - the frame about to be drawn
       starts from an empty batch whatever the rung below left behind - and a batch that is empty
       resets to empty, which is why the call is cheap rather than a promise.

       The capacity ask is answered BEFORE the reset, and that order is the whole point: the ask
       belongs to the frame that was just refused, and a shape's append is atomic, so that frame
       stopped one shape short of full and its live counts say nothing about it. Doubling is the
       batch's whole growth policy, so a scene bigger than the initial capacity settles in a handf
       ul of frames and then stays - xud_batch_grow clears the ask it answered, so a frame that now
       fits asks nothing and the loop does not run again. The grow is a recreate, and this is the
       one place the public boundary sits between two recordings: xud_frame_draw() waited and reset
       the frame's fence before it returned, so nothing is in flight that could still be reading a
       buffer the recreate retires, which is the same guard a swapchain recreate owes its old handle
       and a depth recreate owes the attachment it replaces. */
    for (;;) {
        uint32_t asked = 0u;

        result = xud_batch_growth_asked(context->renderer->batch, &asked);
        if (result != XUD_OK) {
            return result;
        }
        if (asked == 0u) {
            break;
        }
        if ((context->loader == NULL) || (context->device == NULL) || (context->physicalDevice == NULL) || (context->allocator == NULL)) {
            /* A context that owns a renderer but not the rungs the grow needs is a context that
               never completed bring-up. Reported rather than dereferenced - and the ask stays set,
               so the frame below is refused by its appends rather than drawn short. */
            return XUD_UNSUPPORTED;
        }
        result = xud_batch_grow(
            context->renderer->batch, context->loader, context->device, &context->physicalDevice->caps, context->allocator);
        if (result == XUD_UNSUPPORTED) {
            /* At the batch's bound the batch is FULL, and full is a decision rather than a growth
               step: the frame below is refused by its appends and the ask stays set, so the next
               boundary asks again rather than this loop spinning on a grow that cannot happen. */
            break;
        }
        if (result != XUD_OK) {
            /* A grow that failed left the batch retired - no buffers, no mapping - and a boundary
               that cannot open a frame is reported, not papered over. */
            return result;
        }
    }

    return xud_batch_reset(context->renderer->batch);
}

XudResult xud_end_drawing(XudContext* context) {
    XudResult result = XUD_INVALID_ARG;
    uint32_t imageIndex = 0u;

    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if ((context->renderer == NULL) || (context->loader == NULL) || (context->device == NULL) || (context->swapchain == NULL)
        || (context->physicalDevice == NULL) || (context->window == NULL) || (context->allocator == NULL)) {
        /* Every rung the frame and the rebuild below need is a context field, and a context missin
           g one is a context that cannot run a frame at all - the same answer the half above gives
           for the same reason. */
        return XUD_UNSUPPORTED;
    }

    result = xud_renderer_draw(context->renderer, context->loader, context->device, context->swapchain, &imageIndex);
    if (result != XUD_OK) {
        return result;
    }

    /* The recreate ask, acted on one rung above where it was left. xud_renderer_resize() is the re
       nder of XudFrame.recreateWanted and it answers XUD_ALREADY when nothing was asked for, which
       is what makes calling it here every frame cheap rather than a rebuild every frame: the flag
       is the ask, and the two swapchain-boundary codes are the only things that set it. A frame
       that answered VK_SUBOPTIMAL_KHR or VK_ERROR_OUT_OF_DATE_KHR ran to the end above - the image
       was usable and the presentation happened - and this is the rung that replaces the swapchain
       before the NEXT acquire, at the window's FRAMEBUFFER extent rather than at its requested one.
       The resize is not this rung's to refuse: a rebuild that failed is reported, and the renderer
       is left with the rungs it still has. */
    result = xud_renderer_resize(&context->renderer, context->loader, context->device, &context->physicalDevice->caps, context->allocator,
        context->physicalDevice, context->window, &context->swapchain);
    if ((result != XUD_OK) && (result != XUD_ALREADY)) {
        return result;
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The public camera pair, above the context
 *
 * raylib's BeginCamera3D()/EndCamera3D() (reference/raylib/rcamera.h) read in the repo's own way:
 * raylib's camera is a global the library holds, which is why its Begin call cannot validate the
 * camera it is handed and why two windows cannot have two cameras; here the record is the caller's
 * and the ONE place it can be stored is the renderer that owns the frame loop, so the validation
 * happens at the call that asks and the storage happens in the owner that holds it. A camera is a
 * value in the renderer rather than a pointer to a caller's record, so a caller's local camera go
 * ing out of scope at the end of a frame cannot leave the next frame reading a dead record.
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_begin_camera_3d(XudContext* context, const XudCamera3D camera) {
    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if (context->renderer == NULL) {
        /* The same answer the frame pair above gives for the same reason: a context with no rendere
           r is a context that never completed bring-up, and there is no owner here to store the
           camera in. Reported, not ignored.
        */
        return XUD_UNSUPPORTED;
    }

    /* Validated BEFORE it is stored, which is the half the pair buys against raylib's: a degenerate
       camera is refused at the call that asked for it rather than at the frame that draws it, and
       the camera that was already live is left alone rather than replaced by a record that would
       produce a NaN matrix. The frame's own math asks the same test again below, because the rendere
   r's record is a value a caller could have tampered since this call.
    */
    if (xud_camera_validate(&camera) != XUD_OK) {
        return XUD_INVALID_ARG;
    }

    context->renderer->camera = camera;
    context->renderer->cameraWanted = 1u;

    return XUD_OK;
}

XudResult xud_end_camera_3d(XudContext* context) {
    if (context == NULL) {
        return XUD_INVALID_ARG;
    }
    if (context->renderer == NULL) {
        return XUD_UNSUPPORTED;
    }
    if (context->renderer->cameraWanted == 0u) {
        /* No camera live: the state this call asks for is the state present, which is the repo's
           idiom for an idempotent ask rather than a refusal. The record itself is left as it was
           given - the flag is what governs, and a begin after here replaces the whole record.
        */
        return XUD_ALREADY;
    }

    context->renderer->cameraWanted = 0u;

    return XUD_OK;
}
