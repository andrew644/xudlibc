/***********************************************************************************************
 *   xud_frame.h - the frame loop: one command pool, one command buffer, the acquire/submit/p
 *   resent triple and the semaphores + per-image fences that bound it
 *
 *   Layer: above the render pass and the framebuffer array, below the public API. This is the
 *   rung the plan's frame loop is written against, read literally:
 *
 *     BeginDrawing: acquire image (semaphore) -> begin cmd -> begin render pass -> clear
 *     EndDrawing  : end pass -> end cmd -> submit (graphics) -> present (queue) -> fence
 *
 *   and the synchronization rule the repo settles: acquire/present semaphores for frame bounda
 *   ries, ONE VkFence PER SWAPCHAIN IMAGE, classic synchronization, no VK_KHR_vulkan_memory_mode
 *   l anywhere.
 *
 *   The frame loop is what CONSUMES the batch and the descriptor now: it closes the batch's pending ru
 *   n into a draw row before the command buffer is begun, writes set 0's block into the descriptor's m
 *   apped arena, uploads the batch's live bytes with the five vkCmdCopyBuffer calls and the block's by
 *   tes with the descriptor rung's ONE vkCmdCopyBuffer call, binds the batch's four streams with ONE v
 *   kCmdBindVertexBuffers call, its index buffer with one vkCmdBindIndexBuffer call and set 0 with one
 *   vkCmdBindDescriptorSets call against the pipeline's own layout, binds the variant's pipeline row
 *   beside each of the batch's recorded draw rows and replays those rows as vkCmdDrawIndexed calls, and
 *   resets the batch at the end of a frame that completed - so the
 *   geometry is the batch's and the matrix the shader places it by is the block's, and no longer a har
 *   d-coded three-vertex triangle placed by nothing at all. A batch with nothing in it is a frame tha
 *   t clears and presents with no draw at all, which is a legal frame rather than an error; the block i
 *   s uploaded every frame whether or not the batch has content, because the shader reads it.
 *
 *   The staging rule the repo settles is why the buffer barriers below are here: the batch's arena and
 *   the descriptor's uniform arena are BOTH mapped ONCE for the life of their owner, so the host writ
 *   es into them are not a map/unmap pair per frame and the thing that makes them visible to the copies
 *   is ONE HOST_WRITE -> TRANSFER_READ barrier covering both arenas' writes, and the thing that makes
 *   the copies' results visible to the reads that follow them is TWO TRANSFER_WRITE barriers rather th
 *   an one: TRANSFER_WRITE -> VERTEX_ATTRIBUTE_READ for the streams, whose dstStageMask is the input a
 *   nd vertex stages, and TRANSFER_WRITE -> UNIFORM_READ for the block, whose dstStageMask is the two s
 *   hader stages the layout declares for it. One barrier naming both access bits with one stage mask i
 *   s refused by the spec's supported-access-type rule, which is the reason for the split. All three s
 *   it BEFORE vkCmdBeginRenderPass, outside any render pass instance, which is what the rule that a ba
 *   rrier inside a pass must name the bound pipeline's stages does not reach - and vkCmdPipelineBarrie
 *   r is not legal inside a pass at all.
 *
 *   The fence array is per IMAGE, not per frame, and that is the point of the plan's wording: a
 *   fence that bounded a frame would say nothing about which image the frame wrote, and the thing
 *   that has to be true before an image is written again is that the presentation of THAT image
 *   finished.
 ***********************************************************************************************/

#ifndef XUD_FRAME_H_
#define XUD_FRAME_H_

#include "volk.h"
#include "xud_batch.h" /* Required for: XudBatch, the four SoA streams this rung uploads, binds and draws */
#include "xud_descriptor.h" /* Required for: XudDescriptor, set 0's block this rung uploads and binds */
#include "xud_device.h"
#include "xud_loader.h"
#include "xud_physical.h"
#include "xud_pipeline.h"
#include "xud_renderpass.h"
#include "xud_swapchain.h"
#include "xud_vulkan.h"
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

/*-----------------------------------------------------------------------------------------------
 * The rung's own constants
 *-----------------------------------------------------------------------------------------------*/

