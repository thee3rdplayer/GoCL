#pragma once

#include "CapabilityOracle.h" 

#include <vulkan/vulkan.h>
#include <cstdint> 


namespace GoCL {

// Central context — owns all Vulkan handles and shared resources.
// TextureManager and PipelineBuilder take const GoCLContext& instead of
// individual handles. One context per logical GPU.
struct GoCLContext {
    VkInstance       instance     = VK_NULL_HANDLE;
    VkPhysicalDevice physDev      = VK_NULL_HANDLE;
    VkDevice         device       = VK_NULL_HANDLE;
    VkQueue          gfxQueue     = VK_NULL_HANDLE;
    VkQueue          computeQueue = VK_NULL_HANDLE;
    uint32_t         gfxFamily    = 0;
    uint32_t         computeFamily= 0;

    // Shared pipeline cache — persisted to disk between runs (see Save/Load)
    VkPipelineCache  pipelineCache = VK_NULL_HANDLE;

    // Staging buffer — reusable, HOST_VISIBLE|HOST_COHERENT, for texture uploads
    VkBuffer         stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory   stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize     stagingSize   = 0;

    // Transfer command pool + one-time command buffer
    VkCommandPool    transferPool  = VK_NULL_HANDLE;

    // Populated once at init
    DeviceCapabilities caps{};

    // Initialise from a pre-created instance.
    // Selects best physical device, creates logical device + queues,
    // allocates staging buffer and pipeline cache.
    static GoCLContext Create(VkInstance instance,
                              VkDeviceSize stagingBytes = 64ull * 1024 * 1024);

    // Upload data via staging buffer → DEVICE_LOCAL OPTIMAL image.
    // Returns the image; caller owns it.
    VkImage UploadTexture(VkFormat fmt, uint32_t w, uint32_t h,
                          const void* data, VkDeviceSize dataSize) const;

    // Refresh VRAM budget (call once per frame or before large allocs)
    uint32_t VRAMRemainingMB() const;

    // Persist pipeline cache to disk
    void SavePipelineCache(const char* path) const;
    void LoadPipelineCache(const char* path);

    // Destroy all owned handles
    void Destroy();
};

} // namespace GoCL