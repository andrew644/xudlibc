/* xud_test_vendor.c - the vendor rungs: the three implementation TUs, called so the link proves them */

#include "cgltf.h"
#include "miniaudio.h"
#include "stb_truetype.h"
#include "xud_test_harness.h"

#include <string.h> /* Required for: strcmp */

static void xud_test_vendor_cgltf(void) {
    static const char json[] = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"xudlibc\"},\"nodes\":[{},{},{}]}";
    static const char legacy[] = "glTF\0\0\0\0\0\0\0\0";
    cgltf_options auto_detect = (cgltf_options) { .type = cgltf_file_type_invalid, .json_token_count = 0u, .memory = { 0 }, .file = { 0 } };
    cgltf_options want_glb = (cgltf_options) { .type = cgltf_file_type_glb, .json_token_count = 0u, .memory = { 0 }, .file = { 0 } };
    cgltf_data* data = NULL;
    cgltf_result result = cgltf_result_success;

    result = cgltf_parse(NULL, json, sizeof(json), &data);
    xud_check(result == cgltf_result_invalid_options, "vendor/cgltf-null-options", "no options is a bad argument, not an empty document");

    result = cgltf_parse(&auto_detect, json, 4u, &data);
    xud_check(result == cgltf_result_data_too_short, "vendor/cgltf-too-short",
        "under the 12-byte GLB header size there is nothing to sniff, so the parser refuses before it looks");

    result = cgltf_parse(&want_glb, json, sizeof(json) - 1u, &data);
    xud_check(result == cgltf_result_unknown_format, "vendor/cgltf-type-mismatch",
        "an options that asks for GLB and hands JSON is a format refusal, not a JSON syntax refusal");

    result = cgltf_parse(&auto_detect, legacy, 12u, &data);
    xud_check(result == cgltf_result_legacy_gltf, "vendor/cgltf-legacy-glb",
        "the GLB magic with a version word below 1 is a legacy binary, which this parser does not read");

    result = cgltf_parse(&auto_detect, json, sizeof(json) - 1u, &data);
    xud_check(result == cgltf_result_success, "vendor/cgltf-parse", "the fixed document parses");
    xud_check(data != NULL, "vendor/cgltf-out-set", "the out-param is filled on success");
    xud_check(data->file_type == cgltf_file_type_gltf, "vendor/cgltf-file-type",
        "auto-detect lands on the JSON branch, which is what stamps the file type");
    xud_check(data->asset.version != NULL && strcmp(data->asset.version, "2.0") == 0, "vendor/cgltf-asset-version",
        "the asset block is read into the record, so a caller can gate on the glTF version");
    xud_check(data->nodes_count == 3u, "vendor/cgltf-node-count",
        "the array counts are the document's, which is the allocation the implementation ran");
    xud_check(data->meshes_count == 0u, "vendor/cgltf-empty-array", "a section the document omits is a zero count, not garbage");
    cgltf_free(data);

    xud_check(cgltf_num_components(cgltf_type_vec3) == 3u, "vendor/cgltf-num-components",
        "the attribute type to component count table is the implementation's");
    xud_check(cgltf_calc_size(cgltf_type_vec3, cgltf_component_type_r_32f) == 12u, "vendor/cgltf-calc-size",
        "type times component size is what a buffer view's byte length is derived from");
}

static void xud_test_vendor_stb_truetype(void) {
    static const unsigned char ttc[] = { 't', 't', 'c', 'f', 0, 1, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0x20, 0, 0, 0, 0x40 };
    static const unsigned char ttf[] = { '1', 0, 0, 0, 0, 0, 0, 5 };
    static const unsigned char notfont[] = { 'n', 'o', 't', 'e' };

    xud_check(stbtt_GetNumberOfFonts(ttc) == 2, "vendor/stb-ttc-count",
        "the collection header's font count is read big-endian, which is the file format's rule");
    xud_check(stbtt_GetFontOffsetForIndex(ttc, 0) == 32, "vendor/stb-ttc-offset-0",
        "index 0 reads the first table entry after the 12-byte collection header");
    xud_check(stbtt_GetFontOffsetForIndex(ttc, 1) == 64, "vendor/stb-ttc-offset-1",
        "the entries are four bytes apart, so the second one is the second answer");
    xud_check(stbtt_GetFontOffsetForIndex(ttc, 2) == -1, "vendor/stb-ttc-out-of-range",
        "an index the count does not cover is refused, not clamped");

    xud_check(stbtt_GetNumberOfFonts(ttf) == 1, "vendor/stb-ttf-count",
        "a single-font header is a font, so the count is one without any table being read");
    xud_check(stbtt_GetFontOffsetForIndex(ttf, 0) == 0, "vendor/stb-ttf-offset-0", "a plain .ttf sits at offset 0");
    xud_check(stbtt_GetFontOffsetForIndex(ttf, 1) == -1, "vendor/stb-ttf-out-of-range", "a plain .ttf has one index");

    xud_check(stbtt_GetNumberOfFonts(notfont) == 0, "vendor/stb-not-a-font",
        "neither a sfnt version word nor a collection tag is not a font, and the answer is zero rather than a crash");
    xud_check(stbtt_GetFontOffsetForIndex(notfont, 0) == -1, "vendor/stb-not-a-font-index", "the same refusal on the other entry point");
}

static void xud_test_vendor_miniaudio(void) {
    ma_uint32 major = 0u;
    ma_uint32 minor = 0u;
    ma_uint32 revision = 0u;

    ma_version(&major, &minor, &revision);
    xud_check(major == MA_VERSION_MAJOR && minor == MA_VERSION_MINOR && revision == MA_VERSION_REVISION, "vendor/miniaudio-version",
        "the definition the implementation TU emits agrees with the header the caller compiled against");
    xud_check(ma_version_string() != NULL && strcmp(ma_version_string(), MA_VERSION_STRING) == 0, "vendor/miniaudio-version-string",
        "the same version as a string, which is what a log line would print");

    xud_check(ma_get_bytes_per_sample(ma_format_u8) == 1u, "vendor/miniaudio-bytes-u8", "the format table is the implementation's");
    xud_check(ma_get_bytes_per_sample(ma_format_s16) == 2u, "vendor/miniaudio-bytes-s16", "the device format xud would ask for");
    xud_check(ma_get_bytes_per_sample(ma_format_s24) == 3u, "vendor/miniaudio-bytes-s24", "tightly packed, so three bytes and no pad");
    xud_check(ma_get_bytes_per_sample(ma_format_f32) == 4u, "vendor/miniaudio-bytes-f32", "the float form the mixer works in");
    xud_check(ma_get_bytes_per_sample(ma_format_unknown) == 0u, "vendor/miniaudio-bytes-unknown",
        "an unknown format has no size, which is what makes a caller's division-by-size a guard rather than a bug");
}

void xud_test_vendor(void) {
    xud_test_vendor_cgltf();
    xud_test_vendor_stb_truetype();
    xud_test_vendor_miniaudio();
}
