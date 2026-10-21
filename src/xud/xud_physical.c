/***********************************************************************************************
 *
 *   xud_physical.c - the physical device layer: the pure half, the driver half, and the
 *          free-only teardown
 *
 *   Owns : the whole contract of the physical layer. The parts that call no driver function -
 *          xud_physical_select() (the device policy), xud_physical_record_features() (the
 *          intersection S6 hands vkCreateDevice), xud_physical_select_queue_family() (the one-
 *          family rule), xud_physical_record_api_version() (the apiVersion read-back) - and the
 *          driver half, xud_physical_query_devices() and xud_physical_open() (landed today, at
 *          S5.4), which reach the driver only through slots of XudLoader's table bound through
 *          the live VkInstance handed in: this file names no vk* function directly and no Vulkan
 *          symbol is linked (AGENTS.md: there is no -lvulkan anywhere in the build). The three
 *          doubling-capacity queries here (the device list, the advertised device extensions,
 *          the queue families) are the shape xud_instance_query_extensions in xud_instance.c and
 *          xud_diagnostics_query_layers in xud_diagnostics.c already established - all three of
 *          these entry points have an input/output count and no count-only form, so a count can-
 *          not be read without an array. Probed here today (lavvk / vulkan_intel, TigerLake-LP
 *          GT2): **one** physical device enumerated, apiVersion 1.4.354, **196** device-level
 *          extensions with VK_KHR_swapchain among them (the caps' array lands at capacity 256
 *          for that count), **one** queue family whose flags are 0x1f - graphics, compute, trans-
 *          fer, sparse-binding, protected - with queueCount 1 and 36 valid timestamp bits, and
 *          all three features this layer wants present. lavvk also leaves the *2 wrappers alone -
 *          sType 0 in, 0 out, pNext NULL in, NULL out - which is why xud_physical_open() stamps
 *          each wrapper's sType itself and checks pNext rather than assuming it.
 *          xud_physical_close() frees memory and calls no driver function at all, because Vulkan
 *          has no vkDestroyPhysicalDevice (grepped in these headers: the name does not exist) -
 *          a VkPhysicalDevice is a query result, not an object the driver allocated for us.
 *   Layer: above the instance, below the device. The pure half touches no driver, so the whole
 *          of it stays headless-testable (AGENTS.md: pure logic lives in functions that take
 *          structs and never call the driver) - xud_test_physical_pure() and xud_test_physic
 *          al_negative() in tests/xud_test.c cover it with no device present; the driver half
 *          is what xud_test_gpu_physical() covers under XUD_GPU_TESTS=1, landed with it here
 *          (the plan's number for that GPU section is S5.7, and it landed with the driver half
 *          because a driver half with no test against it would be an untested path).
 *   In the archive: this source joined CMakeLists.txt's add_library(xud STATIC ...) with the
 *          other module sources when the driver half landed - the header's contract is complet
 *          e now, both halves are defined, so nothing has to be compiled into the test binar
 *          y specially and the test links the archive. ar -t build/libxud.a lists six object
 *          s here, xud_physical.c.o among them, and nm -u build/xud_test still lists no vk*
 *          symbol: every driver call this file makes is through a slot of the loader's table.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h"
#include "xud_loader.h"
#include "xud_instance.h"
#include "xud_vulkan.h"
#include "xud_physical.h"

#include <assert.h> /* Required for: static_assert, the compile-time checks below */
#include <stdlib.h> /* Required for: free, the one heap array xud_physical_close() releases */
#include <string.h> /* Required for: memset, the zeroed chosen row on the no-family path */

/*-----------------------------------------------------------------------------------------------
 * Compile-time facts this file's code depends on
 *-----------------------------------------------------------------------------------------------*/

/* The queue-family rule below tests one bit of a flag word, so the bit must exist and must be
   exactly one bit - a mask that covered two bits would quietly accept a family that carries
   only one of them. Verified here in vulkan_core.h (Vulkan-Headers 1.4.357): VK_QUEUE_GRAPHICS
   _BIT :2869 = 0x1, VK_QUEUE_COMPUTE_BIT :2870 = 0x2, VK_QUEUE_TRANSFER_BIT :2871 = 0x4. There
   is no VK_QUEUE_PRESENT_BIT anywhere in these headers (grepped across the whole vulkan include
   tree: zero hits), which is why the rule is a graphics check and presentation stays a per-
   window-system query (GLFW's glfwGetPhysicalDevicePresentationSupport at S7) - see
   xud_physical_select_queue_family below for what that means for the one-family rule. */
static_assert(VK_QUEUE_GRAPHICS_BIT != 0u, "the graphics bit must exist");
static_assert((VK_QUEUE_GRAPHICS_BIT & (VK_QUEUE_GRAPHICS_BIT - 1u)) == 0u, "the graphics check assumes exactly one bit");
static_assert((VK_QUEUE_GRAPHICS_BIT & VK_QUEUE_TRANSFER_BIT) == 0u, "graphics and transfer must be distinct bits, transfer is recorded but never required");

/* The feature AND below normalizes its output, and the normalization is written in terms of the
   two boolean values of these headers: VkBool32 (vulkan_core.h:2999) with VK_FALSE = 0 and VK_
   TRUE = 1. A driver that writes anything else into a feature field must not be able to smuggle
   a third value into the request S6 hands vkCreateDevice - see xud_physical_and_feature. */

