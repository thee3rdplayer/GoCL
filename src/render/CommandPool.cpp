#include "CommandPool.h"

#include <cassert>

namespace GoCL {

// ---------------------------------------------------------------------------
// Initialises the graphics command pools.
// Creates one VkCommandPool and one primary VkCommandBuffer per in‑flight frame.
// On failure, any already created pools are destroyed (rollback).
// ---------------------------------------------------------------------------
bool CommandPool::init(GoCLContext& ctx, uint32_t maxFrames) {
    m_device    = ctx.device;
    m_gfxFamily = ctx.gfxFamily;

    VkCommandPoolCreateInfo poolCI{};
    poolCI.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCI.queueFamilyIndex = m_gfxFamily;
    assert(m_gfxFamily != UINT32_MAX && "Graphics queue family index is uninitialized!");

    // RESET_COMMAND_BUFFER_BIT allows vkResetCommandBuffer per frame.
    // TRANSIENT_BIT hints that command buffers are short‑lived (one submission).
    poolCI.flags =
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
        VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

    VkCommandBufferAllocateInfo allocCI{};
    allocCI.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocCI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocCI.commandBufferCount = 1; // one buffer per pool

    m_gfxPools.resize(maxFrames);
    m_gfxBuffers.resize(maxFrames);

    for (uint32_t i = 0; i < maxFrames; ++i) {
        // Create a dedicated pool for this frame slot.
        if (vkCreateCommandPool(m_device, &poolCI, nullptr, &m_gfxPools[i]) != VK_SUCCESS) {
            // Rollback all pools created so far.
            for (uint32_t j = 0; j < i; ++j) {
                vkDestroyCommandPool(m_device, m_gfxPools[j], nullptr);
            }
            m_device = VK_NULL_HANDLE;
            m_gfxPools.clear();
            m_gfxBuffers.clear();
            return false;
        }

        // Allocate a command buffer from that pool.
        allocCI.commandPool = m_gfxPools[i];
        if (vkAllocateCommandBuffers(m_device, &allocCI, &m_gfxBuffers[i]) != VK_SUCCESS) {
            // Rollback all pools (including the one we just created).
            for (uint32_t j = 0; j <= i; ++j) {
                vkDestroyCommandPool(m_device, m_gfxPools[j], nullptr);
            }
            m_gfxPools.clear();
            m_gfxBuffers.clear();
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Optionally creates compute‑queue command pools.
// Only runs if the device has a dedicated compute queue family different from
// the graphics family. Uses per‑frame pools (one per frame) for async compute.
// ---------------------------------------------------------------------------
void CommandPool::initCompute(GoCLContext& ctx,
                              uint32_t maxFrames,
                              VkCommandPoolCreateFlags extraFlags) {
    // No dedicated compute queue – silently skip.
    if (ctx.computeFamily == ctx.gfxFamily || ctx.computeQueue == VK_NULL_HANDLE) {
        return;
    }

    m_computeFamily = ctx.computeFamily;

    VkCommandPoolCreateInfo poolCI{};
    poolCI.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCI.queueFamilyIndex = m_computeFamily;
    poolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | extraFlags;

    VkCommandBufferAllocateInfo allocCI{};
    allocCI.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocCI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocCI.commandBufferCount = 1; // one buffer per pool

    m_computePools.resize(maxFrames);
    m_computeBuffers.resize(maxFrames);
    m_computeFirstUse.assign(maxFrames, 1); // initialised to "first use" state

    for (uint32_t i = 0; i < maxFrames; ++i) {
        if (vkCreateCommandPool(m_device, &poolCI, nullptr, &m_computePools[i]) != VK_SUCCESS) {
            // Rollback any pools created before the failure.
            for (uint32_t j = 0; j < i; ++j) {
                vkDestroyCommandPool(m_device, m_computePools[j], nullptr);
            }
            m_computePools.clear();
            m_computeBuffers.clear();
            m_computeFirstUse.clear();
            return;
        }

        allocCI.commandPool = m_computePools[i];
        if (vkAllocateCommandBuffers(m_device, &allocCI, &m_computeBuffers[i]) != VK_SUCCESS) {
            for (uint32_t j = 0; j <= i; ++j) {
                vkDestroyCommandPool(m_device, m_computePools[j], nullptr);
            }
            m_computePools.clear();
            m_computeBuffers.clear();
            m_computeFirstUse.clear();
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Begins recording on the graphics command buffer for the given frame index.
// Resets the entire command pool (freeing all previous commands) before
// recording a new frame.
// ---------------------------------------------------------------------------
VkResult CommandPool::beginRecording(uint32_t frameIndex) {
    if (frameIndex >= m_gfxBuffers.size() || !m_gfxBuffers[frameIndex]) {
        return VK_ERROR_UNKNOWN;
    }

    // Reset the entire pool – discards all command buffers allocated from it.
    vkResetCommandPool(m_device, m_gfxPools[frameIndex], 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;   // single‑use buffer
    return vkBeginCommandBuffer(m_gfxBuffers[frameIndex], &bi);
}

VkResult CommandPool::endRecording(uint32_t frameIndex) {
    if (frameIndex >= m_gfxBuffers.size() || !m_gfxBuffers[frameIndex]) {
        return VK_ERROR_UNKNOWN;
    }
    return vkEndCommandBuffer(m_gfxBuffers[frameIndex]);
}

// ---------------------------------------------------------------------------
// Begins recording on the compute queue command buffer.
// ---------------------------------------------------------------------------
VkResult CommandPool::beginComputeRecording(uint32_t frameIndex) {
    if (frameIndex >= m_computeBuffers.size() || !m_computeBuffers[frameIndex]) {
        return VK_ERROR_UNKNOWN;
    }

    vkResetCommandPool(m_device, m_computePools[frameIndex], 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(m_computeBuffers[frameIndex], &bi);
}

VkResult CommandPool::endComputeRecording(uint32_t frameIndex) {
    if (frameIndex >= m_computeBuffers.size() || !m_computeBuffers[frameIndex]) {
        return VK_ERROR_UNKNOWN;
    }
    return vkEndCommandBuffer(m_computeBuffers[frameIndex]);
}

// ---------------------------------------------------------------------------
// Destroys all command pools and clears internal vectors.
// Also resets queue family indices to UINT32_MAX to detect uninitialised usage.
// ---------------------------------------------------------------------------
void CommandPool::destroy() {
    if (m_device == VK_NULL_HANDLE) return;

    for (VkCommandPool pool : m_gfxPools) {
        if (pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, pool, nullptr);
        }
    }
    m_gfxPools.clear();
    m_gfxBuffers.clear();

    for (VkCommandPool pool : m_computePools) {
        if (pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, pool, nullptr);
        }
    }
    m_computePools.clear();
    m_computeBuffers.clear();
    m_computeFirstUse.clear();

    // Reset handles and state – helpful for debugging use‑after‑destroy.
    m_device = VK_NULL_HANDLE;
    m_gfxFamily = UINT32_MAX;
    m_computeFamily = UINT32_MAX;
}

}   // namespace GoCL