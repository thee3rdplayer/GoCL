#pragma once

#include "core/GoCLContext.h"
#include <vulkan/vulkan.h>

#include <vector>
#include <cstdint>

namespace GoCL {

/**
 * @brief Manages per‑frame command pools and primary command buffers.
 * 
 * Two queues can be used: graphics (always) and compute (optional).
 * The number of in‑flight frames (maxFrames) is configurable; default is 2.
 *
 * Graphics pools are created for the graphics queue family.
 * Compute pools are created only if the device has a dedicated compute queue.
 */
class CommandPool {
public:
    CommandPool()  = default;
    ~CommandPool() { destroy(); }

    CommandPool(const CommandPool&)            = delete;
    CommandPool& operator=(const CommandPool&) = delete;

    CommandPool(CommandPool&&) noexcept;
    CommandPool& operator=(CommandPool&&) noexcept;

    /**
     * @brief Creates one graphics command pool and primary command buffer per frame.
     * 
     * @param ctx GoCLContext containing device and queue family information.
     * @param maxFrames Number of in‑flight frames (swapchain images / sync slots). Default: 2.
     * @return true on success, false on failure (partial resources rolled back).
     */
    bool init(GoCLContext& ctx, uint32_t maxFrames = 2);

    /**
     * @brief Optionally creates compute‑queue command pools for async compute work.
     * 
     * Only runs if the device has a compute queue family different from graphics.
     * 
     * @param ctx GoCLContext containing device and queue family information.
     * @param maxFrames Number of frames (typically 1 for compute). Default: 1.
     * @param extraFlags Additional command pool creation flags. Default: 0.
     */
    void initCompute(GoCLContext& ctx,
                     uint32_t maxFrames = 1,
                     VkCommandPoolCreateFlags extraFlags = 0);

    /**
     * @brief Destroys all command pools and clears internal vectors.
     * 
     * Idempotent and safe to call multiple times.
     */
    void destroy();

    /**
     * @brief Returns true if the compute pool was successfully initialised.
     */
    [[nodiscard]]
    bool hasCompute() const noexcept {
        return !m_computePools.empty();
    }

    /**
     * @brief Returns the graphics command buffer for the given frame index.
     * 
     * @param frameIndex Frame index.
     * @return VkCommandBuffer or VK_NULL_HANDLE if out of bounds.
     */
    [[nodiscard]]
    VkCommandBuffer cmdBuffer(uint32_t frameIndex) const {
        return (frameIndex < m_gfxBuffers.size()) ? m_gfxBuffers[frameIndex]
                                                  : VK_NULL_HANDLE;
    }

    /**
     * @brief Returns the compute command buffer for the given frame index.
     * 
     * Only valid if hasCompute() is true.
     * 
     * @param frameIndex Frame index.
     * @return VkCommandBuffer or VK_NULL_HANDLE if out of bounds.
     */
    [[nodiscard]]
    VkCommandBuffer computeCmdBuffer(uint32_t frameIndex) const {
        return (frameIndex < m_computeBuffers.size())
                   ? m_computeBuffers[frameIndex]
                   : VK_NULL_HANDLE;
    }

    /**
     * @brief Begins recording on the graphics command buffer for the specified frame.
     * 
     * Resets the entire command pool before recording, discarding previous commands.
     * 
     * @param frameIndex Frame index.
     * @return VkResult.
     */
    VkResult beginRecording(uint32_t frameIndex);
    VkResult endRecording(uint32_t frameIndex);

    /**
     * @brief Begins recording on the compute command buffer.
     * 
     * Only valid if hasCompute() is true.
     * 
     * @param frameIndex Frame index.
     * @return VkResult.
     */
    VkResult beginComputeRecording(uint32_t frameIndex);
    VkResult endComputeRecording(uint32_t frameIndex);

    /**
     * @brief Returns the number of frames (graphics command buffers) currently managed.
     */
    [[nodiscard]]
    uint32_t maxFrames() const {
        return static_cast<uint32_t>(m_gfxBuffers.size());
    }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    static constexpr uint32_t InvalidFamily = UINT32_MAX;

    uint32_t m_gfxFamily     = InvalidFamily;   ///< Graphics queue family index
    uint32_t m_computeFamily = InvalidFamily;   ///< Compute queue family index (if different)

    std::vector<VkCommandPool>   m_gfxPools;    ///< One pool per in‑flight frame
    std::vector<VkCommandBuffer> m_gfxBuffers;  ///< One primary command buffer per frame
    std::vector<uint8_t>         m_gfxFirstUse; ///< Tracks whether a buffer has been used yet

    std::vector<VkCommandPool>   m_computePools;    ///< Compute pools (if dedicated compute queue exists)
    std::vector<VkCommandBuffer> m_computeBuffers;
    std::vector<uint8_t>         m_computeFirstUse;
};

}   // namespace GoCL