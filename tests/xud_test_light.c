/***********************************************************************************************
 *
 *   xud_test_light.c - the light rung: the refusals, the row's bytes, and the table's push
 *
 *   Owns : xud_light_validate as the pure refusal (the kind, the finiteness, the zero direction,
 *          the cone order, the range sign - and the fields a kind does NOT read, which the
 *          refusal must leave free) and xud_light_row_fill as the pure byte string (which member
 *          carries which meaning per kind, the CPU-side normalization, the cosines, the 255ths,
 *          and the zeroing of everything the kind does not read); then, on a real context, the
 *          state: the defaults xud_light_reset leaves, xud_set_lights replacing the table whole
 *          and the exact block bytes the two public setters leave behind.
 *   Layer: tests. Sees xud_light.h plus the context and descriptor internals - the block's bytes
 *          are what the fragment stage reads and what only a test may see.
 *
 *   Deterministic: every number a literal. The near() helper carries the sqrt and cos results,
 *   which are the only floats here that binary cannot hold exactly.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#include "xud.h" /* Required for: XudResult, XudLight and the kind tokens, XUD_MAX_LIGHTS */
#include "xud_core.h" /* Required for: XudContext's fields - the table and the floor the public pair stores (the GPU half only) */
#include "xud_descriptor.h" /* Required for: XudDescriptor's uniformBlock - the bytes the setters must leave behind (the GPU half only) */
#include "xud_light.h" /* Required for: xud_light_validate, xud_light_row_fill - the pure half */
#include "xud_renderer.h" /* TEMP */
#include "xud_renderpass.h" /* TEMP */
#include "xud_solid_spv.h" /* Required for: the shipped SPIR-V words - the decorations of the very modules the pipeline builds */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled */

#include "xud_allocator.h" /* Required for: XudMemoryRange, xud_allocator_acquire_host_visible - the probe's block */
#include <math.h> /* Required for: NAN, sqrtf - the refusals' input and the helper's yardstick */
#include <string.h> /* Required for: memset, the zeroed infos the readback stamps */

static int xudTestNear(float actual, float expected, float tolerance) {
    float difference = actual - expected;
    if (difference < 0.0f) {
        difference = -difference;
    }
    return difference <= tolerance;
}

static XudLight xudTestSun(XudVector3 direction, XudColor color, float intensity) {
    XudLight light;
    light.position = (XudVector3) { 0.0f, 0.0f, 0.0f };
    light.direction = direction;
    light.color = color;
    light.intensity = intensity;
    light.range = 0.0f;
    light.innerConeDegrees = 0.0f;
    light.outerConeDegrees = 0.0f;
    light.kind = XUD_LIGHT_KIND_SUN;
    return light;
}

/*-----------------------------------------------------------------------------------------------
 * The refusal: which number of which kind is a refusal, and which free field is not
 *-----------------------------------------------------------------------------------------------*/
static void xud_test_light_validate(void) {
    XudLight sun = xudTestSun((XudVector3) { 1.0f, 2.0f, 3.0f }, (XudColor) { 255u, 255u, 255u, 255u }, 1.0f);
    XudLight point;
    XudLight spot;

    xud_check(xud_light_validate(NULL) == XUD_INVALID_ARG, "light/validate-null", "a light that does not exist is refused");

    sun.kind = 9u;
    xud_check(xud_light_validate(&sun) == XUD_INVALID_ARG, "light/validate-kind", "a kind past the three is refused");
    sun.kind = XUD_LIGHT_KIND_SUN;

    xud_check(xud_light_validate(&sun) == XUD_OK, "light/validate-sun-ok", "the default-shaped sun passes");

    sun.intensity = -1.0f;
    xud_check(xud_light_validate(&sun) == XUD_INVALID_ARG, "light/validate-negative-intensity", "a negative intensity is refused");
    sun.intensity = NAN;
    xud_check(xud_light_validate(&sun) == XUD_INVALID_ARG, "light/validate-nan-intensity", "and so is a NaN one");
    sun.intensity = 1.0f;

    sun.direction = (XudVector3) { 0.0f, 0.0f, 0.0f };
    xud_check(xud_light_validate(&sun) == XUD_INVALID_ARG, "light/validate-zero-direction", "a sun with nowhere to come from is refused");
    sun.direction = (XudVector3) { NAN, 2.0f, 3.0f };
    xud_check(xud_light_validate(&sun) == XUD_INVALID_ARG, "light/validate-nan-direction", "and so is one with a NaN in its way");
    sun.direction = (XudVector3) { 1.0f, 2.0f, 3.0f };

    /* The kind's unread fields are FREE (xud.h): a SUN never looks at position, range or cones,
       and a validator that refused them would be shading with fields the shader does not read.
    */
    sun.position = (XudVector3) { NAN, NAN, NAN };
    sun.range = -5.0f;
    sun.innerConeDegrees = 300.0f;
    xud_check(xud_light_validate(&sun) == XUD_OK, "light/validate-sun-unread-free", "a NaN position and an absurd cone are free on a sun");
    sun.position = (XudVector3) { 0.0f, 0.0f, 0.0f };
    sun.range = 0.0f;
    sun.innerConeDegrees = 0.0f;

    point = sun;
    point.kind = XUD_LIGHT_KIND_POINT;
    point.position = (XudVector3) { 2.0f, 3.0f, 4.0f };
    point.range = 10.0f;
    xud_check(xud_light_validate(&point) == XUD_OK, "light/validate-point-ok", "a shaped point passes");
    point.position = (XudVector3) { 0.0f, NAN, 0.0f };
    xud_check(xud_light_validate(&point) == XUD_INVALID_ARG, "light/validate-point-nan-position",
        "a position a distance would be computed FROM is read, so a NaN is refused");
    point.position = (XudVector3) { 2.0f, 3.0f, 4.0f };
    point.range = -0.5f;
    xud_check(xud_light_validate(&point) == XUD_INVALID_ARG, "light/validate-point-negative-range", "a negative range is refused");
    point.range = NAN;
    xud_check(xud_light_validate(&point) == XUD_INVALID_ARG, "light/validate-point-nan-range", "and so is a NaN one");
    point.range = 0.0f;
    point.direction = (XudVector3) { 0.0f, 0.0f, 0.0f };
    xud_check(xud_light_validate(&point) == XUD_OK, "light/validate-point-direction-free", "the direction a point ignores is free");

    spot = point;
    spot.kind = XUD_LIGHT_KIND_SPOT;
    spot.range = 10.0f;
    spot.direction = (XudVector3) { 0.0f, -1.0f, 0.0f };
    spot.innerConeDegrees = 20.0f;
    spot.outerConeDegrees = 40.0f;
    xud_check(xud_light_validate(&spot) == XUD_OK, "light/validate-spot-ok", "a shaped spot passes");
    spot.direction = (XudVector3) { 0.0f, 0.0f, 0.0f };
    xud_check(xud_light_validate(&spot) == XUD_INVALID_ARG, "light/validate-spot-zero-direction", "a spot with no axis is refused");
    spot.direction = (XudVector3) { 0.0f, -1.0f, 0.0f };
    spot.outerConeDegrees = 180.5f;
    xud_check(
        xud_light_validate(&spot) == XUD_INVALID_ARG, "light/validate-spot-cone-over-180", "a cone wider than a hemisphere is refused");
    spot.outerConeDegrees = -1.0f;
    xud_check(xud_light_validate(&spot) == XUD_INVALID_ARG, "light/validate-spot-negative-cone", "and so is a negative one");
    spot.outerConeDegrees = 10.0f;
    xud_check(xud_light_validate(&spot) == XUD_INVALID_ARG, "light/validate-spot-inner-over-outer",
        "an inner cone wider than the outer is refused: the fade would light OUTSIDE the rim");
    spot.outerConeDegrees = 40.0f;
    spot.innerConeDegrees = 40.0f;
    xud_check(xud_light_validate(&spot) == XUD_OK, "light/validate-spot-equal-cones", "equal cones is the hard-edge beam, and passes");
}

