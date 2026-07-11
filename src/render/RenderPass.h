#pragma once

#include "core/GoCLContext.h"
#include <vulkan/vulkan.h>

namespace GoCL {

/**
 * @brief Single-subpass render pass: color + depth.
 * 
 * Recommended Vulkan defaults:
 * - clear colour and depth at pass start
 * - discard depth contents after the pass (storeOp = DONT_CARE) to save bandwidth
 * - sample count must match the attachments used by the render pass/subpass
 * - finalColorLayout allows off‑screen rendering (default = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
 */
class RenderPass {
public:
    RenderPass() = default;

    RenderPass(const RenderPass&)            = delete;
    RenderPass& operator=(const RenderPass&) = delete;

    /**
     * @brief Initialises the render pass. Called once at engine startup.
     * 
     * @param ctx GoCLContext containing device and physical device handles.
     * @param colorFormat Format of the swapchain colour attachment.
     * @param depthFormat Format of the depth/stencil attachment (must be supported by the device).
     * @param samples MSAA sample count (must match the pipeline's msaaSamples). Default: VK_SAMPLE_COUNT_1_BIT.
     * @param finalColorLayout Layout the colour attachment transitions to after the render pass. Default: VK_IMAGE_LAYOUT_PRESENT_SRC_KHR.
     * @param hasDepth If false, the render pass has no depth attachment. Default: true.
     * @return true on success, false on failure.
     */
    bool init(GoCLContext& ctx,
              VkFormat colorFormat,
              VkFormat depthFormat,
              VkSampleCountFlagBits  samples          = VK_SAMPLE_COUNT_1_BIT,
              VkImageLayout          finalColorLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
              bool                   hasDepth         = true) noexcept;

    /**
     * @brief Destructor – destroys the underlying Vulkan render pass object.
     */
    ~RenderPass() noexcept { destroy(); }

    /**
     * @brief Returns the Vulkan render pass handle.
     */
    [[nodiscard]]
    VkRenderPass handle() const noexcept {
        return m_renderPass;
    }

    /**
     * @brief Destroys the underlying Vulkan render pass object. Idempotent.
     */
    void destroy() noexcept;

private:
    VkDevice     m_device     = VK_NULL_HANDLE; ///< Logical device (for destruction)
    VkRenderPass m_renderPass = VK_NULL_HANDLE; ///< Vulkan render pass handle
};

}   // namespace GoCL