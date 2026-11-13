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
 *   The acquire and the present are the only two calls in this tree whose VkResult answers about
 *   the SWAPCHAIN rather than about the call: VK_SUBOPTIMAL_KHR ("works, but not the most optimal
 *   swapchain for this surface") and VK_ERROR_OUT_OF_DATE_KHR ("no longer matches the surface -
 *   create a new one before the NEXT acquire, but the image this call handed back is still valid").
 *   For those two the frame is not refused: the image is acquired, drawn, submitted and presented
 *   as usual, and the rung leaves recreateWanted set for the resize step above to act on. Refusing
 *   them is what turns a window resize into a frame that never draws at all.
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
#include "xud_layout.h" /* Required for: XudLayoutTracker - the context's layout map this rung reads its two opening barriers' oldLayouts from */
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

#define XUD_FRAME_IN_FLIGHT_COUNT                                                                                                          \
    2u /* how many frames the CPU may have on the go at once: one slot per in-flight frame, each with its own command                     \
          buffer, acquire semaphore, render-finished semaphore and fence. The CPU waits the slot's fence before it re-                    \
          records the slot, so frame N+1 records while the GPU is still drawing frame N */
#define XUD_FRAME_COMMAND_BUFFER_COUNT XUD_FRAME_IN_FLIGHT_COUNT /* one command buffer per in-flight slot */
#define XUD_FRAME_SUBMIT_COUNT                                                                                                             \
    1u /* one VkSubmitInfo per submit: the plan's one batch flushed                                                                        \
          per frame, and the batch layer that would make this more th                                                                      \
          an one is not in the tree */
#define XUD_FRAME_SCOPE_MAX                                                                                                                \
    8u /* how many off-screen pass scopes one frame may carry. The bound is the frame's, not            \
          the engine's: live render targets are bounded elsewhere (the sampler array), and a frame      \
          that names more scopes than this is a caller bug, refused rather than recorded wrong */
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
    (VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT                                                              \
        | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) /* the dstStageMask \
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
    1u /* vkCmdBindDescriptorSets' descriptorCount of the per-frame bind, which is a SET count starting                              \
          at firstSet: only set 0, the per-frame block, is bound once per frame here. The slot sets (set 1) are bound                   \
          per draw by xud_frame_draw() rather than in this call, because the set a draw samples depends on that draw's                 \
          texture, and binding the right slot set per draw is what keeps every descriptor written before its set is                    \
          bound - the slot a draw samples moves by a rebind, not by a host-side update of an already-bound set */

#define XUD_FRAME_UNIFORM_READ_STAGES                                                                                                      \
    (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT                                                                                                   \
        | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) /* the dstStageMask                                      \
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

static_assert(XUD_FRAME_IN_FLIGHT_COUNT >= 2u, "two or more frames may be on the go, which is what makes the loop pipelined");
static_assert(XUD_FRAME_SUBMIT_COUNT == 1u, "one submit per frame, per the plan");
static_assert(XUD_FRAME_FIRST_BINDING == 0u, "the batch's four streams are bindings 0..3");
static_assert(XUD_FRAME_VERTEX_READ_ACCESS == VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, "the streams are read as vertex attributes");
static_assert(XUD_FRAME_INDEX_TYPE == VK_INDEX_TYPE_UINT32, "the batch's indices are uint32_t entries");
static_assert(XUD_FRAME_FIRST_SET == 0u, "set 0 is the per-frame set and it is the first set bound");
static_assert(XUD_FRAME_SET_COUNT == 1u,
    "the per-frame bind names set 0 alone, which is the one set whose contents do not depend on a draw's texture; the slot sets are "
    "bound per draw for the reason stated beside XUD_FRAME_SET_COUNT");
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
    VkCommandBuffer commandBufferRows[XUD_FRAME_IN_FLIGHT_COUNT]; /* allocated from it, ONE per in-flight slot: the draw
                                                                       records into the slot its own fence has retired, so a
                                                                       buffer the GPU may still be running is never re-recorded */
    VkCommandBuffer commandBuffer; /* borrowed: commandBufferRows[currentFrame] as of the last draw - the slot the loop
                                       records into, kept as a field so every recording site reads one name */
    VkSemaphore imageAcquired[XUD_FRAME_IN_FLIGHT_COUNT]; /* one acquire semaphore per slot: the acquire signals it and that
                                                              slot's submit waits it before anything reads the image. The slot's
                                                              fence is waited before the slot's next acquire, so the semaphore
                                                              cannot be signalled twice with no wait in between */
    VkSemaphore renderFinished[XUD_FRAME_IN_FLIGHT_COUNT]; /* the submit-to-present boundary, one per slot: the slot's submit
                                                               signals it and the present queue's no-command wait submit waits
                                                               it, so a present cannot run ahead of its render when the two role
                                                               queues are distinct VkQueue handles. The wait submit is enqueued
                                                               before the same slot is next signalled, which is what keeps the
                                                               per-slot reuse legal */
    VkFence frameFences[XUD_FRAME_IN_FLIGHT_COUNT]; /* one fence per in-flight slot, vkQueueSubmit's fence argument: waited
                                                        and reset at the START of the slot's next use rather than at the end
                                                        of its own frame, which is the whole of the CPU/GPU overlap */
    uint32_t frameFenceCount; /* how many fences are live - always XUD_FRAME_IN_FLIGHT_COUNT */
    uint32_t inFlight[XUD_FRAME_IN_FLIGHT_COUNT]; /* 1 once a submit named the slot and before the loop waited that slot's
                                                      fence again: what tells the teardown which fences may still bound work,
                                                      because waiting a fence nothing ever submitted would hang the close */
    uint32_t currentFrame; /* the slot the next xud_frame_draw() records into; advances after every submit */
    XudLayoutTracker* layout; /* NOT owned: the context's layout tracker, held here because the tw
                                 o opening barriers read their oldLayout from the map rather than
                                 from rows this rung used to keep, and because the map's two post
                                 -pass moves - the colour image to its finalLayout, the depth ima
                                 ge to its own - are recorded here, after the submit that runs th
                                 em answered success. The colour image is never UNDEFINED in the
                                 map: the swapchain registers every image it creates, the recreat
                                 e registers the new set and forgets the old, so the frame's firs
                                 t barrier of a fresh image still names a real layout. The depth
                                 image is registered by the pass. */

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
    VkSwapchainKHR presentedSwapchain; /* BORROWED: the swapchain the last present named. The field
                                         is not decoration - xud_frame_draw() compares it against th
                                         e swapchain in hand, which is what makes a recreate that rep
                                         laced the swapchain (and therefore every image, every frameb
                                         uffer and every fence index) a caught error rather than a pr
                                         esentation against a dead handle */
    uint32_t lastImageIndex; /* the index vkAcquireNextImageKHR wrote, kept for the present */
    uint32_t recreateWanted; /* 1 once an acquire or a present answered with the swapchain's two
                                out-of-band codes (VK_SUBOPTIMAL_KHR / VK_ERROR_OUT_OF_DATE_KHR),
                                0 until one does. It is a REQUEST, not an action: this rung cannot
                                recreate - the swapchain is the context's, a rung below, and every
                                framebuffer row and every fence index dies with it - so the rung
                                that observed the answer records it and the resize step above acts
                                on it. Sticky rather than rewritten every frame, because "a recrea
                                te is wanted" stays true until someone acts: a clean acquire answer
                                on the next frame does not un-want the recreate the previous one
                                on it asked for. Written at open rather than left to calloc's ze
                                ro: this is state a caller above reads, not a numeric coincidenc
                                e. */
    uint32_t framesDrawn; /* how many frames completed - the counter a test can read to know the lo
                            op ran more than once, which is what exercises a reused command buffer a
                            nd a reused acquire semaphore rather than asserting about them */
};

