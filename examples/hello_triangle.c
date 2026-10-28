/***********************************************************************************************
 *
 *   hello_triangle.c - the smallest program that draws something
 *
 *   Owns : one context, one frame loop, one triangle and one teardown. Nothing else - no flag
 *          parsing, no clock, no camera math, no texture, no model: the point of this example is
 *          the public drawing API, and the public drawing API is the frame pair and one shape.
 *   Layer: examples. Public API only (xud.h) plus stdio for the lines it prints. That is the
 *          whole point of the public header: user code names a context, a frame boundary and a
 *          triangle, and never a VkDevice, a swapchain, a pipeline, a batch, a buffer or a fence.
 *
 *   What the frame does, in the order the calls state it: xud_init_context() brings the whole
 *   ladder up (the loader, a GLFW window, the instance, the physical device and its caps, the
 *   device and its two role queues, the surface inside the window, the swapchain for that surface,
 *   the allocator, the descriptor, the pipeline pair, the render pass, the framebuffer rows, the
 *   frame loop and the batch); xud_begin_drawing() opens the frame boundary; xud_draw_triangle_3d()
 *   appends the shape's three vertices and three indices into the batch; xud_end_drawing() closes
 *   the boundary, which is what records the batch's run, acquires an image, records the command
 *   buffer, uploads the live bytes, binds the streams, draws the row through the pipeline the bind
 *   selected, submits, presents and waits that image's fence. The loop runs XUD_HELLO_FRAMES
 *   frames so the window stays open long enough to see the triangle, and the three points are
 *   clip-space coordinates: the frame's viewProj and this draw's model matrix are both identity
 *   today, so the vertex shader answers with the position it read out of the POS stream unchanged.
 *
 *   Copyright (c) 2026 xudlibc authors.
 *
 ***********************************************************************************************/

#include <stdio.h>

#include "xud.h"

/* The window's stay-open frame count - enough frames that the triangle is on screen for a moment
   before the teardown closes the window, and no feature of the library's own.
*/
#define XUD_HELLO_FRAMES 3000u

int main(void) {
    XudContext* context = NULL;
    XudResult step = xud_init_context(&context, 480, 320);

    printf("xud_init_context(480, 320) -> %s (xudlibc %s)\n", xud_result_name(step), XUD_VERSION);
    if (step != XUD_OK) {
        return (step == XUD_UNSUPPORTED) ? 0 : 1;
    }

    /* The three points are the shape's own, in the space the identity viewProj and the identity
       model matrix land a draw in, and the colour is the batch's COLOR stream read as its own
       R,G,B,A bytes.
    */
    const XudVector3 v0 = (XudVector3) { .x = -0.8f, .y = -0.8f, .z = 0.0f };
    const XudVector3 v1 = (XudVector3) { .x = 0.8f, .y = -0.8f, .z = 0.0f };
    const XudVector3 v2 = (XudVector3) { .x = 0.0f, .y = 0.8f, .z = 0.0f };
    const XudColor red = (XudColor) { .r = 230u, .g = 40u, .b = 40u, .a = 255u };

    for (uint32_t frame = 0u; (frame < XUD_HELLO_FRAMES) && (step == XUD_OK); frame++) {
        step = xud_begin_drawing(context);
        if (step == XUD_OK) {
            step = xud_draw_triangle_3d(context, v0, v1, v2, red);
        }
        if (step == XUD_OK) {
            step = xud_end_drawing(context);
        }
        if (step != XUD_OK) {
            printf("frame %u -> %s\n", frame, xud_result_name(step));
        }
    }

    const XudResult closed = xud_close_context(&context);

    printf("xud_close_context -> %s\n", xud_result_name(closed));

    return ((step != XUD_OK) || (closed != XUD_OK)) ? 1 : 0;
}