/*-----------------------------------------------------------------------------------------------
 * The row: which member carries which meaning, per kind
 *-----------------------------------------------------------------------------------------------*/
static void xud_test_light_row(void) {
    XudLightRow row;
    XudLight sun = xudTestSun((XudVector3) { 0.0f, 1.0f, 0.0f }, (XudColor) { 255u, 128u, 0u, 255u }, 2.0f);
    XudLight point;
    XudLight spot;
    const float unitY = 1.0f;

    xud_check(xud_light_row_fill(NULL, &sun) == XUD_INVALID_ARG, "light/row-null-out", "the out-param is guarded");
    xud_check(xud_light_row_fill(&row, NULL) == XUD_INVALID_ARG, "light/row-null-light", "and the light's");

    /* The up-sun with a half-green colour at intensity 2: the direction lands in position (the
       one member the kind writes), already unit; target and the range stay zero; the colour's
       255ths and the intensity land in color.
    */
    xud_check(xud_light_row_fill(&row, &sun) == XUD_OK, "light/row-sun-ok", "a sun converts");
    xud_check(xudTestNear(row.position[0u], 0.0f, 1e-6f) && xudTestNear(row.position[1u], unitY, 1e-6f)
            && xudTestNear(row.position[2u], 0.0f, 1e-6f) && (row.position[3u] == 0.0f),
        "light/row-sun-direction-in-position", "its unit direction is the row's position member");
    xud_check((row.target[0u] == 0.0f) && (row.target[1u] == 0.0f) && (row.target[2u] == 0.0f) && (row.target[3u] == 0.0f),
        "light/row-sun-target-zero", "and every member the kind does not read is zero, not stack noise");
    xud_check(xudTestNear(row.color[0u], 1.0f, 1e-6f) && xudTestNear(row.color[1u], 128.0f / 255.0f, 1e-6f)
            && xudTestNear(row.color[2u], 0.0f, 1e-6f) && (row.color[3u] == 2.0f),
        "light/row-sun-color", "the rgb arrives as 255ths and the intensity rides in w");
    xud_check((row.params[0u] == 0.0f) && (row.params[1u] == 0.0f) && (row.params[2u] == 0.0f) && (row.params[3u] == 0.0f),
        "light/row-sun-params", "no range, no cosines, and kind 0 in the kind slot");

    /* The sun whose direction is (1,2,3): the default direction, normalized HERE rather than in
       the shader - the same 1/sqrt(14) the fragment stage used to compile in.
    */
    sun.direction = (XudVector3) { 1.0f, 2.0f, 3.0f };
    xud_check(xud_light_row_fill(&row, &sun) == XUD_OK, "light/row-sun-normalizes", "the (1,2,3) sun converts");
    {
        const float scale = 1.0f / sqrtf(14.0f);
        xud_check(xudTestNear(row.position[0u], scale, 1e-6f) && xudTestNear(row.position[1u], 2.0f * scale, 1e-6f)
                && xudTestNear(row.position[2u], 3.0f * scale, 1e-6f),
            "light/row-sun-unit-length", "and the row's vector is the unit one");
    }

    point = sun;
    point.kind = XUD_LIGHT_KIND_POINT;
    point.position = (XudVector3) { 2.0f, 3.0f, 4.0f };
    point.range = 10.0f;
    xud_check(xud_light_row_fill(&row, &point) == XUD_OK, "light/row-point-ok", "a point converts");
    xud_check((row.position[0u] == 2.0f) && (row.position[1u] == 3.0f) && (row.position[2u] == 4.0f) && (row.position[3u] == 0.0f),
        "light/row-point-position-passthrough", "its position lands unnormalized - a position is not a direction");
    xud_check((row.target[0u] == 0.0f) && (row.params[0u] == 10.0f) && (row.params[1u] == 0.0f) && (row.params[2u] == 0.0f)
            && (row.params[3u] == 1.0f),
        "light/row-point-params", "the range lands in x, no cosines, kind 1 in the kind slot");

    spot = point;
    spot.kind = XUD_LIGHT_KIND_SPOT;
    spot.direction = (XudVector3) { 0.0f, -2.0f, 0.0f };
    spot.innerConeDegrees = 60.0f;
    spot.outerConeDegrees = 90.0f;
    xud_check(xud_light_row_fill(&row, &spot) == XUD_OK, "light/row-spot-ok", "a spot converts");
    xud_check((row.position[0u] == 2.0f) && (row.position[1u] == 3.0f) && (row.position[2u] == 4.0f), "light/row-spot-position",
        "and lights from the same position the point would");
    xud_check(xudTestNear(row.target[0u], 0.0f, 1e-6f) && xudTestNear(row.target[1u], -1.0f, 1e-6f)
            && xudTestNear(row.target[2u], 0.0f, 1e-6f) && (row.target[3u] == 0.0f),
        "light/row-spot-axis-normalized", "the cone axis is the normalized direction - the -2.0f became -1.0f");
    xud_check((row.params[0u] == 10.0f) && xudTestNear(row.params[1u], 0.5f, 1e-5f) && xudTestNear(row.params[2u], 0.0f, 1e-5f)
            && (row.params[3u] == 2.0f),
        "light/row-spot-cosines", "cos 60 is the inner edge, cos 90 the outer, kind 2 in the kind slot");

    spot.kind = 7u;
    xud_check(xud_light_row_fill(&row, &spot) == XUD_INVALID_ARG, "light/row-bad-kind",
        "and the builder refuses the kind the validator would already have caught - it is public, it guards itself");
}