/*-----------------------------------------------------------------------------------------------
 * One off-screen pass scope: the triple the loop records BEFORE the swapchain pass, so a render
 * target's image is sampled-ready when the screen pass reads it. The colour and depth images need
 * no rows here: the framebuffer row carries the borrowed colour image and the pass carries its own
 * depth image, and both are read through their owners - the repo's accessor rule, one rung below.
 *-----------------------------------------------------------------------------------------------*/

typedef struct XudFramePassScope {
    XudRenderPass* pass; /* the target's own pass, whose colour finalLayout is SHADER_READ_ONLY -
                            non-const because the renderer writes the frame's background into the
                            row's clear value every frame, the way it writes the screen pass's */
    const XudFramebuffer* framebuffer; /* the target's one framebuffer row */
    XudBatch* batch; /* the target's own batch: the geometry this pass draws */
} XudFramePassScope;

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
/// at the vertex stage and signals the render-finished semaphore itself, and the present waits that
/// same semaphore: the pair is the submit-to-present boundary spelled as a queue-side semaphore so
/// a present cannot run ahead of its render when the two role queues are distinct handles (two VkQue
/// ue objects have no implicit ordering between their submissions, and a bare vkQueuePresentKHR wai
/// ts on nothing) - on a machine where the family exposes one queue the pair is a no-op rather than
/// a save. The fence is vkQueueSubmit's own argument and bounds the frame for the host; the semaphor
/// e bounds the present for the queue. There is NO host-side semaphore-wait row in this contract:
/// vkWaitSemaphores is a core-1.2 entry point, live at this library's 1.3 floor
/// (XUD_INSTANCE_API_FLOOR) and still not called - the gate is this layer's own choice, not a
/// fallback for a missing name. The core-1.0 form of the same boundary is the submit's pWaitSem
/// aphores + pWaitDstStageMask pair, which lives on the queue rather than on the host. XUD_INVA
/// LID_ARG on a NULL out-param or a handle-less command buffer / swapchain / semaphore.
XudResult xud_frame_fill_submit(VkSubmitInfo* outSubmit, VkPresentInfoKHR* outPresent, VkCommandBuffer commandBuffer,
    VkSwapchainKHR swapchain, VkSemaphore renderFinished);

