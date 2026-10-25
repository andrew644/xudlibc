/***********************************************************************************************
 *
 *   xud_shader.c - the shader layer: SPIR-V words to a VkShaderModule and back
 *
 *   Owns : the shader layer. Checks the SPIR-V header before the driver sees it, assembles
 *          the one create-info, creates the module, and tears it back down.
 *   Layer: above the device, below the pipeline. Nothing in this file calls a Vulkan entry
 *          point directly - every one is reached through a row of XudLoader's two Volk tables
 *          (vkCreateShaderModule / vkDestroyShaderModule are device-level rows of struct
 *          VolkDeviceTable, filled in bulk by xud_loader_bind_device(), so the archive holds
 *          no vk* symbol and the link line has no -lvulkan anywhere, AGENTS.md).
 *          The SPIR-V header rule this layer enforces is the spec's own form, not the exact
 *          value: probed today against the module glslangValidator 11:16.6.0 writes here
 *          (shaders/solid.vert, 354 words), word 0 is 0x07230203 and spirv-val rejects every
 *          other low byte of it (0x02, 0xff, 0x00 all answer "Invalid SPIR-V magic number"),
 *          so the tool's rule is one exact magic while the spec's is the fixed 24 bits plus a
 *          version byte in 0x00..0x0B. Accepting only 1.3's value would refuse a 1.0 or a 1.5
 *          module that both the spec and the driver take, so the check below is the spec's.
 *          Word 4 (the instruction count) is deprecated and is NOT checked: spirv-val accepts
 *          5 written into it (probed), so a check here would be stricter than anything that
 *          calls this layer.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud_shader.h"
#include "xud.h"
#include "xud_device.h"
#include "xud_loader.h"
#include "xud_vulkan.h"

#include <assert.h> /* Required for: static_assert, the row-size checks */
#include <stdlib.h> /* Required for: calloc, free */

/* The rows this layer calls through must be pointer-sized, the same rule xud_loader.h checks
   for the table pair as a whole. */
static_assert(sizeof(PFN_vkCreateShaderModule) == sizeof(void*), "PFN row must be pointer-sized");
static_assert(sizeof(PFN_vkDestroyShaderModule) == sizeof(void*), "PFN row must be pointer-sized");

/* The create-info's pCode points at the caller's words, not at a copy: VkShaderModuleCreateInfo
   is read by the driver during the call and the record of it (XudShader::code) is the same
   pointer, so the two cannot disagree about length. Checked rather than trusted: the words are
   uint32_t in both places, so a size in bytes that is not a whole number of words is refused
   before the create-info is written. */
static_assert(sizeof(((XudShader*)0)->codeSize) == sizeof(size_t), "the byte count is a size_t");