/*-----------------------------------------------------------------------------------------------
 * The state, on a real context: defaults, replacement, and the block bytes behind both
 *-----------------------------------------------------------------------------------------------*/
static void xud_test_light_gpu(void) {
    XudContext* context = NULL;
    XudLight lights[2];
    XudLight bad;
    XudResult result;

    result = xud_init_context(&context, 64u, 64u, "xud_test");
    xud_check(result == XUD_OK, "light-gpu/bringup-completes", "this machine can open a context, so the ladder must complete");
    if (context == NULL) {
        return;
    }

    /* The defaults xud_light_reset seeded between the renderer opening and the handle table:
       the old fragment stage's compile-in light as DATA - one white sun along (1,2,3) at
       intensity 1, a white floor at a quarter.
    */
    xud_check(context->lightCount == 1u, "light-gpu/default-count", "a fresh context shades with exactly one light");
    xud_check(context->lights[0u].kind == XUD_LIGHT_KIND_SUN, "light-gpu/default-kind", "and it is the sun");
    xud_check(
        (context->lights[0u].direction.x == 1.0f) && (context->lights[0u].direction.y == 2.0f) && (context->lights[0u].direction.z == 3.0f),
        "light-gpu/default-direction", "along the direction the fragment stage used to hold as a constant");
    xud_check(context->ambientIntensity == 0.25f, "light-gpu/default-ambient", "and the old compile-in floor is the new ambient");

    xud_check(context->descriptor->uniformBlock.lightCount == 1u, "light-gpu/block-seeded",
        "the seed reached the block the fragment stage reads, not just the context");
    {
        const float scale = 1.0f / sqrtf(14.0f);
        xud_check(xudTestNear(context->descriptor->uniformBlock.lights[0u].position[0u], scale, 1e-6f)
                && xudTestNear(context->descriptor->uniformBlock.lights[0u].position[2u], 3.0f * scale, 1e-6f)
                && (context->descriptor->uniformBlock.lights[0u].params[3u] == 0.0f),
            "light-gpu/block-default-row", "the default sun's row is the normalized direction and kind 0");
    }
    xud_check(xudTestNear(context->descriptor->uniformBlock.ambient[0u], 1.0f, 1e-6f)
            && xudTestNear(context->descriptor->uniformBlock.ambient[3u], 0.25f, 1e-6f),
        "light-gpu/block-default-ambient", "and the floor is white at a quarter, in the block too");

    xud_check(xud_set_lights(NULL, lights, 1u) == XUD_INVALID_ARG, "light-gpu/set-null-context", "the public pair refuses a NULL context");
    xud_check(xud_set_lights(context, NULL, 1u) == XUD_INVALID_ARG, "light-gpu/set-null-table",
        "a table of one that does not exist is refused beside it");
    xud_check(xud_set_lights(context, NULL, 0u) == XUD_OK, "light-gpu/set-empty", "the empty table is the legal zero: no light at all");
    xud_check(
        context->descriptor->uniformBlock.lightCount == 0u, "light-gpu/block-empty", "and the block says zero, so the shader sums nothing");

    /* A two-light table replacing the empty one: the point keeps its position, the spot keeps its
       axis and cosines, and the ROW ORDER is the table order - row i is lights[i].
    */
    lights[0u] = xudTestSun((XudVector3) { 1.0f, 2.0f, 3.0f }, (XudColor) { 255u, 255u, 255u, 255u }, 1.0f);
    lights[1u] = lights[0u];
    lights[1u].kind = XUD_LIGHT_KIND_POINT;
    lights[1u].position = (XudVector3) { 1.0f, 1.0f, 1.0f };
    lights[1u].range = 8.0f;
    lights[1u].color = (XudColor) { 0u, 255u, 0u, 255u };
    xud_check(xud_set_lights(context, lights, 2u) == XUD_OK, "light-gpu/set-two", "a two-light table lands");
    xud_check(context->descriptor->uniformBlock.lightCount == 2u, "light-gpu/block-two", "and the count moved with it");
    xud_check(context->descriptor->uniformBlock.lights[1u].position[0u] == 1.0f, "light-gpu/block-row-order",
        "the point is row 1 of the block because it was index 1 of the table");
    xud_check((context->descriptor->uniformBlock.lights[1u].params[0u] == 8.0f)
            && (context->descriptor->uniformBlock.lights[1u].params[3u] == 1.0f),
        "light-gpu/block-point-row", "with its range and its kind");

    /* A refusal changes NOTHING: the bad row is index 1 of a fresh two-row table, and both the
       stored table and the block must still say the two lights of the accepted call.
    */
    bad = lights[1u];
    bad.range = -1.0f;
    lights[1u] = bad;
    xud_check(
        xud_set_lights(context, lights, 2u) == XUD_INVALID_ARG, "light-gpu/set-refused", "a negative range inside a new table refuses");
    xud_check(context->lightCount == 2u, "light-gpu/refusal-keeps-count", "the old count stands");
    xud_check(context->lights[1u].range == 8.0f, "light-gpu/refusal-keeps-table", "the old table stands");
    xud_check(context->descriptor->uniformBlock.lights[1u].params[0u] == 8.0f, "light-gpu/refusal-keeps-block", "and the old block stands");

    xud_check(xud_set_lights(context, lights, XUD_MAX_LIGHTS + 1u) == XUD_INVALID_ARG, "light-gpu/set-over-cap",
        "one past the table the block carries is refused before anything is read");

    xud_check(xud_set_ambient_light(NULL, (XudColor) { 0u, 0u, 0u, 255u }, 1.0f) == XUD_INVALID_ARG, "light-gpu/ambient-null-context",
        "the floor's setter refuses a NULL context too");
    xud_check(xud_set_ambient_light(context, (XudColor) { 255u, 0u, 0u, 255u }, NAN) == XUD_INVALID_ARG, "light-gpu/ambient-nan",
        "and a NaN scalar - the block would carry it to every fragment");
    xud_check(
        xud_set_ambient_light(context, (XudColor) { 255u, 0u, 0u, 128u }, 0.5f) == XUD_OK, "light-gpu/ambient-ok", "red at half lands");
    xud_check(xudTestNear(context->descriptor->uniformBlock.ambient[0u], 1.0f, 1e-6f)
            && (context->descriptor->uniformBlock.ambient[1u] == 0.0f) && (context->descriptor->uniformBlock.ambient[2u] == 0.0f)
            && (context->descriptor->uniformBlock.ambient[3u] == 0.5f),
        "light-gpu/block-ambient", "as red rgb and the intensity, the alpha byte ignored");
    xud_check((context->ambientColor.r == 255u) && (context->ambientColor.g == 0u) && (context->ambientIntensity == 0.5f),
        "light-gpu/ambient-stored", "and the context's own copy agrees with the block's");

    result = xud_close_context(&context);
    xud_check(result == XUD_OK, "light-gpu/close-clean", "the whole ladder tears back down with nothing refused");
    xud_check(context == NULL, "light-gpu/close-clears", "and the out-param is cleared");
}