/// Pure: the present queue's no-command wait submit for one frame. It waits the render-finished semaphore
/// the frame's own submit signals and carries no command buffer, so its only job is to order the present
/// queue behind the render - the submit-to-present boundary for a device whose two role queues are two
/// distinct handles, where a bare vkQueuePresentKHR would wait on nothing. XUD_INVALID_ARG on a NULL
/// out-param or a handle-less semaphore.
XudResult xud_frame_fill_present_wait(VkSubmitInfo* outSubmit, VkSemaphore renderFinished);

/// Pure: the push-constant row of ONE draw, from the batch's draw row. The model matrix is the iden
/// tity for the same reason the block's viewProj is: a non-identity matrix is a transform the step a
/// bove this one has to choose, and it has not landed - what is settled here is the CHANNEL (the row
/// is pushed, the vertex stage reads it, the step above hands its own matrix in without this shape c
/// hanging). materialIndex is the draw row's own, with the batch's XUD_BATCH_NO_MATERIAL sentinel map
/// ped to slot 0 because the push row's member is an index a read could use and the sentinel is 0xFF
/// FFFFF, which indexes nothing - a real material's slot index passes through unchanged, because th
/// e batch's material channel IS the handle layer's MATERIAL slot index; textureIndex is 0 for a n
/// o-map draw, which is what the batch's XUD_BATCH_NO_TEXTURE sentinel maps to - slot 0 holds the w
/// hite default the descriptor rung wrote at its bring-up - and a real slot passes through unchanged; flags is t
/// he variant the draw was flushed under, in the per-draw channel rather than a channel of this rung
/// 's - the pipeline variant is chosen by BINDING, and this is the same fact from the shader's side, which is what lets a stage branch on
/// the va riant without a third pipeline. The vertex stage reads only the model matrix today, so the other m embers are recorded and
/// unread: the row is the channel and a stage need not read every member of a block its layout declares. XUD_INVALID_ARG on a NULL out-row
/// or a NULL draw row.
XudResult xud_frame_fill_push_row(XudPushRow* outRow, const XudBatchDraw* drawRow);

/// Driver: the whole bring-up of this rung - vkCreateCommandPool, vkAllocateCommandBuffers once per
/// in-flight slot, vkCreateSemaphore twice per slot (the acquire and the render-finished pair), and
/// vkCreateFence once per slot. Before any call: the loader is open,
/// its device rows are this device's rows, and EVERY destroy row this teardown needs is checked BE
/// FORE the create rows it would otherwise leak (the rule xud_window.c and xud_swapchain.c state fo
/// r a surface and a swapchain, one rung below). On XUD_OK *outFrame owns a pool, one command buffer
/// per in-flight slot, the two semaphore rows and the fence row, every create-info above says what each was created with, and
/// the wait-stage row names the vertex stage (the stage the acquire semaphore is waited AT, which i
/// s where the draw's work begins). On any error it stays NULL and everything created so far is des
/// troyed in reverse order. XUD_INVALID_ARG on a NULL out-pointer or one that already owns a frame,
/// a NULL loader/device/swapchain/tracker, a handle-less device, or a swapchain with no handle or
/// no images; XUD_UNSUPPORTED when the loader is closed, its rows are not this device's, or a row
/// this rung needs is nil.
XudResult xud_frame_open(
    XudFrame** outFrame, XudLoader* loader, const XudDevice* device, const XudSwapchain* swapchain, XudLayoutTracker* layout);

