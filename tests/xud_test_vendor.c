/* xud_test_vendor.c - the vendor rungs: the four implementation TUs, called so the link proves them */

#include "cgltf.h"
#include "miniaudio.h"
#include "stb_image.h"
#include "stb_truetype.h"
#include "xud_test_harness.h"

#include <stdlib.h> /* Required for: free, the bytes stb decodes into */
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

/* A 2x2 24-bit BMP, which is the one image whose bytes are short enough to write out by hand - no e
   ncoder anywhere in the tree, and every byte below is a fact about the container rather than a file
   on disk. Fourteen bytes of file header ('B','M', the 70-byte total, the reserved pair, and the 54-w
   here the texels start), forty of info header (its own 40, the two axes, the plane count, the 24 bit
   s per texel, BI_RGB, the 16-byte texel body and the four words a reader may ignore), then the body
   . Each row is padded to a multiple of four bytes, so two texels take eight, and a POSITIVE height
   means the rows are stored bottom-up: the file's first row is the picture's BOTTOM one (blue, white
   ) and its second is the top one (red, green), each in BGR. stb hands its rows back top-down unless
   asked otherwise, so the decoded first texel is the red one and the alpha byte is filled opaque by t
   he conversion, since a 24-bit body carries none.
*/
static const unsigned char xudTestBmp2x2[70u] = { 66u, 77u, 70u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 54u, 0u, 0u, 0u, 40u, 0u, 0u, 0u, 2u, 0u, 0u,
    0u, 2u, 0u, 0u, 0u, 1u, 0u, 24u, 0u, 0u, 0u, 0u, 0u, 16u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    255u, 0u, 0u, 255u, 255u, 255u, 0u, 0u, 0u, 0u, 255u, 0u, 255u, 0u, 0u, 0u };

static void xud_test_vendor_stb_image(void) {
    static const unsigned char notAnImage[] = { 'n', 'o', 't', 'a', 'n', 'i', 'm', 'g' };
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = NULL;

    xud_check(sizeof(xudTestBmp2x2) == 70u, "vendor/stb-image-bmp-size",
        "14 bytes of file header, 40 of info header and two rows of eight, which is the size the file"
        "header's own size field states");

    xud_check(stbi_info_from_memory(xudTestBmp2x2, (int)sizeof(xudTestBmp2x2), &width, &height, &channels) == 1, "vendor/stb-image-info",
        "the extent query reads the header without decoding the pixels, which is the half a caller che"
        "cks against an image bound before it spends anything");
    xud_check((width == 2) && (height == 2), "vendor/stb-image-info-extent", "and answers the two axes the header states");
    xud_check(channels == 3, "vendor/stb-image-info-channels", "a 24-bit BMP is three channels, before any conversion ask");
    xud_check(stbi_is_16_bit_from_memory(xudTestBmp2x2, (int)sizeof(xudTestBmp2x2)) == 0, "vendor/stb-image-not-16-bit",
        "eight bits per channel, which is the only depth this library's texture format can hold");

    pixels = stbi_load_from_memory(xudTestBmp2x2, (int)sizeof(xudTestBmp2x2), &width, &height, &channels, STBI_rgb_alpha);
    xud_check(pixels != NULL, "vendor/stb-image-load-rgba", "the decode the texture rung's upload path needs: four bytes per texel");
    xud_check((width == 2) && (height == 2), "vendor/stb-image-load-extent", "the out-extent is the decoded one");
    xud_check(channels == 3, "vendor/stb-image-load-source-channels",
        "and the channel out-param stays the FILE's count, so a caller can tell what was converted and"
        " what was already four");
    xud_check((pixels[0] == 255u) && (pixels[1] == 0u) && (pixels[2] == 0u) && (pixels[3] == 255u), "vendor/stb-image-top-left-red",
        "the first texel is the picture's TOP-left one - stb's rows are top-down, which is the order a"
        " VkImage's texel array is written in - and the BGR file bytes came out as RGB");
    xud_check((pixels[4] == 0u) && (pixels[5] == 255u) && (pixels[6] == 0u) && (pixels[7] == 255u), "vendor/stb-image-top-right-green",
        "the second texel of the top row is the green one, not the white one the file's first row star"
        "ts with, which is the bottom-up flip done");
    xud_check((pixels[8] == 0u) && (pixels[9] == 0u) && (pixels[10] == 255u) && (pixels[11] == 255u), "vendor/stb-image-bottom-left-blue",
        "and the second decoded row is the file's first one, blue then white");
    xud_check((pixels[12] == 255u) && (pixels[13] == 255u) && (pixels[14] == 255u) && (pixels[15] == 255u),
        "vendor/stb-image-bottom-right-white",
        "and the alpha channel was filled opaque by the convert"
        "ion, since a 24-bit BMP has none to read");
    stbi_image_free(pixels);

    pixels = stbi_load_from_memory(xudTestBmp2x2, (int)sizeof(xudTestBmp2x2), &width, &height, &channels, 0);
    xud_check(pixels != NULL, "vendor/stb-image-load-native", "the same image with no conversion ask decodes too");
    xud_check(channels == 3, "vendor/stb-image-load-native-channels",
        "and three channels is what it came out as, so the fourth byte per texel the texture rung want"
        "s is the caller's ask rather than the decoder's default");
    stbi_image_free(pixels);

    xud_check(stbi_load_from_memory(notAnImage, (int)sizeof(notAnImage), &width, &height, &channels, STBI_rgb_alpha) == NULL,
        "vendor/stb-image-not-an-image", "bytes that match no container are a NULL answer and not a partially-filled extent");
    xud_check(stbi_failure_reason() != NULL, "vendor/stb-image-failure-reason",
        "and the reason is a string a loader could put in a diagnostic rather than an unexplained NULL");
    xud_check(stbi_info_from_memory(xudTestBmp2x2, 8, &width, &height, &channels) == 0, "vendor/stb-image-truncated",
        "the first eight bytes are the file header alone, and a header whose info block is missing is "
        "a refusal at the extent query too");
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
    xud_test_vendor_stb_image();
    xud_test_vendor_miniaudio();
}