/*-----------------------------------------------------------------------------------------------
 * The same layout from the other language: the shipped SPIR-V's own Offset decorations
 *-----------------------------------------------------------------------------------------------*/
static void xud_test_light_spv(void) {
    /* The C side of this contract is the static_asserts beside XudFrameUniformRow and XudLightRow
       (xud_descriptor.h). This half reads the DECORATIONS the shader compiler emitted into the very
       words the pipeline module is built from: if a member's std140 spelling drifts in GLSL while C
       stands - the way a float[2] pad strides to 8 and slides the light array 16 bytes down, which
       is a bug no C assert can see and every pixel shows - the numbers below stop matching and
       the scene's lights land in the wrong members.
    */
    static const uint32_t frameOffsets[11] = { 0u, 64u, 76u, 80u, 96u, 112u, 176u, 192u, 196u, 200u, 208u };
    static const uint32_t rowOffsets[4] = { 0u, 16u, 32u, 48u };
    uint32_t ids[64];
    uint32_t members[64];
    uint32_t offsets[64];
    uint32_t decorations = 0u;
    uint32_t index = 5u;
    uint32_t frameId = 0u;
    uint32_t rowId = 0u;
    uint32_t frameCount = 0u;
    uint32_t rowCount = 0u;
    uint32_t candidate = 0u;
    bool frameFound = false;
    bool rowFound = false;

    /* The walk is the one the SPIR-V spec's fixed layout invites: word 0 is the magic, words 1 to 4
       the version, ID bound, schema and flags, and every instruction after that is a word count in
       the top half and an opcode in the bottom. OpMemberDecorate is opcode 72 - re-read off the
       assembler's own output rather than trusted from memory.
    */
    while (index < (uint32_t)(sizeof xudSolidFragSpv / sizeof xudSolidFragSpv[0])) {
        const uint32_t word = xudSolidFragSpv[index];
        const uint32_t opcode = word & 0xFFFFu;
        const uint32_t length = (word >> 16u) & 0xFFFFu;

        if (length == 0u) {
            break;
        }
        if ((opcode == 72u) && (decorations < 64u)) { /* OpMemberDecorate */
            if (xudSolidFragSpv[index + 3u] == 35u) { /* Decoration Offset */
                ids[decorations] = xudSolidFragSpv[index + 1u];
                members[decorations] = xudSolidFragSpv[index + 2u];
                offsets[decorations] = xudSolidFragSpv[index + 4u];
                decorations++;
            }
        }
        index += length;
    }

    xud_check(decorations > 12u, "light/spv-decorations", "the shipped fragment module carries its member offsets");

    for (candidate = 0u; candidate < decorations; candidate++) {
        uint32_t id = ids[candidate];
        uint32_t count = 0u;
        uint32_t walker = 0u;
        for (walker = 0u; walker < decorations; walker++) {
            if (ids[walker] == id) {
                count++;
            }
        }
        if (count == 11u) {
            frameId = id;
            frameCount = count;
            frameFound = true;
        } else if (count == 4u) {
            rowId = id;
            rowCount = count;
            rowFound = true;
        }
    }
    (void)frameCount;
    (void)rowCount;
    xud_check(frameFound, "light/spv-frame-struct", "one eleven-member offset-decorated struct: the frame block");
    xud_check(rowFound, "light/spv-row-struct", "and a four-member one: the light row");

    for (candidate = 0u; candidate < decorations; candidate++) {
        if ((ids[candidate] == frameId) && (members[candidate] < 11u)) {
            xud_check(offsets[candidate] == frameOffsets[members[candidate]], "light/spv-frame-offset",
                "every frame-block member's Offset equals the C offset beside its struct");
        }
        if ((ids[candidate] == rowId) && (members[candidate] < 4u)) {
            xud_check(offsets[candidate] == rowOffsets[members[candidate]], "light/spv-row-offset",
                "and the light row's four members sit at 0, 16, 32 and 48 in the module too");
        }
    }

    /* The same parse for the VERTEX module, whose block is the PREFIX of the one above - the
       matrix the frame writes is read by that stage, so a drift there clips the scene away.
    */
    {
        static const uint32_t vertFrameOffsets[9] = { 0u, 64u, 76u, 80u, 96u, 112u, 176u, 192u, 196u };
        decorations = 0u;
        index = 5u;
        frameFound = false;
        frameId = 0u;
        while (index < (uint32_t)(sizeof xudSolidVertSpv / sizeof xudSolidVertSpv[0])) {
            const uint32_t word = xudSolidVertSpv[index];
            const uint32_t opcode = word & 0xFFFFu;
            const uint32_t length = (word >> 16u) & 0xFFFFu;
            if (length == 0u) {
                break;
            }
            if ((opcode == 72u) && (decorations < 64u)) {
                if (xudSolidVertSpv[index + 3u] == 35u) {
                    ids[decorations] = xudSolidVertSpv[index + 1u];
                    members[decorations] = xudSolidVertSpv[index + 2u];
                    offsets[decorations] = xudSolidVertSpv[index + 4u];
                    decorations++;
                }
            }
            index += length;
        }
        for (candidate = 0u; candidate < decorations; candidate++) {
            uint32_t count = 0u;
            uint32_t walker = 0u;
            for (walker = 0u; walker < decorations; walker++) {
                if (ids[walker] == ids[candidate]) {
                    count++;
                }
            }
            if (count == 9u) {
                frameId = ids[candidate];
                frameFound = true;
            }
        }
        xud_check(frameFound, "light/spv-vert-frame-struct", "the vertex module carries the nine-member prefix block");
        for (candidate = 0u; candidate < decorations; candidate++) {
            if ((ids[candidate] == frameId) && (members[candidate] < 9u)) {
                xud_check(offsets[candidate] == vertFrameOffsets[members[candidate]], "light/spv-vert-frame-offset",
                    "and its matrix, camera, time, fog, ambient and counters sit where the C row puts them");
            }
        }
    }
}

