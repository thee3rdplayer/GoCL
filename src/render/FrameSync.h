#pragma once

#include "core/GoCLContext.h"
#include <vulkan/vulkan.h>

#include <vector>
#include <cstdint>
#include <cassert>

class FrameLoop;    // forward declaration for friend

namespace GoCL {

/**
 * @brief Holds the Vulkan synchronisation objects for a single in‑flight frame
 *        when using the binary semaphore + fence path (fallback).
 */
struct FrameSyncObjects {
    VkSemaphore imageAvailable = VK_NULL_HANDLE;    ///< Signalled when the swapchain image is ready
    VkSemaphore renderFinished = VK_NULL_HANDLE;    ///< Signalled when the frame has finished rendering
    VkFence     inFlight       = VK_NULL_HANDLE;    ///< CPU fence to wait for the frame's completion
};

/**
 * @brief Manages per‑frame synchronisation.
 * 
 * Uses timeline semaphores when the device supports them (VK_KHR_timeline_semaphore)
 * and the `timelineSemaphoreEnabled` capability is true. Otherwise falls back to
 * binary semaphores + fences.
 *
 * The timeline path uses a single timeline semaphore and per‑slot wait values,
 * which reduces the number of Vulkan objects and simplifies CPU/GPU interaction.
 * The binary path creates a dedicated VkSemaphore and VkFence per frame slot.
 */
class FrameSync {
public:
    FrameSync()  = default;
    ~FrameSync() { destroy(); }

    FrameSync(const FrameSync&)            = delete;
    FrameSync& operator=(const FrameSync&) = delete;

    /**
     * @brief Initialises synchronisation objects for the given number of in‑flight frames.
     * 
     * @param ctx GoCLContext containing device and physical device handles.
     * @param maxFrames Number of in‑flight frames (must be at least 1, default 2).
     * @return true on success, false on failure.
     */
    bool init(GoCLContext& ctx, uint32_t maxFrames = 2);

    /**
     * @brief Destroys all synchronisation objects. Idempotent.
     */
    void destroy();

    /**
     * @brief Waits for the current frame's synchronisation object to be signalled,
     *        then resets it for reuse.
     * 
     * @param device Vulkan device.
     * @return VK_SUCCESS on success, otherwise a Vulkan error code.
     */
    VkResult waitAndReset(VkDevice device);

    /**
     * @brief Returns the FrameSyncObjects for a given frame index.
     * 
     * Only valid for the binary path.
     * 
     * @param i Frame index.
     * @return const FrameSyncObjects& Reference to the frame's sync objects.
     */
    [[nodiscard]]
    const FrameSyncObjects& frame(uint32_t i) const {
        assert(i < m_frames.size());
        return m_frames[i];
    }

    /**
     * @brief Returns the index of the current frame slot.
     * 
     * @return uint32_t Current frame index (0 .. maxFrames-1).
     */
    [[nodiscard]]
    uint32_t currentFrame() const { return m_currentFrame; }

    /**
     * @brief Advances to the next frame slot.
     * 
     * For timeline semaphores, also increments the wait value for the current slot
     * by the total number of frames.
     */
    void advance();

    /**
     * @brief Returns true if timeline semaphores are currently active.
     */
    [[nodiscard]]
    bool timelineEnabled() const noexcept { return m_useTimeline; }

private:
    VkDevice                    m_device        = VK_NULL_HANDLE;
    uint32_t                    m_currentFrame  = 0;
    bool                        m_useTimeline   = false;    ///< true = timeline path, false = binary path

    // ─── Binary path objects ──────────────────────────────────────────────────
    // One set of semaphores and fences per frame slot.
    // These are always created, even in timeline mode (used for swapchain acquire/present).
    std::vector<FrameSyncObjects> m_frames;

    // ─── Timeline path objects ───────────────────────────────────────────────
    // Only used when m_useTimeline == true.
    VkSemaphore               m_timelineSemaphore = VK_NULL_HANDLE; ///< Single timeline semaphore
    std::vector<uint64_t>     m_timelineValues;                     ///< Wait value per slot

    // Allow FrameLoop to read private members for efficient access during drawFrame.
    friend class FrameLoop;
};

}   // namespace GoCL