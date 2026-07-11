#pragma once

#include "core/GoCLContext.h"
#include "Swapchain.h"
#include "RenderPass.h"
#include "FrameSync.h"
#include "CommandPool.h"
#include <vulkan/vulkan.h>

#include <functional>
#include <vector>

namespace GoCL {

/**
 * @brief Immutable configuration passed to FrameLoop::init().
 * 
 * Only depth format can be tuned; other parameters come from GoCL.conf.
 */
struct FrameLoopConfig {
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;    ///< Preferred depth format; will fall back if unsupported
};

/**
 * @brief Callback type for recording rendering commands.
 * 
 * Called inside the render pass after vkCmdBeginRenderPass.
 * The implementation must set viewport and scissor dynamically.
 */
using RecordFn = std::function<void(VkCommandBuffer cmd,
                                    VkFramebuffer   fb,
                                    uint32_t        frameIndex,
                                    VkExtent2D      extent)>;

/**
 * @brief Per-draw parameters for DGC.
 * 
 * This struct must match the push constant layout expected by the shader.
 */
struct DrawData {
    uint32_t materialIndex = 0;
    // Add more fields as needed (e.g., model matrix index, etc.)
};

/**
 * @brief High‑level frame loop that owns swapchain, render pass, synchronisation,
 *        command pools, depth resources and framebuffers.
 * 
 * Handles resizing and presentation. The drawing code is injected via a callback (RecordFn).
 */
class FrameLoop {
public:
    FrameLoop()  = default;
    ~FrameLoop() { destroy(); }

    FrameLoop(const FrameLoop&)            = delete;
    FrameLoop& operator=(const FrameLoop&) = delete;

    /**
     * @brief Initialises all Vulkan objects needed for rendering.
     * 
     * @param ctx GoCLContext containing device and physical device handles.
     * @param surface Vulkan surface for presentation.
     * @param width Initial swapchain width.
     * @param height Initial swapchain height.
     * @param cfg Frame loop configuration (optional).
     * @return true on success, false on failure.
     */
    bool init(GoCLContext& ctx, VkSurfaceKHR surface,
              uint32_t width, uint32_t height,
              const FrameLoopConfig& cfg = {});

    /**
     * @brief Destroys all resources in correct order (idempotent).
     */
    void destroy();

    /**
     * @brief Template overload that accepts any callable with the same signature as RecordFn.
     */
    template <typename Fn>
    bool drawFrame(GoCLContext& ctx, Fn&& record, bool& needsResize);

    /**
     * @brief std::function overload – forwards to the template version.
     */
    bool drawFrame(GoCLContext& ctx, RecordFn record, bool& needsResize) {
        return drawFrame(ctx, std::move(record), needsResize);
    }

    /**
     * @brief Recreates the swapchain and dependent resources (framebuffers, depth) after a resize.
     */
    bool resize(GoCLContext& ctx, uint32_t width, uint32_t height);

    // ─── Accessors ──────────────────────────────────────────────────────────────

    VkRenderPass renderPass() const { return m_renderPass.handle(); }
    VkExtent2D   extent()     const { return m_swapchain.extent(); }

    // ─── DGC draw management ──────────────────────────────────────────────────
    void addDraw(const DrawData& draw) { m_draws.push_back(draw); }
    void clearDraws() { m_draws.clear(); }

private:
    bool createDepthResources(GoCLContext& ctx);
    bool createFramebuffers(GoCLContext& ctx);
    void destroyDepthResources();
    void destroyFramebuffers();

    static uint32_t findMemoryType(VkPhysicalDevice gpu,
                                   uint32_t typeFilter,
                                   VkMemoryPropertyFlags props);

    void recordDGC(GoCLContext& ctx, VkCommandBuffer cmd, uint32_t frameIndex, const std::vector<DrawData>& draws);

    // ─── Core rendering objects ───────────────────────────────────────────────
    Swapchain   m_swapchain;
    RenderPass  m_renderPass;
    FrameSync   m_frameSync;    ///< Synchronisation objects (semaphores/fences)
    CommandPool m_cmdPool;      ///< Per‑frame command pools

    // ─── Depth resources ──────────────────────────────────────────────────────
    VkImage        m_depthImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_depthMem    = VK_NULL_HANDLE;
    VkImageView    m_depthView   = VK_NULL_HANDLE;
    VkFormat       m_depthFormat = VK_FORMAT_D32_SFLOAT;    ///< Actual depth format used

    // ─── Framebuffers ─────────────────────────────────────────────────────────
    std::vector<VkFramebuffer> m_framebuffers;   ///< One per swapchain image
    std::vector<VkFence>       m_imageInFlight;  ///< Fence for each swapchain image (to avoid reusing while in flight)

    // ─── DGC data ─────────────────────────────────────────────────────────────
    std::vector<DrawData>      m_draws;          ///< Accumulates per-frame draw parameters for DGC