/*-----------------------------------------------------------------------------------------------
 * The shadow's proof by pixels: a white floor and a white box under one sun, read back off the
 * swapchain twice - once with the sun leaning over +x and once with the same sun leaning the
 * other way. The shadow map is the only thing in the tree that moves a dark floor pixel when
 * the sun vector flips, so the two reads must swap their answers: no shadow pass, a flipped
 * compare or a dead matrix shows up as the pair NOT swapping.
 *-----------------------------------------------------------------------------------------------*/

#define XUD_TEST_SHADOW_WINDOW 192u /* the square window; the two probe columns mirror on its middle */
#define XUD_TEST_SHADOW_ACQUIRE_TRIES 8u /* the out-of-date repair's bound, the readback tests' bound */
#define XUD_TEST_SHADOW_LIT_MIN 120u /* a sunlit byte under zero ambient sits near 200; anything above this is lit */
#define XUD_TEST_SHADOW_DARK_MAX 40u /* and the shadowed one sits at zero; anything below this is dark */

/* One frame of the probe scene: the floor's top face is y = 0 and the box stands on it centred,
   both white, so the only thing that separates the two probe columns is whether the sun reaches
   them. */
static XudResult xudTestShadowFrame(XudContext* context, XudCamera3D camera) {
    XudResult step;

    step = xud_begin_drawing(context);
    if (step == XUD_OK) {
        step = xud_clear_background(context, (XudColor) { 0u, 0u, 0u, 255u });
    }
    if (step == XUD_OK) {
        step = xud_begin_camera_3d(context, camera);
    }
    if (step == XUD_OK) {
        step = xud_draw_cube(context, (XudVector3) { 0.0f, -0.5f, 0.0f }, 20.0f, 1.0f, 20.0f, (XudColor) { 255u, 255u, 255u, 255u });
    }
    if (step == XUD_OK) {
        step = xud_draw_cube(context, (XudVector3) { 0.0f, 1.0f, 0.0f }, 2.0f, 2.0f, 2.0f, (XudColor) { 255u, 255u, 255u, 255u });
    }
    if (step == XUD_OK) {
        step = xud_end_camera_3d(context);
    }
    if (step == XUD_OK) {
        step = xud_end_drawing(context);
    }
    return step;
}

