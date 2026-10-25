/***********************************************************************************************
 *   xud_shader.h - the shader rung: SPIR-V words to a VkShaderModule and back
 *
 *   Layer: above the device, below the pipeline. The one Vulkan object of this rung is a
 *   VkShaderModule, and the SPIR-V it was made from is borrowed, not copied: the words live
 *   in the built-in set CMakeLists.txt generates from shaders/ (glslangValidator at build
 *   time, spirv-val in the same build step, never a runtime GLSL compile - the repo's rule),
 *   and a module this library cannot destroy is a module it must not create.
 *
 *   SPIR-V header facts, probed today against the module glslangValidator 11:16.6.0 writes
 *   here (shaders/solid.vert, 354 words): word 0 is 0x07230203, word 1 is 0x00010000, word
 *   2 is 0x0008000b (the generator word, Khronos glslang), word 3 is 52 (the id upper bound)
 *   and word 4 is 0. The low byte of word 0 is the SPIR-V version: spirv-val rejects every
 *   other low byte for this module (probed - 0x02, 0xff, 0x00 all answer "Invalid SPIR-V
 *   magic number"), so the tool's rule is the exact value, while the spec's rule is the
 *   fixed 24 bits plus a version byte in 0x00..0x0B. The check below is the spec's form,
 *   because a check that only accepted 1.3's magic would refuse a 1.0 or a 1.5 module that
 *   both the spec and the driver accept. Word 4 (the instruction count) is deprecated and
 *   not checked: spirv-val accepts 5 written into it (probed), so a check here would be
 *   stricter than anything that calls this layer.
 ***********************************************************************************************/

#ifndef XUD_SHADER_H_
#define XUD_SHADER_H_

#include "volk.h"
#include "xud_device.h"
#include "xud_loader.h"
#include "xud_vulkan.h"
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

/*-----------------------------------------------------------------------------------------------
 * The one object of this rung
 *-----------------------------------------------------------------------------------------------*/

typedef struct XudShader XudShader;
struct XudShader {
    VkShaderModule module; /* the one Vulkan object this layer owns: VK_NULL_HANDLE until
                              xud_shader_open() creates it, cleared and destroyed by
                              xud_shader_close() */
    const uint32_t* code; /* borrowed: the SPIR-V words vkCreateShaderModule was handed. Not
                             owned and never freed here - the built-in set is static storage
                             in the generated header, and a caller that brings its own SPIR-V
                             up owns that memory, this layer only records what it was given */
    size_t codeSize; /* bytes of it, exactly as VkShaderModuleCreateInfo::codeSize was handed
                        it, so the record and the create-info cannot disagree about length */
};

/*-----------------------------------------------------------------------------------------------
 * The SPIR-V header, as this layer reads it
 *-----------------------------------------------------------------------------------------------*/

#define XUD_SHADER_SPIRV_MAGIC_FIXED                                                                                                       \
    0x07230200u /* the magic's fixed 24 bits, without the                                                                                  \
                   version byte in the low one */
#define XUD_SHADER_SPIRV_VERSION_MAX                                                                                                       \
    0x0Bu /* the highest SPIR-V version byte the spec defines                                                                              \
             (1.0 through 1.11) */
#define XUD_SHADER_SPIRV_HEADER_WORDS                                                                                                      \
    5u /* the header is 5 words: magic, version, generator,                                                                                \
          bound, instruction count */
#define XUD_SHADER_SPIRV_VERSION_MAJOR 1u /* SPIR-V 1.x, the only family these headers take */

static_assert((XUD_SHADER_SPIRV_MAGIC_FIXED & 0xFFu) == 0u,
    "the fixed mask leaves the version byte out, which is what makes the check a "
    "compare of the fixed 24 bits rather than of one exact magic value");

/*-----------------------------------------------------------------------------------------------
 * The layer's contract
 *-----------------------------------------------------------------------------------------------*/

/// Pure: the SPIR-V header check every create in this rung runs before it calls the driver.
/// codeSize is bytes: it must be a multiple of 4 (the words are 32-bit) and at least the
/// 5-word header, and the word count must be at least the header's own bound claim. Refuses
/// XUD_UNSUPPORTED for a blob that is not SPIR-V at all (bad magic, SPIR-V 2.x, a bound of
/// 0 or 1 - an id upper bound below 2 means the module declares no id, so it declares no
/// entry point and no pipeline can be built from it) rather than handing it to the driver,
/// because "this is not a shader" is the reason, not a bad argument: a caller that embeds a
/// blob of its own gets the report rather than a driver-side validation failure it cannot
/// read. XUD_INVALID_ARG on a NULL code pointer with a non-zero size.
XudResult xud_shader_check_spv(const uint32_t* code, size_t codeSize);

/// Pure: the VkShaderModuleCreateInfo the module will be created with, assembled from the
/// words above. Stamped, pNext NULL (this layer chains nothing into it: the extension-level
/// chain entries that go with VkShaderModuleCreateFlags are decisions this library does not
/// take today), flags 0, codeSize the byte count and pCode the words themselves. XUD_INVALID_
/// ARG on a NULL out-create-info, and XUD_UNSUPPORTED on a blob xud_shader_check_spv() would
/// refuse - so the create-info and the record of it come from one derivation path.
XudResult xud_shader_fill_create_info(VkShaderModuleCreateInfo* outCreateInfo, const uint32_t* code, size_t codeSize);

/// Driver: vkCreateShaderModule through the device row. Before any call: the loader is open,
/// its device rows are this device's rows (xud_loader_device_bound - a module created through
/// another device's rows is a module this device cannot destroy), and the destroy row is
/// checked BEFORE the create row (a module this layer cannot destroy is a module it must not
/// create, the rule xud_window.c applies to a surface and xud_swapchain.c to a swapchain).
/// Then the SPIR-V check, then the assembly, then the create. On XUD_OK *outShader owns a
/// live module; on any error it stays NULL and a module that was created but whose record
/// could not be completed is destroyed before the rung returns. XUD_INVALID_ARG on a NULL
/// out-pointer or one that already owns a module, a NULL loader or device, a handle-less
/// device, or a NULL code pointer with a non-zero size; XUD_UNSUPPORTED when the loader is
/// closed, a row is nil, or the SPIR-V is not SPIR-V; XUD_OUT_OF_MEMORY/XUD_OUT_OF_RESOURCES
/// when the driver refuses the creation on host or device memory, XUD_VULKAN_ERROR otherwise.
XudResult xud_shader_open(XudShader** outShader, XudLoader* loader, const XudDevice* device, const uint32_t* code, size_t codeSize);

/// Driver: vkDestroyShaderModule through the device row, then free the struct. The borrowed
/// code pointer is never freed - see the note above. XUD_ALREADY when the pointer-to-pointer
/// owns no module; XUD_INVALID_ARG on a NULL pointer-to-pointer, a NULL loader/device, a
/// handle-less device or a closed loader; XUD_UNSUPPORTED when the device rows are not this
/// device's or the destroy row is nil while a handle is in hand (the struct is not freed,
/// because freeing it would leak the handle silently).
XudResult xud_shader_close(XudShader** shader, const XudDevice* device, const XudLoader* loader);

#endif /* XUD_SHADER_H_ */
