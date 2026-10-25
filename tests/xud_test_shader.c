/***********************************************************************************************
 *
 *   xud_test_shader.c - the shader rung: SPIR-V words to a VkShaderModule and back
 *
 *   Owns : the SPIR-V header check as the spec's own rule rather than the tool's exact magic, and the one create-info assembly. Pure: the
 * words are borrowed, so this area never destroys a module it must not create. Layer: tests. Sees xud_shader.h. The words themselves are
 * the build's, compiled from shaders/solid.vert into build/gen/xud_solid_spv.h - nothing SPIR-V is committed.
 *
 *   Deterministic: fixed loop counts, no time, no randomness, no filesystem.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XUD_VERSION, the XUD_GPU_TESTS gate */
#include "xud_shader.h" /* Required for: xud_shader_check_spv, xud_shader_fill_create_info, the close */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled, the area entry points */

/*-----------------------------------------------------------------------------------------------
 * The shader rung, headless: the SPIR-V header rule read as a check rather than as a toolchain
 * assumption, and the create-info assembly. The blob below is the header glslangValidator writ
 * eS here (magic 0x07230203 = SPIR-V 1.3, version word 0x00010000 = 1.0, generator 0x0008000b
 * = Khronos glslang, id bound 52, instruction count 0) followed by one filler word, so every r
 * efusal tested is one word changed in that same header.
 *-----------------------------------------------------------------------------------------------*/

static void xud_test_shader_pure(void) {
    const uint32_t good[] = { 0x07230203u, 0x00010000u, 0x0008000Bu, 52u, 0u, 1u };
    const uint32_t notSpv[] = { 0x474f5245u, 0x00010000u, 0u, 52u, 0u };
    const uint32_t tooSmall[] = { 0x07230203u, 0x00010000u, 0u };
    const uint32_t badBound[] = { 0x07230203u, 0x00010000u, 0u, 1u, 0u };
    const uint32_t badMajor[] = { 0x07230203u, 0x00020000u, 0u, 52u, 0u };
    const uint32_t versionHigh[] = { 0x072302FFu, 0x00010000u, 0u, 52u, 0u };
    VkShaderModuleCreateInfo info = (VkShaderModuleCreateInfo) { .sType = VK_STRUCTURE_TYPE_MAX_ENUM };
    XudResult result = XUD_OK;

    result = xud_shader_check_spv(NULL, 0u);
    xud_check(result == XUD_INVALID_ARG, "shader/check-null-code", "no words at all is a bad argument, not a bad shader");
    result = xud_shader_check_spv(good, 0u);
    xud_check(result == XUD_INVALID_ARG, "shader/check-zero-size", "a zero length is a bad argument: there is nothing to check");
    result = xud_shader_check_spv(good, sizeof(good));
    xud_check(result == XUD_OK, "shader/check-good",
        "the header glslangValidator writes here passes, which is the whole point of the rule b"
        "eing the spec's form rather than one exact magic");

    xud_check(xud_shader_check_spv(notSpv, sizeof(notSpv)) == XUD_UNSUPPORTED, "shader/check-not-spirv",
        "not SPIR-V at all is reported as not-a-shader, not as a bad argument: a caller that embe"
        "ds a blob of its own gets the reason, not a driver-side validation failure it cannot read");
    xud_check(xud_shader_check_spv(tooSmall, sizeof(tooSmall)) == XUD_UNSUPPORTED, "shader/check-short",
        "shorter than the 5-word header cannot be a module");
    xud_check(xud_shader_check_spv(good, sizeof(good) - 1u) == XUD_UNSUPPORTED, "shader/check-not-word-multiple",
        "a byte count that is not a multiple of 4 is not a module, because the words are 32-bit");
    xud_check(xud_shader_check_spv(badBound, sizeof(badBound)) == XUD_UNSUPPORTED, "shader/check-bound",
        "an id upper bound below 2 declares no id at all, so no entry point and no pipeline");
    xud_check(xud_shader_check_spv(badMajor, sizeof(badMajor)) == XUD_UNSUPPORTED, "shader/check-major",
        "a SPIR-V 2.x module is not one these headers take");
    xud_check(xud_shader_check_spv(versionHigh, sizeof(versionHigh)) == XUD_UNSUPPORTED, "shader/check-version-byte",
        "a version byte above the highest the spec defines is refused, while the fixed 24 bits b"
        "elow it are the same as a good module's");

    xud_check(xud_shader_fill_create_info(NULL, good, sizeof(good)) == XUD_INVALID_ARG, "shader/fill-null-out", "refused");
    xud_check(xud_shader_fill_create_info(&info, notSpv, sizeof(notSpv)) == XUD_UNSUPPORTED, "shader/fill-not-spirv",
        "the create-info and the check share one derivation path, so a blob the check refuses nev"
        "er reaches an assembled create-info");
    xud_check(xud_shader_fill_create_info(&info, good, sizeof(good)) == XUD_OK, "shader/fill-ok", "the create-info is assembled");
    xud_check(info.sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, "shader/fill-stamped", "stamped");
    xud_check(info.pNext == NULL, "shader/fill-pnext-clear", "nothing is chained in");
    xud_check(info.flags == 0u, "shader/fill-flags-clear", "no module flag is asked for");
    xud_check(info.codeSize == sizeof(good), "shader/fill-size",
        "the byte count is handed over as it was, so the record and the create-info cannot disagr"
        "ee about length");
    xud_check(info.pCode == good, "shader/fill-code-row",
        "the words are borrowed, not copied: the create-info points at the caller's own array, wh"
        "ich is what keeps the built-in set in static storage and lets xud_shader_close() free no"
        "thing beside the module");
}

void xud_test_shader(void) { xud_test_shader_pure(); }