    // ─── Vulkan handles ──────────────────────────────────────────────────────
    VkDevice m_device    = VK_NULL_HANDLE;
    VkQueue  m_graphicsQ = VK_NULL_HANDLE;
    VkQueue  m_presentQ  = VK_NULL_HANDLE;
};

// ---------------------------------------------------------------------------
// Templated drawFrame – zero‑cost abstraction over the record callback.
// The callback is invoked inside the render pass, after the framebuffer is bound.
// The bool& needsResize is set to true when the swapchain is out‑of‑date or suboptimal.
// Returns false on permanent errors (e.g., queue submission failure).
// ---------------------------------------------------------------------------
template <typename Fn>
bool FrameLoop::drawFrame(GoCLContext& ctx, Fn&& record, bool& needsResize) {
    needsResize = false;
    const uint32_t fi = m_frameSync.currentFrame();

    // Acquire the next swapchain image.
    uint32_t imageIdx = 0;
    VkResult r = m_swapchain.acquireNextImage(m_device,
                                               m_frameSync.frame(fi).imageAvailable,
                                               imageIdx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        needsResize = true;
        return true;
    }

    // Wait for the previous frame's fence (or timeline semaphore) to complete.
    if (m_frameSync.waitAndReset(m_device) != VK_SUCCESS) {
        return false;
    }

    // Handle other acquire results.
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        return false;
    }

    // Prevent reusing a swapchain image that is still being used by the GPU.
    if (m_imageInFlight[imageIdx] != VK_NULL_HANDLE) {
        vkWaitForFences(m_device, 1, &m_imageInFlight[imageIdx], VK_TRUE, UINT64_MAX);
    }
    m_imageInFlight[imageIdx] = m_frameSync.frame(fi).inFlight;

    // Begin recording commands for this frame.
    if (m_cmdPool.beginRecording(fi) != VK_SUCCESS) {
        return false;
    }
    VkCommandBuffer cmd = m_cmdPool.cmdBuffer(fi);

    // Clear values for colour and depth attachments.
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color        = {{ 0.0f, 0.0f, 0.0f, 1.0f }};
    clearValues[1].depthStencil = { 1.0f, 0 };

    VkRenderPassBeginInfo rpBI{};
    rpBI.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBI.renderPass        = m_renderPass.handle();
    rpBI.framebuffer       = m_framebuffers[imageIdx];
    rpBI.renderArea.offset = { 0, 0 };
    rpBI.renderArea.extent = m_swapchain.extent();
    rpBI.clearValueCount   = 2;
    rpBI.pClearValues      = clearValues.data();

    vkCmdBeginRenderPass(cmd, &rpBI, VK_SUBPASS_CONTENTS_INLINE);

    if (ctx.caps.dgcSupported) {
        // If DGC is supported, the callback should record draws into m_draws,
        // not issue actual vkCmdDraw calls.
        // We assume the callback does this internally.
        // After the callback, we flush the draws via DGC.
        record(cmd, m_framebuffers[imageIdx], fi, m_swapchain.extent());

        // Flush DGC draws
        if (!m_draws.empty()) {
            recordDGC(ctx, cmd, fi, m_draws);
            m_draws.clear();
        }
    } else {
        // Traditional path: callback issues vkCmdDraw directly.
        record(cmd, m_framebuffers[imageIdx], fi, m_swapchain.extent());
    }

    vkCmdEndRenderPass(cmd);

    if (m_cmdPool.endRecording(fi) != VK_SUCCESS) {
        return false;
    }

    // Submit the command buffer.
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &m_frameSync.frame(fi).imageAvailable;
    si.pWaitDstStageMask    = &waitStage;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cmd;
    si.signalSemaphoreCount = 1;

    // Timeline vs binary semaphore handling.
    VkTimelineSemaphoreSubmitInfo timelineInfo{};
    uint64_t signalValue = 0;
    if (m_frameSync.m_useTimeline) {
        si.pSignalSemaphores = &m_frameSync.m_timelineSemaphore;
        signalValue = m_frameSync.m_timelineValues[fi];
        timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineInfo.signalSemaphoreValueCount = 1;
        timelineInfo.pSignalSemaphoreValues = &signalValue;
        si.pNext = &timelineInfo;
    } else {
        si.pSignalSemaphores = &m_frameSync.frame(fi).renderFinished;
        si.pNext = nullptr;
    }

    VkFence submitFence = m_frameSync.m_useTimeline ? VK_NULL_HANDLE : m_frameSync.frame(fi).inFlight;

    if (vkQueueSubmit(m_graphicsQ, 1, &si, submitFence) != VK_SUCCESS) {
        vkDeviceWaitIdle(m_device);
        return false;
    }

    // Present the rendered image.
    VkSwapchainKHR sc = m_swapchain.handle();
    VkPresentInfoKHR pi{};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = m_frameSync.m_useTimeline
                                ? &m_frameSync.m_timelineSemaphore
                                : &m_frameSync.frame(fi).renderFinished;
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &sc;
    pi.pImageIndices      = &imageIdx;

    r = vkQueuePresentKHR(m_presentQ, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        needsResize = true;
        return true;
    } else if (r == VK_SUBOPTIMAL_KHR) {
        needsResize = true;
    } else if (r != VK_SUCCESS) {
        return false;
    }

    m_frameSync.advance();
    return true;
}

}   // namespace GoCL