#define XUD_FRAME_COMMAND_BUFFER_COUNT                                                                                                     \
    1u /* ONE command buffer for the whole rung: the plan's                                                                                \
          "one command buffer", and the reason it is one and                                                                               \
          not one-per-image is that the buffer is recorded and                                                                             \
          submitted inside one frame here - a per-image buffer                                                                             \
          would be the pipelined frame loop the plan does not                                                                              \
          ask for yet */
#define XUD_FRAME_SUBMIT_COUNT                                                                                                             \
    1u /* one VkSubmitInfo per submit: the plan's one batch flushed                                                                        \
          per frame, and the batch layer that would make this more th                                                                      \
          an one is not in the tree */
#define XUD_FRAME_FIRST_BINDING                                                                                                            \
    0u /* vkCmdBindVertexBuffers' firstBinding: the batch's four streams are bindings 0..3, the same                                       \
          numbers xud_batch_fill_binding writes and the same ones the pipeline's vertex-input row                                          \
          names, so ONE call binds the whole array rather than four calls that would each leave a                                          \
          binding of the pipeline's four unbound */
#define XUD_FRAME_BARRIER_HOST_STAGE                                                                                                       \
    VK_PIPELINE_STAGE_HOST_BIT /* the srcStageMask of the barrier that makes the batch's host writes                                       \
                                  into its mapped arena visible to the copies below: HOST is the one                                       \
                                  stage the spec's table puts VK_ACCESS_HOST_WRITE_BIT at, and it is                                       \
                                  legal in a srcStageMask and never in a dstStageMask - which is why                                       \
                                  this barrier is a separate call from the layout-transition one below                                     \
                                  rather than a third entry added to that call's masks */
#define XUD_FRAME_BARRIER_TRANSFER_STAGE                                                                                                   \
    VK_PIPELINE_STAGE_TRANSFER_BIT /* the dstStageMask of that first barrier AND the srcStageMask of                                       \
                                     the second one, which is the copy the upload issues: TRANSFER is                                      \
                                     the one stage both VK_ACCESS_TRANSFER_READ_BIT and VK_ACCESS_TRANSFER_WRITE_BIT sit at */
#define XUD_FRAME_BARRIER_VERTEX_READ_STAGES                                                                                               \
    (VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) /* the dstStageMask \
          of the barrier that makes the copies visible to the reads that follow them: the batch's bytes                                    \
          are read as vertex attributes at the input stage and as uniforms at the shader stages, and the                                   \
          validation layer's supported-access-type rule is what refuses a mask that covers neither */
#define XUD_FRAME_HOST_WRITE_ACCESS VK_ACCESS_HOST_WRITE_BIT /* the srcAccessMask of the barrier over the batch's host writes */
#define XUD_FRAME_TRANSFER_READ_ACCESS VK_ACCESS_TRANSFER_READ_BIT /* the dstAccessMask of that same barrier: the copies read the arena */
#define XUD_FRAME_TRANSFER_WRITE_ACCESS VK_ACCESS_TRANSFER_WRITE_BIT /* the srcAccessMask of the barrier over the copies' results */
#define XUD_FRAME_VERTEX_READ_ACCESS                                                                                                       \
    VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT /* the dstAccessMask of that same barrier: the draws read the                                      \
                                          streams as vertex attributes */
#define XUD_FRAME_WAIT_STAGE                                                                                                               \
    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT /* the stage the acquire semapho                                                                   \
                                           re is waited AT: the first stag                                                                 \
                                           e of the submit that reads the i                                                                \
                                           mage. The repo's rule is that a                                                                 \
                                           wait names the stage it bounds                                                                  \
                                           rather than relying on an impli                                                                 \
                                           cit one, and with no vertex inp                                                                 \
                                           ut stage in this pipeline the                                                                   \
                                           vertex shader IS that first s                                                                   \
                                           tage */
#define XUD_FRAME_BARRIER_STAGES                                                                                                           \
    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT /* both halves of the layout-transition barrier's stage masks. ALL_GRAPHICS rather than the         \
                                          pipeline's two shader stages, because the access flags this barrier names are a                  \
                                          colour-attachment READ and a WRITE, and the spec's table of supported access types puts the      \
                                          write at the fragment-test stages, which the two shader stages alone do not cover - the          \
                                          validation layer reports a VERTEX|FRAGMENT mask as not supporting the write bit, probed today.   \
                                          The barrier sits BEFORE vkCmdBeginRenderPass, outside any render pass instance, so the rule that \
                                          a barrier's srcStageMask must be a subset of the bound pipeline's stages does not reach it */