/*-----------------------------------------------------------------------------------------------
 * The SPIR-V header check - pure, no driver call
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_shader_check_spv(const uint32_t* code, size_t codeSize) {
    if ((code == NULL) || (codeSize == 0u)) {
        return XUD_INVALID_ARG;
    }

    /* The words are 32-bit, so a byte count that is not a multiple of 4 is not a module. */
    if ((codeSize % sizeof(uint32_t)) != 0u) {
        return XUD_UNSUPPORTED;
    }
    if (codeSize < (XUD_SHADER_SPIRV_HEADER_WORDS * sizeof(uint32_t))) {
        return XUD_UNSUPPORTED;
    }

    /* The magic's fixed 24 bits, with the version byte below them left out - see the note above
       about why this is not a compare against one exact value. */
    if ((code[0] & 0xFFFFFF00u) != XUD_SHADER_SPIRV_MAGIC_FIXED) {
        return XUD_UNSUPPORTED;
    }
    if ((code[0] & 0xFFu) > XUD_SHADER_SPIRV_VERSION_MAX) {
        return XUD_UNSUPPORTED;
    }

    /* Word 1 is (major << 16) | (minor << 8): these headers take SPIR-V 1.x only, and a module
       of another major version is not a module a driver here would accept. */
    if ((code[1] >> 16) != XUD_SHADER_SPIRV_VERSION_MAJOR) {
        return XUD_UNSUPPORTED;
    }

    /* Word 3 is the id upper bound. Ids start at 1, so a bound below 2 declares no id at all,
       which means no entry point and no pipeline that could be built from it. */
    if (code[3] < 2u) {
        return XUD_UNSUPPORTED;
    }

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The create-info assembly - pure, no driver call
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_shader_fill_create_info(VkShaderModuleCreateInfo* outCreateInfo, const uint32_t* code, size_t codeSize) {
    XudResult checked = XUD_UNSUPPORTED;

    if (outCreateInfo == NULL) {
        return XUD_INVALID_ARG;
    }

    checked = xud_shader_check_spv(code, codeSize);
    if (checked != XUD_OK) {
        return checked;
    }

    outCreateInfo->sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    outCreateInfo->pNext = NULL; /* this layer chains nothing into the create-info */
    outCreateInfo->flags = 0u; /* VkShaderModuleCreateFlags has no bit this library takes */
    outCreateInfo->codeSize = codeSize;
    outCreateInfo->pCode = code;

    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The create rung - the driver half
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_shader_open(XudShader** outShader, XudLoader* loader, const XudDevice* device, const uint32_t* code, size_t codeSize) {
    XudShader* shader = NULL;
    VkShaderModuleCreateInfo createInfo;
    VkShaderModule created = VK_NULL_HANDLE;
    XudResult result = XUD_INVALID_ARG;
    VkResult vk = VK_ERROR_UNKNOWN;

    if ((outShader == NULL) || (*outShader != NULL)) {
        return XUD_INVALID_ARG;
    }
    if ((loader == NULL) || (device == NULL) || (!loader->open)) {
        return (loader == NULL) ? XUD_INVALID_ARG : XUD_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return XUD_INVALID_ARG;
    }

    /* The destroy row before the create row: a module this layer cannot destroy is a module it
       must not create, the same rule xud_window.c applies to a surface and xud_swapchain.c to a
       swapchain. */
    if ((loader->deviceTable.vkDestroyShaderModule == NULL) || (loader->deviceTable.vkCreateShaderModule == NULL)) {
        return XUD_UNSUPPORTED;
    }

    /* The rows must be THIS device's rows - a module created through another device's rows is a
       module this device cannot destroy. */
    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }

    shader = calloc(1u, sizeof(*shader));
    if (shader == NULL) {
        return XUD_OUT_OF_MEMORY;
    }

    result = xud_shader_fill_create_info(&createInfo, code, codeSize);
    if (result != XUD_OK) {
        free(shader);
        return result;
    }

    vk = loader->deviceTable.vkCreateShaderModule(device->device, &createInfo, NULL, &created);
    result = xud_result_from_vk(vk);
    if (result != XUD_OK) {
        free(shader);
        return result;
    }

    shader->module = created;
    shader->code = code; /* borrowed, never freed here - see the note in the header above */
    shader->codeSize = codeSize;

    *outShader = shader;
    return XUD_OK;
}

/*-----------------------------------------------------------------------------------------------
 * The teardown rung - the driver half
 *-----------------------------------------------------------------------------------------------*/

XudResult xud_shader_close(XudShader** shader, const XudDevice* device, const XudLoader* loader) {
    XudShader* victim = NULL;
    VkShaderModule handle = VK_NULL_HANDLE;

    if ((shader == NULL) || (*shader == NULL)) {
        return XUD_INVALID_ARG;
    }
    victim = *shader;

    handle = victim->module;
    if (handle == VK_NULL_HANDLE) {
        /* Already torn down by a rung that destroyed it without freeing, or a struct that never
           got a module. Freeing is what this rung is for, so it still runs. */
        free(victim);
        *shader = NULL;
        return XUD_ALREADY;
    }

    if ((loader == NULL) || (device == NULL)) {
        return (loader == NULL) ? XUD_INVALID_ARG : XUD_UNSUPPORTED;
    }
    if ((!loader->open) || (device->device == VK_NULL_HANDLE)) {
        return XUD_UNSUPPORTED;
    }

    if (!xud_loader_device_bound(loader, device->device)) {
        return XUD_UNSUPPORTED;
    }
    if (loader->deviceTable.vkDestroyShaderModule == NULL) {
        return XUD_UNSUPPORTED;
    }

    /* vkDestroyShaderModule returns void, so there is no VkResult to map: the handle is cleared
       before the call, which is the zero-state-before-the-call rule this tree follows for a void
       teardown (vkDestroyDevice, vkDestroySurfaceKHR, vkFreeMemory). */
    victim->module = VK_NULL_HANDLE;
    loader->deviceTable.vkDestroyShaderModule(device->device, handle, NULL);

    free(victim);
    *shader = NULL;
    return XUD_OK;
}
