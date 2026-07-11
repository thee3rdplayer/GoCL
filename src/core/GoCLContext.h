#pragma once

#include "CapabilityOracle.h"
#include "ASTCThresholds.h"
#include "DGCManager.h"
#include <vulkan/vulkan.h>

#include <cstdint>

namespace GoCL {

/**
 * @brief Small header prepended to the pipeline cache file.
 * Used to verify that the cached data matches the current GPU/driver.
 */
struct PipelineCacheHeader {
    uint32_t vendorID;
    uint32_t deviceID;
    uint32_t driverVersion;
    uint8_t  pipelineCacheUUID[VK_UUID_SIZE];
};

/**
 * @brief Central engine context that holds all Vulkan objects and device capabilities.
 * 
 * Created once at startup and passed to subsystems (renderer, texture manager, etc.).
 * The same binary works on discrete GPUs, integrated GPUs, and software renderers.
 */
struct GoCLContext {
    // ─── Vulkan core handles ──────────────────────────────────────────────────
    VkInstance       instance       = VK_NULL_HANDLE;   ///< Vulkan instance (provided by caller)
    VkPhysicalDevice physDev        = VK_NULL_HANDLE;   ///< Selected physical device
    VkDevice         device         = VK_NULL_HANDLE;   ///< Logical device

    // ─── Queue families and queues ──────────────────────────────────────────
    VkQueue          gfxQueue       = VK_NULL_HANDLE;   ///< Primary graphics queue. Used for rendering, transfers, and presentation (if possible).
    uint32_t         gfxFamily      = VK_QUEUE_FAMILY_IGNORED;

    VkQueue          presentQueue   = VK_NULL_HANDLE;   ///< Present queue – populated by SetupPresentQueue() after surface creation. On most desktops it aliases gfxQueue.
    uint32_t         presentFamily  = VK_QUEUE_FAMILY_IGNORED;

    VkQueue          computeQueue   = VK_NULL_HANDLE;   ///< Dedicated async compute queue if available. May alias gfxQueue.
    uint32_t         computeFamily  = VK_QUEUE_FAMILY_IGNORED;

    VkQueue          transferQueue  = VK_NULL_HANDLE;   ///< Transfer queue – used for staging buffer copies. Often aliases gfxQueue.
    uint32_t         transferFamily = VK_QUEUE_FAMILY_IGNORED;

    // ─── Pipeline cache ──────────────────────────────────────────────────────
    VkPipelineCache  pipelineCache  = VK_NULL_HANDLE;   ///< Pipeline cache (persisted to disk between runs)

    // ─── Staging buffer ─────────────────────────────────────────────────────
    VkBuffer         stagingBuffer  = VK_NULL_HANDLE;   ///< CPU‑visible staging buffer for texture uploads
    VkDeviceMemory   stagingMemory  = VK_NULL_HANDLE;
    VkDeviceSize     stagingSize    = 0;                ///< Current allocated size (may grow)

    // ─── Command pool ───────────────────────────────────────────────────────
    VkCommandPool    transferPool   = VK_NULL_HANDLE;   ///< Command pool for one‑shot transfer commands

    // ─── DGC Manager ────────────────────────────────────────────────────────
    DGCManager       dgcManager;                        ///< Device-Generated Commands manager (layout, execution set, buffers)

    // ─── Device capabilities ───────────────────────────────────────────────
    DeviceCapabilities caps{};                          ///< Device capabilities (snapshotted at startup)
    VkPhysicalDeviceProperties devProps{};              ///< Cached Vulkan physical device properties

    // ─── Extension availability flags ──────────────────────────────────────
    bool hasMemoryBudgetExt = false;                    ///< VK_EXT_memory_budget
    bool hasSync2           = false;                    ///< VK_KHR_synchronization2
    PFN_vkCmdPipelineBarrier2KHR fpCmdPipelineBarrier2KHR = nullptr; ///< Function pointer for sync2 barriers

    // ─── Runtime configuration ─────────────────────────────────────────────
    ASTCThresholds     astcThresholds{};                ///< Thresholds for ASTC block size selection (from GoCL.conf)

    // ─── Move-only (copy disallowed) ───────────────────────────────────────
    GoCLContext(const GoCLContext&) = delete;
    GoCLContext& operator=(const GoCLContext&) = delete;
    GoCLContext(GoCLContext&&) noexcept = default;
    GoCLContext& operator=(GoCLContext&&) noexcept = default;

    GoCLContext() = default;

    // ─── Lifecycle ──────────────────────────────────────────────────────────

    /**
     * @brief Creates a fully initialised context.
     * 
     * Selects the best GPU, sets up queues, queries capabilities, and allocates staging resources.
     * 
     * @param instance Vulkan instance.
     * @param stagingBytes Size of staging buffer in bytes (default: 64 MiB).
     * @return Initialised GoCLContext.
     */
    static GoCLContext Create(VkInstance instance,
                              VkDeviceSize stagingBytes = 64ull * 1024 * 1024);

    /**
     * @brief Must be called after the surface is created.
     * 
     * Finds a queue family that supports present to that surface and retrieves the queue.
     * 
     * @param surface Vulkan surface.
     * @return true on success, false on failure.
     */
    bool SetupPresentQueue(VkSurfaceKHR surface);

    // ─── Operations ─────────────────────────────────────────────────────────

    /**
     * @brief Copies raw pixel data to a device‑local image with optimal tiling.
     * 
     * The returned VkImage is ready for sampling. The caller must free both the image
     * and its memory via DestroyTexture().
     * 
     * @param fmt Image format.
     * @param w Image width.
     * @param h Image height.
     * @param data Source pixel data.
     * @param dataSize Size of source data in bytes.
     * @param outMemory Output device memory handle.
     * @return VkImage Handle to the created image.
     */
    VkImage UploadTexture(VkFormat fmt, uint32_t w, uint32_t h,
                          const void* data, VkDeviceSize dataSize,
                          VkDeviceMemory* outMemory) const;

    /**
     * @brief Destroys a texture image and its bound device memory.
     * 
     * @param image Image to destroy.
     * @param memory Memory to free.
     */
    void DestroyTexture(VkImage image, VkDeviceMemory memory) const;

    /**
     * @brief Returns the remaining VRAM budget in MB.
     * 
     * Only meaningful if hasMemoryBudgetExt is true.
     * Falls back to total VRAM when the extension is absent.
     * 
     * @return Remaining VRAM in MiB.
     */
    uint32_t VRAMRemainingMB() const;

    /**
     * @brief Pipeline cache persistence.
     * 
     * The cache is validated using the header before loading.
     * 
     * @param path File path for the pipeline cache.
     */
    void SavePipelineCache(const char* path) const;
    void LoadPipelineCache(const char* path);

    /**
     * @brief Destroys all Vulkan objects owned by this context.
     * 
     * Idempotent and safe to call multiple times.
     */
    void Destroy();
};

}   // namespace GoCL