#define XUD_FRAME_NO_TIMEOUT                                                                                                               \
    (~0ull) /* the all-ones 64-bit value, which is what vkAcquireNextImageKHR's                                                            \
              timeout means by                                                                                                             \
              "wait as long as the compositor or the semaphore takes": a                                                                   \
              timeout of 0 would be a wait that returns without waiting,                                                                   \
              which is the opposite of the boundary this rung exists to                                                                    \
              enforce */
#define XUD_FRAME_INDEX_TYPE                                                                                                               \
    VK_INDEX_TYPE_UINT32 /* vkCmdBindIndexBuffer's indexType: the batch's index buffer holds uint32_t                                      \
                            entries, XUD_BATCH_INDEX_SIZE is 4 bytes an entry, so UINT32 is the only type                                  \
                            that names them - UINT16 would be a bind reading half an index and UINT8 a                                     \
                            bind reading a quarter of one */
#define XUD_FRAME_FIRST_SET                                                                                                                \
    0u /* vkCmdBindDescriptorSets' firstSet: set 0 is the per-frame set of the design sketch and this                                      \
          rung binds that one set, so the first set bound is 0 and the count below is 1 - the pair is                                      \
          the unambiguous form of "set 0 only" rather than a bit pattern to reason about */

#define XUD_FRAME_SET_COUNT                                                                                                                \
    1u /* how many sets one vkCmdBindDescriptorSets call binds: set 0 today, and set 1 is the texture                                      \
          step's to allocate and bind, which is why the pool above is sized for both */

#define XUD_FRAME_UNIFORM_READ_STAGES                                                                                                      \
    (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) /* the dstStageMask                                      \
          of the barrier that makes the block's upload visible to the stages that read it: this is XU_                                     \
          DESCRIPTOR_BUFFER_STAGES, the pair the layout declares for the block, and a stage that does                                      \
          not read it is not a stage the barrier has to cover */

#define XUD_FRAME_UNIFORM_READ_ACCESS                                                                                                      \
    VK_ACCESS_UNIFORM_READ_BIT /* the dstAccessMask of that same barrier: the block is read as a unifo                                     \
          rm buffer, and the spec's supported-access-type rule is what refuses a barrier that names th                                     \
          is access at a stage that cannot read a uniform - which is why this barrier is a separate ca                                     \
          ll from the vertex-attribute one above rather than two access bits crammed into one mask, wh                                     \
          ere the rule would refuse the stage pair the other mask names */
#define XUD_FRAME_WAIT_ALL                                                                                                                 \
    1u /* vkWaitForFences' waitAll argument: every fence in the list has t                                                                 \
         o be signalled, and the list is one fence, so the pair is the un                                                                  \
         ambiguous form rather than a bit pattern to reason about */

static_assert(XUD_FRAME_COMMAND_BUFFER_COUNT == 1u, "one command buffer, per the plan");
static_assert(XUD_FRAME_SUBMIT_COUNT == 1u, "one submit per frame, per the plan");
static_assert(XUD_FRAME_FIRST_BINDING == 0u, "the batch's four streams are bindings 0..3");
static_assert(XUD_FRAME_VERTEX_READ_ACCESS == VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, "the streams are read as vertex attributes");
static_assert(XUD_FRAME_INDEX_TYPE == VK_INDEX_TYPE_UINT32, "the batch's indices are uint32_t entries");
static_assert(XUD_FRAME_FIRST_SET == 0u, "set 0 is the per-frame set and it is the first set bound");
static_assert(XUD_FRAME_SET_COUNT == 1u, "one set bound per frame, which is set 0 and its block");
static_assert(XUD_FRAME_UNIFORM_READ_ACCESS == VK_ACCESS_UNIFORM_READ_BIT, "the block is read as a uniform buffer");
static_assert(XUD_FRAME_WAIT_STAGE == VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, "the first stage that reads the image");
static_assert(XUD_FRAME_NO_TIMEOUT == (~0ull), "the all-ones value is the no-timeout answer");
static_assert(VK_COMMAND_BUFFER_LEVEL_PRIMARY == 0u, "the primary level is what this rung records");
static_assert(VK_SUBPASS_CONTENTS_INLINE == 0u, "no secondary command buffers exist in this tree");
static_assert(XUD_FRAME_WAIT_ALL == 1u, "waitAll is the unambiguous form for a one-fence list");
static_assert(XUD_FRAME_BARRIER_STAGES == VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
    "every graphics stage, so the colour-attachment write bit is supported");