static void xud_test_light_shadow(void) {
    XudContext* context = NULL;
    XudLight sun;
    XudCamera3D camera;
    XudResult result;
    XudResult step;
    XudMemoryRange readbackRange;
    VkResult vk = VK_SUCCESS;
    VkBuffer readback = VK_NULL_HANDLE;
    VkMemoryRequirements readbackReq;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer copyCommand = VK_NULL_HANDLE;
    VkFence acquireFence = VK_NULL_HANDLE;
    VkFence copyFence = VK_NULL_HANDLE;
    uint8_t* pixels = NULL;
    uint32_t frame = 0u;
    uint32_t phase = 0u;
    uint32_t extentBeforeWidth = 0u;
    uint32_t extentBeforeHeight = 0u;
    uint32_t resizeRetries = 0u;
    uint32_t redIndex = 0u;
    uint32_t probeRow = 0u;
    uint32_t probeOffset = 0u;
    uint32_t leftByte = 0u;
    uint32_t rightByte = 0u;

    const float sunLean = 0.70710678f;
    const float probeDepth = 11.6619f; /* |(0,6,10)|: the view depth of every floor point on the probe row */
    const float halfTan = 0.41421356f; /* tan(fovy / 2) at the camera's 45 degrees */

    result = xud_init_context(&context, XUD_TEST_SHADOW_WINDOW, XUD_TEST_SHADOW_WINDOW, "xud_test");
    xud_check(result == XUD_OK, "light-shadow/bringup", "the probe scene needs a completed context");
    if (context == NULL) {
        return;
    }
    (void)xud_set_ambient_light(context, (XudColor) { 0u, 0u, 0u, 255u }, 0.0f);
    camera = (XudCamera3D) { .position = (XudVector3) { 0.0f, 6.0f, 10.0f },
        .target = (XudVector3) { 0.0f, 0.0f, 0.0f },
        .up = (XudVector3) { 0.0f, 1.0f, 0.0f },
        .fovy = 45.0f,
        .near = 0.1f,
        .far = 100.0f };

    if (vk == VK_SUCCESS) {
        VkCommandPoolCreateInfo poolInfo;
        VkCommandBufferAllocateInfo allocationInfo;
        VkFenceCreateInfo fenceInfo;
        memset(&poolInfo, 0, sizeof poolInfo);
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = context->device->graphicsQueue.queueFamilyIndex;
        vk = context->loader->deviceTable.vkCreateCommandPool(context->device->device, &poolInfo, NULL, &pool);
        memset(&allocationInfo, 0, sizeof allocationInfo);
        allocationInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocationInfo.commandPool = pool;
        allocationInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocationInfo.commandBufferCount = 1u;
        vk = context->loader->deviceTable.vkAllocateCommandBuffers(context->device->device, &allocationInfo, &copyCommand);
        memset(&fenceInfo, 0, sizeof fenceInfo);
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vk = context->loader->deviceTable.vkCreateFence(context->device->device, &fenceInfo, NULL, &acquireFence);
        vk = context->loader->deviceTable.vkCreateFence(context->device->device, &fenceInfo, NULL, &copyFence);
        xud_check(vk == VK_SUCCESS, "light-shadow/plumbing", "the copy's pool, command buffer and fences are live");
    }

    /* The window manager's late tile - the pair of resize recreates that lands seconds after
       map on this compositor - must land BEFORE the probes, or a phase reads an image whose
       extent moved under it. Spin frames until the extent answers the same size three times. */
    if (vk == VK_SUCCESS) {
        uint32_t stableFrames = 0u;
        uint32_t spinFrames = 0u;
        uint32_t lastWidth = context->swapchain->createInfo.imageExtent.width;
        uint32_t lastHeight = context->swapchain->createInfo.imageExtent.height;
        while ((stableFrames < 12u) && (spinFrames < 600u)) {
            (void)xudTestShadowFrame(context, camera);
            if ((context->swapchain->createInfo.imageExtent.width == lastWidth)
                && (context->swapchain->createInfo.imageExtent.height == lastHeight)) {
                stableFrames++;
            } else {
                stableFrames = 0u;
                lastWidth = context->swapchain->createInfo.imageExtent.width;
                lastHeight = context->swapchain->createInfo.imageExtent.height;
            }
            spinFrames++;
        }
    }

    /* Two reads of one scene, the sun's lean the only thing that moves between them: the dark
       column must travel with the lean. A missing shadow pass leaves the pair identical, a
       flipped compare leaves it swapped against the expectation, and a dead matrix leaves both
       columns lit - three different failures, none of them invisible to this pair. */
    for (phase = 0u; (phase < 2u) && (vk == VK_SUCCESS); phase++) {
        uint32_t acquired = 0u;
        uint32_t tries = 0u;

        extentBeforeWidth = context->swapchain->createInfo.imageExtent.width;
        extentBeforeHeight = context->swapchain->createInfo.imageExtent.height;
        const float lean = (phase == 0u) ? sunLean : -sunLean;
        sun = xudTestSun((XudVector3) { lean, sunLean, 0.0f }, (XudColor) { 255u, 255u, 255u, 255u }, 1.0f);
        step = xud_set_lights(context, &sun, 1u);
        xud_check(step == XUD_OK, "light-shadow/lights-set", "the one-sun table publishes");
        for (frame = 0u; frame < (context->swapchain->imageCount + 1u); frame++) {
            (void)xudTestShadowFrame(context, camera);
        }
        for (;;) {
            (void)context->loader->deviceTable.vkResetFences(context->device->device, 1u, &acquireFence);
            vk = context->loader->deviceTable.vkAcquireNextImageKHR(
                context->device->device, context->swapchain->swapchain, UINT64_MAX, VK_NULL_HANDLE, acquireFence, &acquired);
            (void)context->loader->deviceTable.vkWaitForFences(context->device->device, 1u, &acquireFence, VK_TRUE, UINT64_MAX);
            if (vk == VK_ERROR_OUT_OF_DATE_KHR) {
                VkPresentInfoKHR presentInfo;
                memset(&presentInfo, 0, sizeof presentInfo);
                presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
                presentInfo.swapchainCount = 1u;
                presentInfo.pSwapchains = &context->swapchain->swapchain;
                presentInfo.pImageIndices = &acquired;
                (void)context->loader->deviceTable.vkQueuePresentKHR(context->device->graphicsQueue.queue, &presentInfo);
                (void)xudTestShadowFrame(context, camera);
                tries++;
                if (tries < XUD_TEST_SHADOW_ACQUIRE_TRIES) {
                    continue;
                }
            }
            break;
        }
        xud_check(
            (vk == VK_SUCCESS) || (vk == VK_SUBOPTIMAL_KHR), "light-shadow/acquired", "a presented image of the scene is ours to copy");
        if ((vk != VK_SUCCESS) && (vk != VK_SUBOPTIMAL_KHR)) {
            break;
        }
        vk = VK_SUCCESS;
        {
            const uint32_t width = context->swapchain->createInfo.imageExtent.width;
            const uint32_t height = context->swapchain->createInfo.imageExtent.height;

            const VkFormat format = context->swapchain->createInfo.imageFormat;
            const float aspect = (float)width / (float)height;
            VkCommandBufferBeginInfo beginInfo;
            VkImageMemoryBarrier imageBarrier;
            VkImageSubresourceLayers subresource;
            VkBufferImageCopy region;
            VkSubmitInfo submitInfo;

            VkBufferCreateInfo bufferInfo;
            memset(&bufferInfo, 0, sizeof bufferInfo);
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = (VkDeviceSize)width * (VkDeviceSize)height * 4u;
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            vk = context->loader->deviceTable.vkCreateBuffer(context->device->device, &bufferInfo, NULL, &readback);
            xud_check(vk == VK_SUCCESS, "light-shadow/buffer", "the probe's destination buffer is sized to the LIVE extent");
            if (vk != VK_SUCCESS) {
                break;
            }
            context->loader->deviceTable.vkGetBufferMemoryRequirements(context->device->device, readback, &readbackReq);
            {
                const VkMemoryType* memTypes = context->physicalDevice->caps.memoryProperties.memoryProperties.memoryTypes;
                const uint32_t memTypeCount = context->physicalDevice->caps.memoryProperties.memoryProperties.memoryTypeCount;
                uint32_t hostVisibleMask = 0u;
                uint32_t type = 0u;
                for (type = 0u; type < memTypeCount; type++) {
                    if ((memTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u) {
                        hostVisibleMask |= (1u << type);
                    }
                }
                readbackReq.memoryTypeBits &= hostVisibleMask;
            }
            result = xud_allocator_acquire_host_visible(
                &readbackRange, context->allocator, context->loader, context->device, &context->physicalDevice->caps, &readbackReq);
            xud_check(result == XUD_OK, "light-shadow/block", "a host-visible block is carved for it");
            vk = context->loader->deviceTable.vkBindBufferMemory(
                context->device->device, readback, readbackRange.memory, readbackRange.offset);
            xud_check(vk == VK_SUCCESS, "light-shadow/bound", "and the buffer binds into it");
            vk = context->loader->deviceTable.vkMapMemory(
                context->device->device, readbackRange.memory, readbackRange.offset, readbackRange.size, 0u, (void**)&pixels);
            xud_check((vk == VK_SUCCESS) && (pixels != NULL), "light-shadow/mapped", "and the mapping is live");
            if ((vk != VK_SUCCESS) || (pixels == NULL)) {
                break;
            }

            redIndex = ((format == VK_FORMAT_B8G8R8A8_SRGB) || (format == VK_FORMAT_B8G8R8A8_UNORM)) ? 2u : 0u;
            probeRow = height / 2u;
            probeOffset = (uint32_t)(((2.5f / (halfTan * aspect * probeDepth)) * (float)(width / 2u)) + 0.5f);
            xud_check((probeOffset > 4u) && (probeOffset < ((width / 2u) - 4u)), "light-shadow/probes-in-view",
                "both probe columns land on the floor: clear of the image's edge and of the box's silhouette");

            (void)context->loader->deviceTable.vkResetCommandBuffer(copyCommand, 0u);
            memset(&beginInfo, 0, sizeof beginInfo);
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vk = context->loader->deviceTable.vkBeginCommandBuffer(copyCommand, &beginInfo);
            memset(&imageBarrier, 0, sizeof imageBarrier);
            imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            imageBarrier.srcAccessMask = 0u;
            imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            imageBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            imageBarrier.image = context->swapchain->images[acquired];
            imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            imageBarrier.subresourceRange.levelCount = 1u;
            imageBarrier.subresourceRange.layerCount = 1u;
            context->loader->deviceTable.vkCmdPipelineBarrier(copyCommand, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u, NULL, 1u, &imageBarrier);
            memset(&subresource, 0, sizeof subresource);
            subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            subresource.layerCount = 1u;
            memset(&region, 0, sizeof region);
            region.bufferImageHeight = height;
            region.imageSubresource = subresource;
            region.imageExtent.width = width;
            region.imageExtent.height = height;
            region.imageExtent.depth = 1u;
            context->loader->deviceTable.vkCmdCopyImageToBuffer(
                copyCommand, context->swapchain->images[acquired], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1u, &region);
            imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            imageBarrier.dstAccessMask = 0u;
            imageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            imageBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            context->loader->deviceTable.vkCmdPipelineBarrier(copyCommand, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0u, 0u, NULL, 0u, NULL, 1u, &imageBarrier);
            vk = context->loader->deviceTable.vkEndCommandBuffer(copyCommand);
            (void)context->loader->deviceTable.vkResetFences(context->device->device, 1u, &copyFence);
            memset(&submitInfo, 0, sizeof submitInfo);
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1u;
            submitInfo.pCommandBuffers = &copyCommand;
            vk = context->loader->deviceTable.vkQueueSubmit(context->device->graphicsQueue.queue, 1u, &submitInfo, copyFence);
            vk = context->loader->deviceTable.vkWaitForFences(context->device->device, 1u, &copyFence, VK_TRUE, UINT64_MAX);
            xud_check(vk == VK_SUCCESS, "light-shadow/copied", "the copy's fence signals and the bytes are host-visible");
            if (vk != VK_SUCCESS) {
                break;
            }
            leftByte = pixels[((((size_t)probeRow * (size_t)width) + ((size_t)width / 2u)) - (size_t)probeOffset) * 4u + (size_t)redIndex];
            rightByte = pixels[((((size_t)probeRow * (size_t)width) + ((size_t)width / 2u)) + (size_t)probeOffset) * 4u + (size_t)redIndex];
        }
        (void)context->loader->deviceTable.vkUnmapMemory(context->device->device, readbackRange.memory);
        pixels = NULL;
        context->loader->deviceTable.vkDestroyBuffer(context->device->device, readback, NULL);
        readback = VK_NULL_HANDLE;
        (void)xud_allocator_release(context->allocator, &readbackRange);
        memset(&readbackRange, 0, sizeof readbackRange);
        if ((resizeRetries < 6u)
            && ((context->swapchain->createInfo.imageExtent.width != extentBeforeWidth)
                || (context->swapchain->createInfo.imageExtent.height != extentBeforeHeight))) {
            resizeRetries++;
            phase--;
            continue;
        }
        if (phase == 0u) {
            xud_check(leftByte <= XUD_TEST_SHADOW_DARK_MAX, "light-shadow/sun-plus-x-left-dark",
                "with the sun leaning over +x the floor left of the box is in its shadow");
            xud_check(rightByte >= XUD_TEST_SHADOW_LIT_MIN, "light-shadow/sun-plus-x-right-lit",
                "and the mirrored column on the sun's own side of the box is lit");
        } else {
            xud_check(leftByte >= XUD_TEST_SHADOW_LIT_MIN, "light-shadow/sun-minus-x-left-lit",
                "lean the same sun the other way and the left column is the lit one");
            xud_check(rightByte <= XUD_TEST_SHADOW_DARK_MAX, "light-shadow/sun-minus-x-right-dark",
                "while the right column, lit a moment ago, is now in the shadow - the map moved with the sun");
        }
    }

    if (pixels != NULL) {
        context->loader->deviceTable.vkUnmapMemory(context->device->device, readbackRange.memory);
    }
    if (acquireFence != VK_NULL_HANDLE) {
        context->loader->deviceTable.vkDestroyFence(context->device->device, acquireFence, NULL);
    }
    if (copyFence != VK_NULL_HANDLE) {
        context->loader->deviceTable.vkDestroyFence(context->device->device, copyFence, NULL);
    }
    if (pool != VK_NULL_HANDLE) {
        context->loader->deviceTable.vkFreeCommandBuffers(context->device->device, pool, 1u, &copyCommand);
        context->loader->deviceTable.vkDestroyCommandPool(context->device->device, pool, NULL);
    }
    if (readback != VK_NULL_HANDLE) {
        context->loader->deviceTable.vkDestroyBuffer(context->device->device, readback, NULL);
    }
    (void)xud_close_context(&context);
}

void xud_test_light(void) {
    xud_test_light_validate();
    xud_test_light_row();
    xud_test_light_spv();
    if (xud_gpu_tests_enabled()) {
        xud_test_light_gpu();
        xud_test_light_shadow();
    }
}