/* The slots xud_physical_open() binds through xud_loader_bind() must be pointer-sized, the same
   rule xud_loader.c checks for the global-level ones and xud_instance.c adds for the instance-
   level ones it binds (xud_pfn_copy moves one word). vkGetPhysicalDeviceMemoryProperties2 is the
   one name here that the table did not already have: xud_loader.h grew a slot for it today and
   XUD_LOADER_SLOT_COUNT went 19 -> 20 (probed: it resolves through a live VkInstance, and answers
   NULL with VK_NULL_HANDLE in hand, so it belongs to the instance-level tier like the other six
   physical names). The non-2 vkGetPhysicalDeviceProperties form is slotted too but never bound
   here - a ranking policy has to read deviceType with no extension in hand, this layer reads the
   *2 form. */
static_assert(sizeof(PFN_vkEnumeratePhysicalDevices) == sizeof(void *), "the device-list slot must be pointer-sized");
static_assert(sizeof(PFN_vkGetPhysicalDeviceProperties2) == sizeof(void *), "the properties2 slot must be pointer-sized");
static_assert(sizeof(PFN_vkGetPhysicalDeviceFeatures2) == sizeof(void *), "the features2 slot must be pointer-sized");
static_assert(sizeof(PFN_vkGetPhysicalDeviceMemoryProperties2) == sizeof(void *), "the memoryProperties2 slot must be pointer-sized");
static_assert(sizeof(PFN_vkGetPhysicalDeviceQueueFamilyProperties2) == sizeof(void *), "the queue-family slot must be pointer-sized");
static_assert(sizeof(PFN_vkEnumerateDeviceExtensionProperties) == sizeof(void *), "the device-extension slot must be pointer-sized");
static_assert(VK_FALSE == 0u, "VK_FALSE is zero in these headers");
static_assert(VK_TRUE == 1u, "VK_TRUE is one in these headers");

/* The apiVersion read-back compares the driver's number with the instance floor. The encoding
   (VK_MAKE_API_VERSION, variant<<22 | major<<12 | minor<<4 | patch) is what makes that
   comparison mean "at or above", and the floor itself is asserted in xud_physical.h. */
static_assert(VK_API_VERSION_1_0 <= VK_API_VERSION_1_4, "the apiVersion encoding must be ordered, oldest version first");

/*-----------------------------------------------------------------------------------------------
 * The features this library wants: one owner per wanted list (AGENTS.md), this layer's decision
 *-----------------------------------------------------------------------------------------------*/

/* The list is intersected with what the driver reported in xud_physical_record_features(), so
   anything not wanted here stays VkFalseString in the request S6 hands vkCreateDevice, and
   anything wanted but not reported stays off too (AGENTS.md: an unverified feature stays off and
   the feature that needed it returns XUD_UNSUPPORTED later, never silently).

   Wanted, with the reason each one is wanted:
   - robustBufferAccess: the renderer suballocates one big vertex/index buffer pair, so an
     out-of-range index or attribute read must return zeroes rather than be undefined behaviour.
     It is core-guaranteed from Vulkan 1.1 up, so this costs nothing there; at the 1.0 floor it
     is a request, and a driver that lacks it still works - the library just loses that promise.
   - samplerAnisotropy: the textured pipeline filters textures, and anisotropic filtering is the
     only quality knob that is a feature rather than a sampler parameter. Absent, the sampler
     creation above falls back to linear, it does not fail.
   - wideLines: the line pipeline draws thicker than one pixel (raylib's DrawLine3D shape).
     Absent, the line pipeline keeps width 1, it does not fail.

   Deliberately not wanted, and why:
   - geometryShader / tessellationShader / tessellation-everything: no such pipeline in the
     design sketch, and Vulkan's own rule is that a driver may not report them unless it also
     reports the shader stages, so asking would be asking for a stage we never bind.
   - the whole textureCompression* trio: no compressed-texture path yet; asking for BC/ETC2/ASTC
     would make a driver that has only one of them look "supported" when the loader above has no
     way to decode any of them.
   - shaderStorageImage* / shaderImageGatherExtended / the shader*DynamicIndexing quartet /
     shaderFloat64 / shaderInt64 / shaderInt16 / shaderResourceResidency / shaderResourceMinLod:
     no storage images, no 64-bit or 16-bit shader types, no residency queries anywhere in the
     sketched pipelines. The dynamic-indexing quartet is not asked for either: the descriptor
     layout above reaches a texture with a descriptor index, which is core in the partially-bound
     set it declares, not one of these four.
   - sparseBinding and the whole sparseResidency* set: no sparse images yet.
   - multiDrawIndirect / drawIndirectFirstInstance / multiViewport / dualSrcBlend / logicOp /
     independentBlend / alphaToOne / largePoints / fillModeNonSolid / depthBounds / depthClamp /
     depthBiasClamp / imageCubeArray / fullDrawIndexUint32 / occlusionQueryPrecise /
     pipelineStatisticsQuery / vertexPipelineStoresAndAtomics / fragmentStoresAndAtomics /
     variableMultisampleRate / inheritedQueries: nothing in the design sketch needs any of them.
     sampleRateShading is on that list on purpose: whether the swapchain step asks for
     multisampling at all is S7's decision, and asking for shading-at-sample-rate before that
     decision is asking for a knob that may never be turned.

   One struct, one owner: the layers above read the answer this layer produces, they never
   re-decide what is wanted - the same rule xud_instance.c and xud_diagnostics.c follow for their
   extension lists. */