/*-----------------------------------------------------------------------------------------------
 * The frame loop's objects, and what each was created with
 *-----------------------------------------------------------------------------------------------*/

typedef struct XudFrame XudFrame;
struct XudFrame {
    VkCommandPool commandPool; /* the one pool this rung owns */
    VkCommandBuffer commandBuffer; /* allocated from it, once, and re-recorded every frame */
    VkSemaphore imageAcquired; /* the acquire semaphore: acquire signals it, the submit waits on
                                  it before anything reads the image. ONE semaphore, reused every
                                  frame, and the reason that is legal is that the frame loop waits
                                  it before the next acquire - a second acquire while one is still
                                  unwaited would be two signals on a semaphore nobody waited */
    VkFence* imageFences; /* the plan's "one VkFence per swapchain image", heap array, index-cor
                            respondent with the swapchain's image array - the fence that bounds a
                            frame is the fence of the image that frame wrote, which is what makes
                            "this image may be written again" the question the fence answers */
    uint32_t imageFenceCount; /* how many fences are live - always the swapchain's imageCount */
    uint32_t imageFenceCapacity; /* fences allocated for it, allocated exactly rather than doubled
                                    (the count comes from a query that has an upper limit, the sam
                                    e rule xud_framebuffer_array_open() states) */
    VkImageLayout* imageLayouts; /* the layout each swapchain image is in as this rung LEFT it, index-correspondent with the fence array. It
                                    is state, not decoration: the render pass this rung begins requires its attachment to be in the
                                    attachment's initialLayout from the first sample, and the spec requires the image to have been in that
                                    layout when the pass begins, so a barrier has to run before the pass and a barrier names the layout the
                                    image is in NOW. The array is per image because acquire chooses the image, not this rung. */
    uint32_t imageLayoutCount; /* how many layout rows are live - always the swapchain's imageCount, allocated exactly by the same rule as
                                  the fence array above */
    VkImageLayout depthLayout; /* the layout the pass's ONE depth image is in as this rung LEFT it, same
                                  shape as the array above but one field rather than an array: the colour
                                  images are one per swapchain image and acquire chooses among them, whi
                                  le the depth image is one image the pass owns and every framebuffer ro
                                  w attaches the same view of. It starts at VK_IMAGE_LAYOUT_UNDEFINED, th
                                  e layout xud_renderpass_fill_depth_info() declares the image created in
                                  , and the first frame's barrier is what moves it out of that layout an
                                  d every later frame's is a no-op because the pass's initialLayout and f
                                  inalLayout for a depth attachment are the same layout - a depth attachm
                                  ent is never presented, so it has no PRESENT_SRC_KHR half to move back
                                  out to. */
    VkFence lastFrameFence; /* borrowed: which fence the last xud_frame_draw() signalled, VK_NULL_
                               HANDLE when no frame is in flight. The wait-then-reset pair at the
                               end of a frame is what keeps every fence in the not-signalled state
                               the NEXT submit requires, and recording which one it was is what ma
                               kes that pair testable rather than a claim */