/// Driver: one frame, end to end - wait the slot's fence if the slot still bounds work, acquire, reset + record the slot's command buffer (begin, viewport, scissor, bind pipeline, begin render pass with the pass's clear values, draw, end pass, end), submit to the graphics queue with the slot's own fence, present on the present queue. The CPU does NOT wait its own frame at the end: the next use of the same slot waits the fence, which is what lets frame N+1 record while frame N still runs. The framebuffer is picked by the
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
/// e driver refused the submit, the present or a wait, or answered the acquire/present pair with a
/// code outside the swapchain's own two (VK_SUBOPTIMAL_KHR and VK_ERROR_OUT_OF_DATE_KHR are NOT re
/// fusals: for those two the image is usable and the presentation happens, so the frame runs and
/// leaves frame->recreateWanted set for the resize step above to act on - xud_swapchain_result_fro
/// m_vk() is the second funnel that makes that distinction a table rather than a caller's judge
/// ment, and the general funnel still calls both codes a fault everywhere else it is used).
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
/// the driver half pushes it against the layout's own recorded range) and, when the row's texture channel names a slot other than the one set 1 was left
/// at, the loop rebinds set 1 to that slot's OWN descriptor set - one set per texture slot, each
/// holding the one view-and-sampler pair the textured column's fragment stage samples. The rebind is
/// a bind, not a write: every slot set was written before it is bound (white at open, a texture's own
/// pair at its bind, white again at its release), so no update lands on a set this buffer has already
/// recorded. so the vertex is placed by thi
/// s frame's viewProj times this draw's model matrix - the per-draw channel and the per-frame set are
/// both live, and what is still identity in both is the matrices themselves, because a non-identity m
/// atrix is a transform the step above this one has to choose.
/* The sentinel a recorded pass carries when it is NOT the resolve pass: any value at or past the
   sampler-array size names no slot, so a real slot (0 .. size-1) says "this pass ends by drawing the
   full-screen triangle from that slot's image" - see xud_frame_record_pass in xud_frame.c. */
#define XUD_FRAME_NO_RESOLVE 0xFFFFFFFFu
/// One pass sequence among the recordings is the DEPTH PRE-PASS (S2.4): `prepassPass` and its own
/// `prepassFramebuffer` are recorded once before `hdrPass`/`hdrFramebuffer` - the same scene batch
/// drawn through the pipeline's colorless pre-pass row into the scratch pass's own depth buffer -
/// and the scratch pass then LOADs that depth and rejects the fragments behind it at the
/// rasterizer. The clear, the sort and the replay are one sequence owned at this level; the caller
/// above hands over the pair and the pipeline list and knows nothing of the order.
XudResult xud_frame_draw(XudFrame* frame, XudLoader* loader, const XudDevice* device, const XudSwapchain* swapchain,
    const XudPipeline* pipeline, const XudRenderPass* pass, const XudFramebufferArray* framebuffers, uint32_t framebufferCount,
    const XudRenderPass* hdrPass, const XudFramebuffer* hdrFramebuffer, const XudRenderPass* prepassPass,
    const XudFramebuffer* prepassFramebuffer, uint32_t hdrSlot, const XudFramePassScope* scopes, uint32_t scopeCount, XudBatch* batch,
    XudDescriptor* descriptor, uint32_t* outImageIndex);

/// Driver: retire everything this rung owns, in the one order that is correct - vkWaitForFences on
/// every fence still bounding a submission, then vkFreeCommandBuffers, vkDestroySemaphore, vkDestroyFence for ever
/// y in-flight fence, vkDestroyCommandPool LAST because the command buffer belongs to the pool and the
/// spec requires the pool to outlive every buffer allocated from it. XUD_ALREADY when the pointer o
/// wns no frame; XUD_INVALID_ARG on a NULL pointer-to-pointer or a NULL loader/device; XUD_UNSUPPOR
/// TED when the loader is closed, its rows are not this device's, or a destroy row is nil while the
/// object it would destroy is live (the struct is not freed, because freeing it would leak the hand
/// les silently). A refused teardown is reported, not hidden, and the rungs after it still run - th
/// e rule xud_close_context() states.
XudResult xud_frame_close(XudFrame** ioFrame, const XudDevice* device, XudLoader* loader);

#endif /* XUD_FRAME_H_ */
