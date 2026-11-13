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
#include "xud_solid_spv.h" /* Required for: the shipped SPIR-V words - the decorations of the very modules the pipeline builds */
#include "xud_test_harness.h" /* Required for: xud_check, xud_gpu_tests_enabled */

#include <math.h> /* Required for: NAN, sqrtf - the refusals' input and the helper's yardstick */

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
    static const uint32_t frameOffsets[9] = { 0u, 64u, 76u, 80u, 96u, 112u, 116u, 120u, 128u };
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
        if (count == 9u) {
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
    xud_check(frameFound, "light/spv-frame-struct", "one nine-member offset-decorated struct: the frame block");
    xud_check(rowFound, "light/spv-row-struct", "and a four-member one: the light row");

    for (candidate = 0u; candidate < decorations; candidate++) {
        if ((ids[candidate] == frameId) && (members[candidate] < 9u)) {
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
        static const uint32_t vertFrameOffsets[7] = { 0u, 64u, 76u, 80u, 96u, 112u, 116u };
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
            if (count == 7u) {
                frameId = ids[candidate];
                frameFound = true;
            }
        }
        xud_check(frameFound, "light/spv-vert-frame-struct", "the vertex module carries the seven-member prefix block");
        for (candidate = 0u; candidate < decorations; candidate++) {
            if ((ids[candidate] == frameId) && (members[candidate] < 7u)) {
                xud_check(offsets[candidate] == vertFrameOffsets[members[candidate]], "light/spv-vert-frame-offset",
                    "and its matrix, camera, time, fog, ambient and counters sit where the C row puts them");
            }
        }
    }
}

void xud_test_light(void) {
    xud_test_light_validate();
    xud_test_light_row();
    xud_test_light_spv();
    if (xud_gpu_tests_enabled()) {
        xud_test_light_gpu();
    }
}