    /* --- the create-infos and the rows the driver calls were handed, kept beside the handles the
       same way every layer above keeps its own - the rule xud_device.c states for a queue row poin
       ting at the struct's own priorities array, one rung up --- */
    VkSemaphoreCreateInfo semaphoreInfo; /* what vkCreateSemaphore was handed - the empty form,
                                            VkSemaphoreCreateInfo has nothing else to say */
    VkFenceCreateInfo fenceInfo; /* what every vkCreateFence was handed - the empty form, and the
                                   reason VK_FENCE_CREATE_SIGNALED_BIT is NOT set: a fence that sta
                                   rted already signalled is the opposite of a fence that has to bo
                                   und a submission */
    VkCommandPoolCreateInfo poolInfo; /* what vkCreateCommandPool was handed */
    VkCommandBufferAllocateInfo allocationInfo; /* what vkAllocateCommandBuffers was handed */
    VkCommandBufferBeginInfo beginInfo; /* what vkBeginCommandBuffer was handed, every frame */
    VkSubmitInfo submitInfo; /* what vkQueueSubmit was handed, every frame */
    VkPresentInfoKHR presentInfo; /* what vkQueuePresentKHR was handed, every frame */
    VkPipelineStageFlags waitStages[XUD_FRAME_SUBMIT_COUNT]; /* the pWaitDstStageMask row of the
                                                                submit: the stage the acquire semaph
                                                                ore is waited AT, kept in the struct
                                                                because the submit row points at it
                                                                and must not point at a frame-local
                                                                array that has gone out of scope */
    VkCommandBuffer commandBufferRows[XUD_FRAME_COMMAND_BUFFER_COUNT]; /* the array vkAllocateComm
                                                                         andBuffers was handed and v
                                                                         kFreeCommandBuffers hands b
                                                                         ack (VkCommandBuffer is alre
                                                                         ady a pointer typedef, so th
                                                                         e array is of the handle its
                                                                         elf, not of a pointer to it) */
    VkSwapchainKHR presentedSwapchain; /* BORROWED: the swapchain the last present named. The field
                                         is not decoration - xud_frame_draw() compares it against th
                                         e swapchain in hand, which is what makes a recreate that rep
                                         laced the swapchain (and therefore every image, every frameb
                                         uffer and every fence index) a caught error rather than a pr
                                         esentation against a dead handle */
    uint32_t lastImageIndex; /* the index vkAcquireNextImageKHR wrote, kept for the present */
    uint32_t framesDrawn; /* how many frames completed - the counter a test can read to know the lo
                            op ran more than once, which is what exercises a reused command buffer a
                            nd a reused acquire semaphore rather than asserting about them */
};

/*-----------------------------------------------------------------------------------------------
 * The layer's contract
 *-----------------------------------------------------------------------------------------------*/

/// Pure: the command pool's create-info. flags VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
/// which is the one flag this rung needs because the command buffer is re-recorded every frame,
/// and the pool without the flag cannot hand one out that may be re-recorded. queueFamilyIndex is
/// the physical layer's CHOSEN family (the same rule xud_swapchain_fill_create_info() states for t
/// he queue-family row: the physical layer already chose one family and the device layer already cr
/// eated a queue from it), and XUD_PHYSICAL_NO_QUEUE_FAMILY - "never chosen" - is a bad argument. X
/// UD_INVALID_ARG on a NULL out-create-info or on that sentinel.
XudResult xud_frame_fill_pool_info(VkCommandPoolCreateInfo* outCreateInfo, uint32_t queueFamilyIndex);

/// Pure: the command-buffer allocation for one pool handle. XUD_INVALID_ARG on a NULL out-create
/// -info; XUD_UNSUPPORTED on a handle-less pool (there is nothing to allocate from).
XudResult xud_frame_fill_allocation(VkCommandBufferAllocateInfo* outAllocation, VkCommandPool commandPool);

/// Pure: the begin-info, the viewport and the scissor for one extent. The extent is the swapchain's
/// or the framebuffer row's, copied rather than re-queried (xud_swapchain.c's note about the surf
/// ace's "undefined" currentExtent), and a zero extent is refused here rather than recorded: a vie
/// wport of 0x0 is a viewport that draws nothing, which is a silent no-op rather than an error. XUD
/// _INVALID_ARG on a NULL out-param or a zero extent.
XudResult xud_frame_fill_recording(
    VkCommandBufferBeginInfo* outBegin, VkViewport* outViewport, VkRect2D* outScissor, uint32_t width, uint32_t height);