static const VkPhysicalDeviceFeatures XUD_PHYSICAL_WANTED_FEATURES = {
    .robustBufferAccess = VK_TRUE,
    .samplerAnisotropy  = VK_TRUE,
    .wideLines          = VK_TRUE
};

/* One field of the intersection: on only when the driver has it and this layer wants it, and the
   result is normalized to VK_TRUE/VK_FALSE so that no third value can reach vkCreateDevice. */
static VkBool32 xud_physical_and_feature(VkBool32 driverHasIt, VkBool32 layerWantsIt)
{
    if ((driverHasIt == VK_TRUE) && (layerWantsIt == VK_TRUE)) return VK_TRUE;
    return VK_FALSE;
}

/*-----------------------------------------------------------------------------------------------
 * The feature intersection S6 hands vkCreateDevice
 *-----------------------------------------------------------------------------------------------*/

/// See xud_physical.h for the contract. The wanted list is the file-scope const above, so this
/// function is the intersection and nothing else: it never turns a feature on that the driver
/// did not report, and never turns one off that both sides agreed on.
XudResult xud_physical_record_features(VkPhysicalDeviceFeatures *outEnabledFeatures, const XudDeviceCaps *caps)
{
    const VkPhysicalDeviceFeatures *has;
    const VkPhysicalDeviceFeatures *wants;
    VkPhysicalDeviceFeatures *out;

    if ((outEnabledFeatures == NULL) || (caps == NULL)) return XUD_INVALID_ARG;

    has   = &caps->features.features;          /* the driver's verbatim answer, the caps stay as said */
    wants = &XUD_PHYSICAL_WANTED_FEATURES;     /* this layer's decision, one owner above */
    out   = outEnabledFeatures;

    /* Written in struct order, every field named: see the wanted-list note above for why each
       field is wanted or not. */
    out->robustBufferAccess                          = xud_physical_and_feature(has->robustBufferAccess, wants->robustBufferAccess);
    out->fullDrawIndexUint32                         = xud_physical_and_feature(has->fullDrawIndexUint32, wants->fullDrawIndexUint32);
    out->imageCubeArray                              = xud_physical_and_feature(has->imageCubeArray, wants->imageCubeArray);
    out->independentBlend                            = xud_physical_and_feature(has->independentBlend, wants->independentBlend);
    out->geometryShader                              = xud_physical_and_feature(has->geometryShader, wants->geometryShader);
    out->tessellationShader                          = xud_physical_and_feature(has->tessellationShader, wants->tessellationShader);
    out->sampleRateShading                           = xud_physical_and_feature(has->sampleRateShading, wants->sampleRateShading);
    out->dualSrcBlend                                = xud_physical_and_feature(has->dualSrcBlend, wants->dualSrcBlend);
    out->logicOp                                     = xud_physical_and_feature(has->logicOp, wants->logicOp);
    out->multiDrawIndirect                           = xud_physical_and_feature(has->multiDrawIndirect, wants->multiDrawIndirect);
    out->drawIndirectFirstInstance                   = xud_physical_and_feature(has->drawIndirectFirstInstance, wants->drawIndirectFirstInstance);
    out->depthClamp                                  = xud_physical_and_feature(has->depthClamp, wants->depthClamp);
    out->depthBiasClamp                              = xud_physical_and_feature(has->depthBiasClamp, wants->depthBiasClamp);
    out->fillModeNonSolid                            = xud_physical_and_feature(has->fillModeNonSolid, wants->fillModeNonSolid);
    out->depthBounds                                 = xud_physical_and_feature(has->depthBounds, wants->depthBounds);
    out->wideLines                                   = xud_physical_and_feature(has->wideLines, wants->wideLines);
    out->largePoints                                 = xud_physical_and_feature(has->largePoints, wants->largePoints);
    out->alphaToOne                                  = xud_physical_and_feature(has->alphaToOne, wants->alphaToOne);
    out->multiViewport                               = xud_physical_and_feature(has->multiViewport, wants->multiViewport);
    out->samplerAnisotropy                           = xud_physical_and_feature(has->samplerAnisotropy, wants->samplerAnisotropy);
    out->textureCompressionETC2                      = xud_physical_and_feature(has->textureCompressionETC2, wants->textureCompressionETC2);
    out->textureCompressionASTC_LDR                  = xud_physical_and_feature(has->textureCompressionASTC_LDR, wants->textureCompressionASTC_LDR);
    out->textureCompressionBC                        = xud_physical_and_feature(has->textureCompressionBC, wants->textureCompressionBC);
    out->occlusionQueryPrecise                       = xud_physical_and_feature(has->occlusionQueryPrecise, wants->occlusionQueryPrecise);
    out->pipelineStatisticsQuery                     = xud_physical_and_feature(has->pipelineStatisticsQuery, wants->pipelineStatisticsQuery);
    out->vertexPipelineStoresAndAtomics              = xud_physical_and_feature(has->vertexPipelineStoresAndAtomics, wants->vertexPipelineStoresAndAtomics);
    out->fragmentStoresAndAtomics                    = xud_physical_and_feature(has->fragmentStoresAndAtomics, wants->fragmentStoresAndAtomics);
    out->shaderTessellationAndGeometryPointSize      = xud_physical_and_feature(has->shaderTessellationAndGeometryPointSize, wants->shaderTessellationAndGeometryPointSize);
    out->shaderImageGatherExtended                   = xud_physical_and_feature(has->shaderImageGatherExtended, wants->shaderImageGatherExtended);
    out->shaderStorageImageExtendedFormats           = xud_physical_and_feature(has->shaderStorageImageExtendedFormats, wants->shaderStorageImageExtendedFormats);
    out->shaderStorageImageMultisample               = xud_physical_and_feature(has->shaderStorageImageMultisample, wants->shaderStorageImageMultisample);
    out->shaderStorageImageReadWithoutFormat         = xud_physical_and_feature(has->shaderStorageImageReadWithoutFormat, wants->shaderStorageImageReadWithoutFormat);
    out->shaderStorageImageWriteWithoutFormat        = xud_physical_and_feature(has->shaderStorageImageWriteWithoutFormat, wants->shaderStorageImageWriteWithoutFormat);
    out->shaderUniformBufferArrayDynamicIndexing     = xud_physical_and_feature(has->shaderUniformBufferArrayDynamicIndexing, wants->shaderUniformBufferArrayDynamicIndexing);
    out->shaderSampledImageArrayDynamicIndexing      = xud_physical_and_feature(has->shaderSampledImageArrayDynamicIndexing, wants->shaderSampledImageArrayDynamicIndexing);
    out->shaderStorageBufferArrayDynamicIndexing     = xud_physical_and_feature(has->shaderStorageBufferArrayDynamicIndexing, wants->shaderStorageBufferArrayDynamicIndexing);
    out->shaderStorageImageArrayDynamicIndexing      = xud_physical_and_feature(has->shaderStorageImageArrayDynamicIndexing, wants->shaderStorageImageArrayDynamicIndexing);
    out->shaderClipDistance                          = xud_physical_and_feature(has->shaderClipDistance, wants->shaderClipDistance);
    out->shaderCullDistance                          = xud_physical_and_feature(has->shaderCullDistance, wants->shaderCullDistance);
    out->shaderFloat64                               = xud_physical_and_feature(has->shaderFloat64, wants->shaderFloat64);
    out->shaderInt64                                 = xud_physical_and_feature(has->shaderInt64, wants->shaderInt64);
    out->shaderInt16                                 = xud_physical_and_feature(has->shaderInt16, wants->shaderInt16);
    out->shaderResourceResidency                     = xud_physical_and_feature(has->shaderResourceResidency, wants->shaderResourceResidency);
    out->shaderResourceMinLod                        = xud_physical_and_feature(has->shaderResourceMinLod, wants->shaderResourceMinLod);
    out->sparseBinding                               = xud_physical_and_feature(has->sparseBinding, wants->sparseBinding);
    out->sparseResidencyBuffer                       = xud_physical_and_feature(has->sparseResidencyBuffer, wants->sparseResidencyBuffer);
    out->sparseResidencyImage2D                      = xud_physical_and_feature(has->sparseResidencyImage2D, wants->sparseResidencyImage2D);
    out->sparseResidencyImage3D                      = xud_physical_and_feature(has->sparseResidencyImage3D, wants->sparseResidencyImage3D);
    out->sparseResidency2Samples                     = xud_physical_and_feature(has->sparseResidency2Samples, wants->sparseResidency2Samples);
    out->sparseResidency4Samples                     = xud_physical_and_feature(has->sparseResidency4Samples, wants->sparseResidency4Samples);
    out->sparseResidency8Samples                     = xud_physical_and_feature(has->sparseResidency8Samples, wants->sparseResidency8Samples);
    out->sparseResidency16Samples                    = xud_physical_and_feature(has->sparseResidency16Samples, wants->sparseResidency16Samples);
    out->sparseResidencyAliased                      = xud_physical_and_feature(has->sparseResidencyAliased, wants->sparseResidencyAliased);
    out->variableMultisampleRate                     = xud_physical_and_feature(has->variableMultisampleRate, wants->variableMultisampleRate);
    out->inheritedQueries                            = xud_physical_and_feature(has->inheritedQueries, wants->inheritedQueries);

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The device policy: which enumerated handle this context uses
 *-----------------------------------------------------------------------------------------------*/

/// See xud_physical.h for the contract. The policy is "first in driver order" today, and
/// `properties` is part of the signature so a ranking can be added without touching a call
/// site - it is checked for being where it claims to be (a NULL array with a non-zero count is
/// a broken argument) and its contents are deliberately not consulted, because the only thing a
/// discrete-preference policy could rank on here is core deviceType plus vendorID/deviceName/
/// pipelineCacheUUID: VkPhysicalDeviceArchitectureInfo, which names the microarchitecture a
/// discrete GPU actually is, is not in these headers at all (grepped: zero hits). Ranking is the
/// plan's "policy later", not something this layer invents - see xud_physical.h.
XudResult xud_physical_select(uint32_t *outIndex, const VkPhysicalDevice *candidates, const VkPhysicalDeviceProperties2 *properties, uint32_t candidateCount)
{
    if (outIndex == NULL) return XUD_INVALID_ARG;

    /* Both arrays are claimed to hold candidateCount rows, so with a non-zero count a NULL one
       is a broken argument; with a zero count neither is read at all. */
    if ((candidates == NULL) && (candidateCount != 0u)) return XUD_INVALID_ARG;
    if ((properties == NULL) && (candidateCount != 0u)) return XUD_INVALID_ARG;

    /* No device at all: the same answer xud_vulkan_supported() gives when there is no driver,
       and the reason xud_physical_open() fails rather than record a NULL handle. */
    if (candidateCount == 0u) return XUD_UNSUPPORTED;

    *outIndex = 0u;   /* first in driver order, the whole policy today */

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The one queue family the rest of the library will use
 *-----------------------------------------------------------------------------------------------*/

/// See xud_physical.h for the contract. The rule is VK_QUEUE_GRAPHICS_BIT, first family in
/// driver order wins; transfer/compute/granularity are recorded, not required, and presentation
/// is not a flag at all in these headers (no VK_QUEUE_PRESENT_BIT - grepped), so S7's GLFW check
/// is what refuses a surface on a family that cannot present.
XudResult xud_physical_select_queue_family(uint32_t *outIndex, VkQueueFamilyProperties2 *outChosen, const VkQueueFamilyProperties2 *families, uint32_t familyCount)
{
    uint32_t i;

    if ((outIndex == NULL) || (outChosen == NULL)) return XUD_INVALID_ARG;
    if ((families == NULL) && (familyCount != 0u)) return XUD_INVALID_ARG;

    /* No family at all, or none with the graphics bit: a compute-only or transfer-only driver.
       The sentinel is written so the caller cannot mistake an untouched index for family 0, and
       the chosen row is zeroed on this path too - a stale row in the caps would be a row the
       driver never said. */
    if (familyCount == 0u)
    {
        *outIndex = XUD_PHYSICAL_NO_QUEUE_FAMILY;
        memset(outChosen, 0, sizeof(*outChosen));
        return XUD_UNSUPPORTED;
    }

    for (i = 0u; i < familyCount; i++)
    {
        if ((families[i].queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT) != VK_QUEUE_GRAPHICS_BIT) continue;

        *outIndex = i;
        *outChosen = families[i];   /* the whole row: sType, pNext, queueFlags, queueCount, timestampValidBits, minImageTransferGranularity */

        /* The row now lives in the caps, not in the caller's temporary array: sType is this
           layer's claim about a struct it assembled, and pNext must not be a pointer into an
           array that xud_physical_open() frees right after choosing. Nothing is chained onto a
           queue-family query here, so the caps' row says so. */
        outChosen->sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        outChosen->pNext = NULL;

        return XUD_OK;
    }

    *outIndex = XUD_PHYSICAL_NO_QUEUE_FAMILY;
    memset(outChosen, 0, sizeof(*outChosen));

    return XUD_UNSUPPORTED;
}

/*-----------------------------------------------------------------------------------------------
 * The apiVersion read-back
 *-----------------------------------------------------------------------------------------------*/

/// See xud_physical.h for the contract. The driver's number is recorded verbatim first, then
/// compared with the floor the instance layer asked for: the caps are a record of what the
/// driver said, and the answer is whether this layer can drive it. A driver above what these
/// headers know (VK_API_VERSION_1_4 is the newest core here) is recorded as said too - the
/// layers above gate on the caps, they cannot use a core version their headers do not declare.
XudResult xud_physical_record_api_version(XudDeviceCaps *caps)
{
    if (caps == NULL) return XUD_INVALID_ARG;

    /* The core field inside the wrapper (xud_physical.h's naming trap: the fields read out of a
       *2 struct are the core ones). */
    caps->apiVersion = caps->properties.properties.apiVersion;

    /* A driver that answered nothing leaves this 0 (xud_physical_open() zeroes the caps before
       the query that fills it, and the four getters that return void give no other failure
       signal) - 0 is below the floor, so "the driver said nothing" fails here rather than
       looking like a usable device. */
    if (caps->apiVersion < XUD_INSTANCE_API_FLOOR) return XUD_UNSUPPORTED;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * Teardown: the free-only rung of the ladder
 *-----------------------------------------------------------------------------------------------*/

/// See xud_physical.h for the contract: Vulkan has no vkDestroyPhysicalDevice, a VkPhysicalDevice
/// is a query result, so this frees the caps' one heap array and the struct, and calls no driver
/// function at all - which is why it takes no loader.
XudResult xud_physical_close(XudPhysicalDevice **physicalDevice)
{
    XudPhysicalDevice *victim;

    /* Arguments before state, the same rule xud_diagnostics_close() follows. */
    if (physicalDevice == NULL) return XUD_INVALID_ARG;
    if (*physicalDevice == NULL) return XUD_ALREADY;

    victim = *physicalDevice;

    /* The one heap thing the caps own: the advertised device-extension array, NULL when the
       query never ran. The count/capacity are cleared with it so a freed struct cannot look
       like it still describes rows that are gone. */
    if (victim->caps.deviceExtensionProperties != NULL)
    {
        free(victim->caps.deviceExtensionProperties);
        victim->caps.deviceExtensionProperties = NULL;
    }
    victim->caps.deviceExtensionCount = 0u;
    victim->caps.deviceExtensionCapacity = 0u;

    free(victim);
    *physicalDevice = NULL;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The driver half: the three doubling-capacity queries, then the bring-up that runs them
 *-----------------------------------------------------------------------------------------------*/

/* Internal: the advertised device-extension list, into the caps' one heap array. vkEnumerateDev
   iceExtensionProperties has no count-only form (pCount is input/output, the typedef at vulkan_c
   ore.h:4544), so this is the same doubling shape as xud_instance_query_extensions in xud_instan
   ce.c and xud_diagnostics_query_layers in xud_diagnostics.c - verified there. pLayerName is NUL
   L: the driver's own list, not a layer's, which is what VkDeviceCreateInfo's ppEnabledExtension
   Names is intersected with at S6. The capacity is an out-param because the caps record it next
   to the count: the array is a step of the sequence above, not a tight fit, and xud_physical_clo
   se() frees exactly that block. */
static XudResult xud_physical_query_device_extensions(const XudLoader *loader, VkPhysicalDevice physicalDevice, VkExtensionProperties **outProperties, uint32_t *outCount, uint32_t *outCapacity)
{
    VkExtensionProperties *items = NULL;
    uint32_t capacity = XUD_PHYSICAL_QUERY_MIN_CAPACITY;
    uint32_t count = 0u;
    VkResult vk;

    if ((loader == NULL) || (outProperties == NULL) || (outCount == NULL) || (outCapacity == NULL))
        return XUD_INVALID_ARG;
    if ((physicalDevice == VK_NULL_HANDLE) || (!xud_loader_is_open(loader)) || (loader->enumerateDeviceExtensionProperties == NULL))
        return XUD_UNSUPPORTED;

    *outProperties = NULL;
    *outCount = 0u;
    *outCapacity = 0u;

    for (;;)
    {
        items = calloc(capacity, sizeof(VkExtensionProperties));
        if (items == NULL)
            return XUD_OUT_OF_MEMORY;

        count = capacity;
        vk = loader->enumerateDeviceExtensionProperties(physicalDevice, NULL, &count, items);

        if ((vk == VK_SUCCESS) && (count <= capacity))
        {
            *outProperties = items;
            *outCount = count;
            *outCapacity = capacity;
            return XUD_OK;
        }

        free(items);

        if ((vk != VK_INCOMPLETE) && ((vk != VK_SUCCESS) || (count <= capacity)))
            return xud_result_from_vk(vk);
        if (capacity >= XUD_PHYSICAL_QUERY_MAX_CAPACITY)
            return XUD_VULKAN_ERROR;
        capacity *= 2u;
    }
}

/* Internal: the queue families, as the driver rows them. vkGetPhysicalDeviceQueueFamilyPropert
   ies2 returns void in these headers (the typedef at vulkan_core.h:6182) and has an input/outpu
   t count, so the doubling shape is the only way to size the array and the only failure signal t
   he call can give is "it wrote nothing": a zero count comes back as XUD_UNSUPPORTED, which is w
   hat xud_physical_select_queue_family() answers for an empty list too - the sentinel plus a zer
   oed row, never a row still carrying a family this layer did not check. A count above the capaci
   ty in hand is a driver that wrote past the rows this layer provided: reported, not chased. */
static XudResult xud_physical_query_queue_families(const XudLoader *loader, VkPhysicalDevice physicalDevice, VkQueueFamilyProperties2 **outFamilies, uint32_t *outCount)
{
    VkQueueFamilyProperties2 *items = NULL;
    uint32_t capacity = XUD_PHYSICAL_QUERY_MIN_CAPACITY;
    uint32_t count = 0u;
    uint32_t row = 0u;

    if ((loader == NULL) || (outFamilies == NULL) || (outCount == NULL))
        return XUD_INVALID_ARG;
    if ((physicalDevice == VK_NULL_HANDLE) || (!xud_loader_is_open(loader)) || (loader->getPhysicalDeviceQueueFamilyProperties2 == NULL))
        return XUD_UNSUPPORTED;

    *outFamilies = NULL;
    *outCount = 0u;

    for (;;)
    {
        items = calloc(capacity, sizeof(VkQueueFamilyProperties2));
        if (items == NULL)
            return XUD_OUT_OF_MEMORY;

        /* Every row is stamped before the call, the whole capacity of it: the validation laye
           r checks the rows it is handed, not only the ones the driver writes, and an unstamped
           wrapper claims nothing. Probed here: without this stamp the validation layer answers
           "pQueueFamilyProperties[i].sType must be VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2"
           for every row up to the duplicate-message limit, which is exactly the signal AGENTS.m
           d says validation output is (a debug GPU run must be validation-clean, not just crash
           -free).
         */
        for (row = 0u; row < capacity; row++)
        {
            items[row].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
            items[row].pNext = NULL;
        }

        count = capacity;
        loader->getPhysicalDeviceQueueFamilyProperties2(physicalDevice, &count, items);

        if ((count > 0u) && (count <= capacity))
        {
            *outFamilies = items;
            *outCount = count;
            return XUD_OK;
        }

        free(items);

        if (count > capacity)
            return XUD_VULKAN_ERROR;
        if (capacity >= XUD_PHYSICAL_QUERY_MAX_CAPACITY)
            return XUD_UNSUPPORTED;
        capacity *= 2u;
    }
}

/* Public, see xud_physical.h: the device list. vkEnumeratePhysicalDevices is one of the two ph
   ysical names that do answer with a VkResult (the typedef at vulkan_core.h:4532), so a refusal
   maps through xud_result_from_vk; VK_INCOMPLETE means the list was cut at the capacity in hand,
   which is not an error in Vulkan but is here - a device this layer did not see is a device it c
   annot rank, so the doubling continues and a list cut at the cap fails. */
XudResult xud_physical_query_devices(const XudLoader *loader, VkPhysicalDevice **outCandidates, uint32_t *outCount, VkInstance instance)
{
    VkPhysicalDevice *items = NULL;
    uint32_t capacity = XUD_PHYSICAL_QUERY_MIN_CAPACITY;
    uint32_t count = 0u;
    VkResult vk;

    if ((loader == NULL) || (outCandidates == NULL) || (outCount == NULL))
        return XUD_INVALID_ARG;
    if ((instance == VK_NULL_HANDLE) || (!xud_loader_is_open(loader)) || (loader->enumeratePhysicalDevices == NULL))
        return XUD_UNSUPPORTED;

    *outCandidates = NULL;
    *outCount = 0u;

    for (;;)
    {
        items = calloc(capacity, sizeof(VkPhysicalDevice));
        if (items == NULL)
            return XUD_OUT_OF_MEMORY;

        count = capacity;
        vk = loader->enumeratePhysicalDevices(instance, &count, items);

        if ((vk == VK_SUCCESS) && (count <= capacity))
        {
            *outCandidates = items;
            *outCount = count;
            return XUD_OK;
        }

        free(items);

        if ((vk != VK_INCOMPLETE) && ((vk != VK_SUCCESS) || (count <= capacity)))
            return xud_result_from_vk(vk);
        if (capacity >= XUD_PHYSICAL_QUERY_MAX_CAPACITY)
            return XUD_VULKAN_ERROR;
        capacity *= 2u;
    }
}

/*-----------------------------------------------------------------------------------------------
 * Bring-up: the whole layer in one call, and the one place this file calls the driver
 *-----------------------------------------------------------------------------------------------*/

/* Public, see xud_physical.h. The order is the layer's contract: bind the six names this layer
   calls, enumerate, rank, then record the chosen device's caps. The caps are zeroed as part of
   the one allocation because the four caps getters return void - a field the driver did not wri
   te has to be at a valid zero state, never at uninitialised memory (see xud_physical.h). */
XudResult xud_physical_open(XudPhysicalDevice **outPhysicalDevice, XudLoader *loader, VkInstance instance)
{
    XudPhysicalDevice *physicalDevice = NULL;
    XudDeviceCaps *caps = NULL;
    VkPhysicalDevice *candidates = NULL;
    VkPhysicalDeviceProperties2 *properties = NULL;
    VkQueueFamilyProperties2 *families = NULL;
    VkExtensionProperties *extensions = NULL;
    XudResult result = XUD_INVALID_ARG;
    uint32_t candidateCount = 0u;
    uint32_t familyCount = 0u;
    uint32_t extensionCount = 0u;
    uint32_t extensionCapacity = 0u;
    uint32_t chosenIndex = 0u;
    uint32_t familyIndex = XUD_PHYSICAL_NO_QUEUE_FAMILY;
    uint32_t candidate = 0u;

    if ((outPhysicalDevice == NULL) || (*outPhysicalDevice != NULL) || (loader == NULL) || (instance == VK_NULL_HANDLE))
        return XUD_INVALID_ARG;
    if (!xud_loader_is_open(loader))
        return XUD_UNSUPPORTED;

    physicalDevice = calloc(1u, sizeof(XudPhysicalDevice));
    if (physicalDevice == NULL)
        return XUD_OUT_OF_MEMORY;
    caps = &physicalDevice->caps;

    /* The zero state the void getters need: the handle is NULL until the driver answers, and t
       he queue-family slot is the sentinel xud_physical.h states, so a caps that got no family s
       ays "no family" instead of "family zero, all zeroes". */
    physicalDevice->physicalDevice = VK_NULL_HANDLE;
    caps->queueFamilyIndex = XUD_PHYSICAL_NO_QUEUE_FAMILY;

    /* The six names this layer calls, resolved through the live instance and cached in the tab
       le: they name the driver's own code, not a layer instantiated per instance, so a slot sta
       ys valid across an instance recreation (xud_loader.h). A name the loader does not know ans
       wers NULL and xud_loader_bind reports XUD_UNSUPPORTED - never called through. */
    result = xud_loader_bind(loader, instance, "vkEnumeratePhysicalDevices", &loader->enumeratePhysicalDevices);
    if (result != XUD_OK)
        goto fail;
    result = xud_loader_bind(loader, instance, "vkGetPhysicalDeviceProperties2", &loader->getPhysicalDeviceProperties2);
    if (result != XUD_OK)
        goto fail;
    result = xud_loader_bind(loader, instance, "vkGetPhysicalDeviceFeatures2", &loader->getPhysicalDeviceFeatures2);
    if (result != XUD_OK)
        goto fail;
    result = xud_loader_bind(loader, instance, "vkGetPhysicalDeviceMemoryProperties2", &loader->getPhysicalDeviceMemoryProperties2);
    if (result != XUD_OK)
        goto fail;
    result = xud_loader_bind(loader, instance, "vkGetPhysicalDeviceQueueFamilyProperties2", &loader->getPhysicalDeviceQueueFamilyProperties2);
    if (result != XUD_OK)
        goto fail;
    result = xud_loader_bind(loader, instance, "vkEnumerateDeviceExtensionProperties", &loader->enumerateDeviceExtensionProperties);
    if (result != XUD_OK)
        goto fail;

    /* The nil-slot rule xud_physical.h states, for the three caps getters that return void and
       therefore have no query helper of their own to check it (the three queries above check th
       eir own slot before calling). xud_loader_bind filled the slot on XUD_OK; this is the check
       the contract promises, not a second opinion about the bind. */
    if ((loader->getPhysicalDeviceProperties2 == NULL) || (loader->getPhysicalDeviceFeatures2 == NULL) || (loader->getPhysicalDeviceMemoryProperties2 == NULL))
    {
        result = XUD_UNSUPPORTED;
        goto fail;
    }

    result = xud_physical_query_devices(loader, &candidates, &candidateCount, instance);
    if (result != XUD_OK)
        goto fail;
    if (candidateCount == 0u)
    {
        result = XUD_UNSUPPORTED; /* the driver enumerated no device at all: nothing to rank, nothing to record */
        goto fail;
    }

    properties = calloc(candidateCount, sizeof(VkPhysicalDeviceProperties2));
    if (properties == NULL)
    {
        result = XUD_OUT_OF_MEMORY;
        goto fail;
    }

    /* One row per candidate, then the pure policy picks. The wrapper's sType is stamped before
       the call because lavvk leaves the wrapper alone (probed here: sType 0 in, 0 out) and an u
       nstamped row would claim nothing; pNext stays NULL because this layer chains nothing. */
    for (candidate = 0u; candidate < candidateCount; candidate++)
    {
        properties[candidate].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties[candidate].pNext = NULL;
        loader->getPhysicalDeviceProperties2(candidates[candidate], &properties[candidate]);
    }

    result = xud_physical_select(&chosenIndex, candidates, properties, candidateCount);
    if (result != XUD_OK)
        goto fail;

    physicalDevice->physicalDevice = candidates[chosenIndex];
    free(candidates);
    candidates = NULL; /* the handle is a value: the array was a query result, this layer owns it and its job is done */

    caps->properties = properties[chosenIndex];
    free(properties);
    properties = NULL; /* only the chosen row is kept; the other candidates were query results, and a VkPhysicalDevice needs no release */

    caps->features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    caps->features.pNext = NULL;
    loader->getPhysicalDeviceFeatures2(physicalDevice->physicalDevice, &caps->features);

    caps->memoryProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    caps->memoryProperties.pNext = NULL;
    loader->getPhysicalDeviceMemoryProperties2(physicalDevice->physicalDevice, &caps->memoryProperties);

    /* The chain rule, checked not assumed: this layer provided no chained struct for any of the
       three *2 queries, so a driver that wrote one into pNext is a driver whose caps this layer
       cannot hand on - the caps' pNext is never walked by xud_physical_close(), and a struct the
       layer does not own is not something a later layer may read. Probed here (lavvk): NULL in,
       NULL out for all three. */
    if ((caps->properties.pNext != NULL) || (caps->features.pNext != NULL) || (caps->memoryProperties.pNext != NULL))
    {
        result = XUD_VULKAN_ERROR;
        goto fail;
    }

    result = xud_physical_query_device_extensions(loader, physicalDevice->physicalDevice, &extensions, &extensionCount, &extensionCapacity);
    if (result != XUD_OK)
        goto fail;

    caps->deviceExtensionProperties = extensions;
    caps->deviceExtensionCount = extensionCount;
    caps->deviceExtensionCapacity = extensionCapacity;
    extensions = NULL; /* ownership moved into the caps: xud_physical_close() frees that block, the fail ladder below must not free it twice */

    result = xud_physical_query_queue_families(loader, physicalDevice->physicalDevice, &families, &familyCount);
    if (result != XUD_OK)
        goto fail;

    result = xud_physical_select_queue_family(&familyIndex, &caps->chosenQueueFamilyProperties, families, familyCount);
    if (result != XUD_OK)
        goto fail; /* XUD_UNSUPPORTED: no family here carries the graphics bit; the sentinel and a zeroed row are already in the caps, and this struct is about to be freed */

    caps->queueFamilyCount = familyCount;
    caps->queueFamilyIndex = familyIndex;
    free(families);
    families = NULL; /* the rows were a query result; the chosen one was copied into the caps */

    result = xud_physical_record_api_version(caps);
    if (result != XUD_OK)
        goto fail; /* XUD_UNSUPPORTED: below the instance floor, or the driver answered nothing at all and apiVersion is still 0 */

    *outPhysicalDevice = physicalDevice;
    return XUD_OK;

fail:
    /* Reverse creation order, and nothing destroyed: this layer's only Vulkan object is a query
       result, vkDestroyPhysicalDevice does not exist in these headers, and the instance the hand
       le came from belongs to the layer above, which is the one that destroys it. */
    if (families != NULL)
        free(families);
    if (extensions != NULL)
        free(extensions); /* still a local: the move into the caps had not happened yet */
    if (properties != NULL)
        free(properties);
    if (candidates != NULL)
        free(candidates);
    if (physicalDevice != NULL)
    {
        if (caps->deviceExtensionProperties != NULL)
        {
            free(caps->deviceExtensionProperties);
            caps->deviceExtensionProperties = NULL;
        }
        caps->deviceExtensionCount = 0u;
        caps->deviceExtensionCapacity = 0u;
        free(physicalDevice);
    }
    *outPhysicalDevice = NULL;
    return (result != XUD_OK) ? result : XUD_INTERNAL_ERROR;
}
