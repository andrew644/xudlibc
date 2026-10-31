/***********************************************************************************************
 *
 *   xud_test_harness.h - the one harness every test area shares
 *
 *   Owns : the run's two counters, the one named check, the GPU gate, and the list of area
 *          entry points the runner calls. Nothing else - no test of its own.
 *   Layer: tests. Every tests/xud_test_<area>.c includes this and defines exactly one of the
 *          entry points declared below; tests/xud_test_main.c is the runner and the only place
 *          that reads the counters. The area files never call each other: one area's GPU half
 *          is gated inside that area's entry point, so the runner stays one line per area and
 *          the headless run stays green with no driver present.
 *          The counters are extern here and defined once in xud_test_harness.c, which is what
 *          makes the split one run rather than sixteen: the check that used to be a local pair
 *          in one file is now shared state with one owner.
 *
 *   Copyright (c) 2026 xudlibc authors.
 ***********************************************************************************************/

#ifndef XUD_TEST_HARNESS_H_
#define XUD_TEST_HARNESS_H_

/* The run's counters, defined once in xud_test_harness.c. Every area increments them through
   xud_check(), and only the runner reads them for the exit code. */
extern int xudChecks;
extern int xudFailures;

/* The one named check of the whole run: a named condition and the detail line that explains a
   failure. */
void xud_check(int condition, const char* name, const char* detail);

/* The GPU gate: 1 when the driver-backed run is wanted, 0 for the headless default. Read once
   per area entry point, never per check. */
int xud_gpu_tests_enabled(void);

/* The area entry points, in the order the runner calls them: bottom of the layer ladder up.
   Each one runs its own pure half and, when the gate answers 1, its own GPU half. */
void xud_test_vocabulary(void);
void xud_test_loader(void);
void xud_test_diagnostics(void);
void xud_test_instance(void);
void xud_test_physical(void);
void xud_test_device(void);
void xud_test_gpu_bringup(void);
void xud_test_window(void);
void xud_test_swapchain(void);
void xud_test_handle(void);
void xud_test_shader(void);
void xud_test_pipeline(void);
void xud_test_renderpass(void);
void xud_test_frame(void);
void xud_test_renderer(void); /* the renderer rung's resize: the recreate ask, the framebuffer extent, the rebuild */
void xud_test_allocator(void);
void xud_test_descriptor(void);
void xud_test_batch(void);
void xud_test_shapes(void); /* the shapes rung and the public drawing API: the atomic append, the frame pair */
void xud_test_camera(void); /* the camera rung and the public camera pair: the basis, the frustum, the matrix's geometry */
void xud_test_core(void);

#endif /* XUD_TEST_HARNESS_H_ */