/// Pure: the submit row and the present row for one frame. The submit waits the acquire semaphore
/// at the vertex stage and signals nothing itself (the fence is vkQueueSubmit's own argument, not a
/// row of this struct); the present names the swapchain and leaves its wait-semaphore pair empty, b
/// ecause the fence at the end of the frame is what bounds the presentation and a wait semaphore the
/// re would be a second boundary for the same thing. There is NO host-side semaphore-wait row in th
/// is contract: vkWaitSemaphores is a core-1.2 entry point and this library's instance sits at the
/// core-1.0 floor (XUD_INSTANCE_API_FLOOR), so the acquire gate is the submit's pWaitSemaphores + pW
/// aitDstStageMask pair, which is the core-1.0 form of the same boundary and lives on the queue rat
/// her than on the host. XUD_INVALID_ARG on a NULL out-param or a handle-less command buffer / swa
/// tchain.
XudResult xud_frame_fill_submit(
    VkSubmitInfo* outSubmit, VkPresentInfoKHR* outPresent, VkCommandBuffer commandBuffer, VkSwapchainKHR swapchain);

/// Pure: the push-constant row of ONE draw, from the batch's draw row. The model matrix is the iden
/// tity for the same reason the block's viewProj is: a non-identity matrix is a transform the step a
/// bove this one has to choose, and it has not landed - what is settled here is the CHANNEL (the row
/// is pushed, the vertex stage reads it, the step above hands its own matrix in without this shape c
/// hanging). materialIndex is the draw row's own, with the batch's XUD_BATCH_NO_MATERIAL sentinel map
/// ped to slot 0 because the push row's member is an index a read could use and the sentinel is 0xFF
/// FFFFF, which indexes nothing; textureIndex is 0 because set 1 is declared and never allocated, so
/// there is no sampler array with an entry in it; flags is the variant the draw was flushed under, in
/// the per-draw channel rather than a channel of this rung's - the pipeline variant is chosen by BIND
/// ING, and this is the same fact from the shader's side, which is what lets a stage branch on the va
/// riant without a third pipeline. The vertex stage reads only the model matrix today, so the other m
/// embers are recorded and unread: the row is the channel and a stage need not read every member of a
/// block its layout declares. XUD_INVALID_ARG on a NULL out-row or a NULL draw row.
XudResult xud_frame_fill_push_row(XudPushRow* outRow, const XudBatchDraw* drawRow);

/// Driver: the whole bring-up of this rung - vkCreateCommandPool, vkAllocateCommandBuffers, vkCre
/// ateSemaphore, and vkCreateFence once per swapchain image. Before any call: the loader is open,
/// its device rows are this device's rows, and EVERY destroy row this teardown needs is checked BE
/// FORE the create rows it would otherwise leak (the rule xud_window.c and xud_swapchain.c state fo
/// r a surface and a swapchain, one rung below). On XUD_OK *outFrame owns a pool, a command buffer,
/// a semaphore and imageCount fences, every create-info above says what each was created with, and
/// the wait-stage row names the vertex stage (the stage the acquire semaphore is waited AT, which i
/// s where the draw's work begins). On any error it stays NULL and everything created so far is des
/// troyed in reverse order. XUD_INVALID_ARG on a NULL out-pointer or one that already owns a frame,
/// a NULL loader/device/swapchain, a handle-less device, or a swapchain with no handle or no images
/// ; XUD_UNSUPPORTED when the loader is closed, its rows are not this device's, or a row this rung
/// needs is nil.
XudResult xud_frame_open(XudFrame** outFrame, XudLoader* loader, const XudDevice* device, const XudSwapchain* swapchain);

