#include "FrameSync.h"
#include "core/GoCLContext.h"

namespace GoCL {

// ---------------------------------------------------------------------------
// Initialises synchronisation objects for the given number of in‑flight frames.
// Prefers timeline semaphores when supported by the device (VK_KHR_timeline_semaphore).
// If timeline semaphores are not available, falls back to binary semaphores + fences.
// ---------------------------------------------------------------------------
bool FrameSync::init(GoCLContext& ctx, uint32_t maxFrames) {
    m_device = ctx.device;
    if (maxFrames == 0) {
        maxFrames = 2;
    }

    // Query whether the device supports timeline semaphores.
    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{};
    timelineFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;

    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &timelineFeatures;
    vkGetPhysicalDeviceFeatures2(ctx.physDev, &features);

    bool timelineAvailable = (timelineFeatures.timelineSemaphore == VK_TRUE);

    // Also check the capability flag stored in ctx.caps.timelineSemaphoreEnabled
    // (which should be set during CapabilityOracle::Query).
    m_useTimeline = timelineAvailable && ctx.caps.timelineSemaphoreEnabled;

    if (m_useTimeline) {
        // Create a single timeline semaphore for all frames.
        VkSemaphoreTypeCreateInfo typeCI{};
        typeCI.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        typeCI.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        typeCI.initialValue  = 0;

        VkSemaphoreCreateInfo semCI{};
        semCI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semCI.pNext = &typeCI;

        if (vkCreateSemaphore(m_device, &semCI, nullptr, &m_timelineSemaphore) != VK_SUCCESS) {
            return false;
        }

        // Each frame slot will wait for a different timeline value.
        // The first frame waits for 1, second for 2, etc.
        m_timelineValues.resize(maxFrames);
        for (uint32_t i = 0; i < maxFrames; ++i) {
            m_timelineValues[i] = i + 1;
        }
    }

    // Binary semaphores are always needed for swapchain image acquisition and
    // for signalling when the frame is finished (in the binary fallback path).
    VkSemaphoreCreateInfo semCI{};
    semCI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    m_frames.resize(maxFrames);
    for (uint32_t i = 0; i < maxFrames; ++i) {
        if (vkCreateSemaphore(m_device, &semCI, nullptr, &m_frames[i].imageAvailable) != VK_SUCCESS) {
            return false;
        }

        if (vkCreateSemaphore(m_device, &semCI, nullptr, &m_frames[i].renderFinished) != VK_SUCCESS) {
            return false;
        }

        // Fences are only needed when timeline semaphores are not used.
        if (!m_useTimeline) {
            VkFenceCreateInfo fenceCI{};
            fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fenceCI.flags = VK_FENCE_CREATE_SIGNALED_BIT;   // start in signalled state

            if (vkCreateFence(m_device, &fenceCI, nullptr, &m_frames[i].inFlight) != VK_SUCCESS) {
                return false;
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Destroys all synchronisation objects. Idempotent.
// ---------------------------------------------------------------------------
void FrameSync::destroy() {
    if (!m_device) {
        return;
    }

    for (auto& f : m_frames) {
        if (f.imageAvailable) {
            vkDestroySemaphore(m_device, f.imageAvailable, nullptr);
        }
        if (f.renderFinished) {
            vkDestroySemaphore(m_device, f.renderFinished, nullptr);
        }
        if (f.inFlight) {
            vkDestroyFence(m_device, f.inFlight, nullptr);
        }
        f = {};
    }

    m_currentFrame = 0;
    m_useTimeline = false;
    m_frames.clear();

    if (m_timelineSemaphore) {
        vkDestroySemaphore(m_device, m_timelineSemaphore, nullptr);
        m_timelineSemaphore = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Waits for the current frame's synchronisation object (timeline semaphore or fence)
// to be signalled, then resets it for the next use.
// For timeline semaphores, we wait for the specific value associated with this frame.
// For binary path, we wait for the fence and then reset it.
// ---------------------------------------------------------------------------
VkResult FrameSync::waitAndReset(VkDevice device) {
    if (m_useTimeline) {
        uint64_t waitValue = m_timelineValues[m_currentFrame];

        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores    = &m_timelineSemaphore;
        waitInfo.pValues        = &waitValue;

        // Wait indefinitely (UINT64_MAX) – the GPU will eventually signal.
        VkResult r = vkWaitSemaphores(device, &waitInfo, UINT64_MAX);
        if (r != VK_SUCCESS) {
            return r;
        }

        return VK_SUCCESS;
    } else {
        VkFence fence = m_frames[m_currentFrame].inFlight;
        VkResult r = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        if (r != VK_SUCCESS) {
            return r;
        }

        return vkResetFences(device, 1, &fence);
    }
}

// ---------------------------------------------------------------------------
// Advances to the next frame slot.
// For timeline semaphores, we also increase the wait value for the current slot
// by the total number of frames, so that when this slot is reused later,
// the GPU will signal a larger value (ensuring forward progress).
// ---------------------------------------------------------------------------
void FrameSync::advance() {
    if (m_useTimeline) {
        // Increase the value for the slot we just used, so that when we come back
        // to it, we wait for a higher value (the GPU will have signalled it).
        m_timelineValues[m_currentFrame] += m_frames.size();
    }

    m_currentFrame = (m_currentFrame + 1) % m_frames.size();
}

}   // namespace GoCL