/***********************************************************************************************
 *
 *   hello_triangle.c - the smallest program that draws something
 *
 *   Owns : one context, one frame boundary, one triangle and one teardown. Nothing else - no inp
 *          ut, no camera math, no texture, no model: the point of this example is the public draw
 *          ing API, and the public drawing API is the frame pair and one shape.
 *   Layer: examples. Public API only (xud.h). That is the whole point of the public header: user
 *          code names a context, a frame boundary and a triangle, and never a VkDevice, a swapcha
 *          in, a pipeline, a batch, a buffer or a fence.
 *
 *   What the frame does, in the order the calls state it: xud_init_context() brings the whole lad
 *          der up (the loader, a GLFW window, the VkInstance carrying that window's required insta
 *          nce extensions, the debug messenger, the physical layer's caps, the VkDevice and its tw
 *          o role queues, the VkSurfaceKHR inside the window, the VkSwapchainKHR for that surface,
 *          the GPU-memory allocator, the descriptor layout, the pipeline pair, the render pass, th
 *          e framebuffer rows, the frame loop and the batch); xud_begin_drawing() opens the frame
 *          boundary; xud_draw_triangle_3d() appends the shape's three vertices and three indices i
 *          nto the batch; xud_end_drawing() closes the boundary, which is what records the batch's
 *          run, acquires an image, records the command buffer, uploads the live bytes, binds the
 *          streams, draws the row through the pipeline the bind selected, submits, presents and em
 *          pties the batch; xud_close_context() tears all of it back in the reverse order.
 *
 *   The triangle's coordinates are the shape's own, and they are already clip-space coordinates:
 *          the frame's viewProj and this draw's model matrix are both identity today, so the vertex
 *          shader answers with the position it read out of the POS stream unchanged. A triangle at
 *          +/-0.8 therefore covers most of the window on any extent, which is what makes this exam
 *          ple's verdict visible rather than a pixel no one can find. The camera + transform step
 *          is what will make that an ordinary world-space triangle instead, and it is the step abo
 *          ve this one - see todo.md.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <stdio.h>

#include "xud.h"

int main(void) {
    const XudResult supported = xud_vulkan_supported();
    printf("xud_vulkan_supported() -> %s\n", xud_result_name(supported));

    XudContext* context = NULL;
    const XudResult result = xud_init_context(&context, 480, 320);

    printf("xud_init_context(480, 320) -> %s (xudlibc %s)\n", xud_result_name(result), XUD_VERSION);

    if (result != XUD_OK) {
        /* XUD_UNSUPPORTED is the honest state on a machine with no Vulkan loader or no window sys
           tem; anything else is a real failure. On a machine that has both, the call above answer
           ed XUD_OK and the context below is the one that owns everything.
         */
        return result == XUD_UNSUPPORTED ? 0 : 1;
    }

    /* The triangle, in the order the caller names it. The three points are counter-clockwise in
       raylib's own wording of DrawTriangle3D, and the library appends them in that order with a
       flat +Z normal and a zero uv - the two constant attribute rows xud_shapes.h states. The
       colour is the batch's COLOR stream's byte order, so the red below is the stream's bytes.
     */
    const XudVector3 v0 = (XudVector3) { .x = -0.8f, .y = -0.8f, .z = 0.0f };
    const XudVector3 v1 = (XudVector3) { .x = 0.8f, .y = -0.8f, .z = 0.0f };
    const XudVector3 v2 = (XudVector3) { .x = 0.0f, .y = 0.8f, .z = 0.0f };
    const XudColor red = (XudColor) { .r = 230u, .g = 40u, .b = 40u, .a = 255u };

    /* Three frames, so the loop is visible: the first frame's batch is empty at open, the frame
       that completed resets it, and the next frame's begin_drawing() opens a boundary on an emp
       ty batch again. Nothing here is a state machine - the library has no frame counter and no
       notion of a frame outside the one call xud_end_drawing() runs.
     */
    for (uint32_t frame = 0u; frame < 3u; frame++) {
        XudResult step = xud_begin_drawing(context);

        if (step != XUD_OK) {
            printf("frame %u: xud_begin_drawing -> %s\n", (unsigned)frame, xud_result_name(step));
            (void)xud_close_context(&context);
            return 1;
        }

        step = xud_draw_triangle_3d(context, v0, v1, v2, red);

        if (step != XUD_OK) {
            printf("frame %u: xud_draw_triangle_3d -> %s\n", (unsigned)frame, xud_result_name(step));
            (void)xud_close_context(&context);
            return 1;
        }

        step = xud_end_drawing(context);

        printf("frame %u: xud_begin_drawing/xud_draw_triangle_3d/xud_end_drawing -> %s\n", (unsigned)frame, xud_result_name(step));

        if (step != XUD_OK) {
            (void)xud_close_context(&context);
            return 1;
        }
    }

    const XudResult closed = xud_close_context(&context);

    printf("xud_close_context -> %s\n", xud_result_name(closed));

    return closed == XUD_OK ? 0 : 1;
}