/// Driver: one frame, end to end - acquire, reset + record the command (begin, viewport, scissor, bind pipeline, begin render pass with the
/// pass's clear values, draw, end pass, end), submit to the graphics queue with the image's own fence, present on the pr esent queue, then
/// wait that fence and reset it so the NEXT submit starts from the not-signalled state the spec requires. The framebuffer is picked by the
/// index acquire wrote, through the array accessor, so a recreate that replaced the array is a caught error rather than a stale row (the r
/// epo's accessor rule, one rung below). On XUD_OK *outImageIndex is the index that was presented a
/// nd frame->framesDrawn has bumped; on any error the frame is left in the state it was in before t
/// he acquire, and a command buffer that was begun is always ended before the rung returns - the Cm
/// d* calls this rung makes in between are void and cannot fail, so vkEndCommandBuffer is the only
/// point a recording can fail at, which is what makes that claim checkable rather than a promise. X
/// UD_INVALID_ARG on a NULL frame/loader/device/pass/array/out-index, a handle-less device, render
/// pass, pipeline, framebuffer row or swapchain, or an acquire answer at or above the swapchain's i
/// mage count; XUD_UNSUPPORTED when the loader is closed, its rows are not this device's, a row thi
/// s rung needs is nil, or the fence / framebuffer arrays no longer match the swapchain in hand (th
/// e plan's "the framebuffer array is recreated with the swapchain" rule, read here as a refusal ra
/// ther than as a rebuild this rung cannot do without a resize to act on); XUD_VULKAN_ERROR when th
/// e driver refused the acquire, the submit, the present or a wait (an out-of-date acquire answer m
/// aps here, and the recreate path is the resize step's, not this rung's).
/// The batch is the argument after the framebuffer array for one reason: the frame loop is what
/// consumes it. It uploads the batch's live bytes with the batch rung's five vkCmdCopyBuffer calls,
/// binds the batch's four streams with ONE vkCmdBindVertexBuffers call over the whole array and its
/// index buffer with one vkCmdBindIndexBuffer call (vkCmdDrawIndexed names no index buffer at all -
/// the bind is the state and the draw reads it), and
/// replays the batch's recorded draw rows as vkCmdDrawIndexed calls - so the geometry is the batch's
/// and no longer a hard-coded three-vertex triangle. A batch with nothing in it is a frame that clears
/// and presents with no draw at all, which is a legal frame rather than an error. The frame loop also
/// closes the batch's pending row into the draw list and resets the batch at the end of a frame that
/// completed, which is what makes "the frame loop is the flush" true rather than a promise: the next
/// frame starts from an empty batch and the arena's bytes are rewritten in place, never appended to.
/// The descriptor is the argument after the batch for the same reason one rung over: the frame loop i
/// s what updates set 0. It writes the block's bytes into the descriptor's mapped arena, uploads them
/// with the descriptor rung's ONE vkCmdCopyBuffer call, and binds set 0 with one vkCmdBindDescriptorS
/// ets call against the pipeline's own layout - so the block the shader reads is a block that holds s
/// omething rather than a block that was never written. The push row of the same layout is pushed BE
/// SIDE every draw this loop records (xud_frame_fill_push_row builds it from the batch's draw row and
/// the driver half pushes it against the layout's own recorded range), so the vertex is placed by thi
/// s frame's viewProj times this draw's model matrix - the per-draw channel and the per-frame set are
/// both live, and what is still identity in both is the matrices themselves, because a non-identity m
/// atrix is a transform the step above this one has to choose.
XudResult xud_frame_draw(XudFrame* frame, XudLoader* loader, const XudDevice* device, const XudSwapchain* swapchain,
    const XudPipeline* pipeline, const XudRenderPass* pass, const XudFramebufferArray* framebuffers, uint32_t framebufferCount,
    XudBatch* batch, XudDescriptor* descriptor, uint32_t* outImageIndex);

/// Driver: retire everything this rung owns, in the one order that is correct - vkWaitForFences on
/// a fence still in flight, then vkFreeCommandBuffers, vkDestroySemaphore, vkDestroyFence for ever
/// y image fence, vkDestroyCommandPool LAST because the command buffer belongs to the pool and the
/// spec requires the pool to outlive every buffer allocated from it. XUD_ALREADY when the pointer o
/// wns no frame; XUD_INVALID_ARG on a NULL pointer-to-pointer or a NULL loader/device; XUD_UNSUPPOR
/// TED when the loader is closed, its rows are not this device's, or a destroy row is nil while the
/// object it would destroy is live (the struct is not freed, because freeing it would leak the hand
/// les silently). A refused teardown is reported, not hidden, and the rungs after it still run - th
/// e rule xud_close_context() states.
XudResult xud_frame_close(XudFrame** ioFrame, const XudDevice* device, XudLoader* loader);

#endif /* XUD_FRAME_H_ */
