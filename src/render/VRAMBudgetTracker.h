#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>
#include <cstring>

namespace GoCL {

// =============================================================================
// Queries the GPU's current memory usage and budget.
//
// Uses VK_EXT_memory_budget to get runtime‑accurate numbers when available.
// Falls back to static heap sizes on older drivers.
//
// The extension reports a *per‑process budget*, not total installed VRAM.
// The `RemainingMB()` method returns how much of that budget is still free.
// =============================================================================

/**
 * @brief Tracks GPU memory budget and usage.
 * 
 * Uses VK_EXT_memory_budget when available, otherwise falls back to
 * static heap size information.
 */
class VRAMBudgetTracker {
public:
    /**
     * @brief Constructor that immediately refreshes the budget.
     * 
     * @param physDev Physical device to query.
     * @param hasMemoryBudget Whether VK_EXT_memory_budget is supported.
     */
    explicit VRAMBudgetTracker(VkPhysicalDevice physDev,
                               bool hasMemoryBudget = false) {
        Refresh(physDev, hasMemoryBudget);
    }

    /**
     * @brief Refresh – re‑queries the current memory budget.
     * 
     * Should be called periodically (e.g., once per frame) to keep
     * RemainingMB() up‑to‑date without recreating the object.
     * 
     * @param physDev Physical device to query.
     * @param hasMemoryBudget Whether VK_EXT_memory_budget is supported.
     */
    void Refresh(VkPhysicalDevice physDev, bool hasMemoryBudget = false) {
        totalBudget_ = 0;
        usedBudget_  = 0;

        // Determine the Vulkan API version to decide whether we can use
        // VkPhysicalDeviceMemoryProperties2 (core in 1.1, extension in 1.0).
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physDev, &props);
        const bool hasProps2 = (props.apiVersion >= VK_API_VERSION_1_1);

        // If the caller didn't already tell us about the extension, try to
        // detect it ourselves.
        bool hasBudget = hasMemoryBudget;
        if (!hasBudget && hasProps2) {
            uint32_t extCount = 0;
            VkResult r = vkEnumerateDeviceExtensionProperties(physDev, nullptr,
                                                               &extCount, nullptr);
            if (r == VK_SUCCESS) {
                std::vector<VkExtensionProperties> exts(extCount);
                r = vkEnumerateDeviceExtensionProperties(physDev, nullptr,
                                                          &extCount, exts.data());
                if (r == VK_SUCCESS) {
                    for (auto& e : exts) {
                        if (std::strcmp(e.extensionName,
                                        VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) {
                            hasBudget = true;
                            break;
                        }
                    }
                }
            }
        }

        if (hasBudget && hasProps2) {
            // Use the official memory budget extension to get per‑heap budget
            // and current usage. This gives the most accurate remaining VRAM.
            VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
            budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;

            VkPhysicalDeviceMemoryProperties2 memProps2{};
            memProps2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
            memProps2.pNext = &budget;
            vkGetPhysicalDeviceMemoryProperties2(physDev, &memProps2);

            for (uint32_t i = 0; i < memProps2.memoryProperties.memoryHeapCount; ++i) {
                // Sum only device‑local heaps (VRAM). Exclude multi‑instance
                // heaps (those are for multi‑GPU systems, not counted here).
                if ((memProps2.memoryProperties.memoryHeaps[i].flags &
                     VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
                    !(memProps2.memoryProperties.memoryHeaps[i].flags &
                      VK_MEMORY_HEAP_MULTI_INSTANCE_BIT)) {
                    totalBudget_ += budget.heapBudget[i];
                    usedBudget_  += budget.heapUsage[i];
                }
            }
        } else {
            // Fallback for drivers that do not support the budget extension:
            // only the total static heap size is available; we cannot know
            // current usage, so RemainingMB() will return the total VRAM.
            VkPhysicalDeviceMemoryProperties memProps{};
            vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);

            for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
                if ((memProps.memoryHeaps[i].flags &
                     VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
                    !(memProps.memoryHeaps[i].flags &
                      VK_MEMORY_HEAP_MULTI_INSTANCE_BIT)) {
                    totalBudget_ += memProps.memoryHeaps[i].size;
                }
            }
            usedBudget_ = 0;    // No usage information → assume all VRAM is free.
        }
    }

    /**
     * @brief Returns the free budget in megabytes.
     * 
     * Guards against drivers that transiently report usedBudget_ > totalBudget_,
     * which would give a negative result – in that case, returns 0.
     * 
     * @return Free VRAM budget in MiB.
     */
    [[nodiscard]]
    uint32_t RemainingMB() const noexcept {
        if (usedBudget_ >= totalBudget_) {
            return 0;
        }
        return static_cast<uint32_t>((totalBudget_ - usedBudget_) / (1024u * 1024u));
    }

    /**
     * @brief A very rough estimate of VRAM saved by using ETC2 instead of uncompressed RGBA8.
     * 
     * Used only for heuristic decisions. Assumes ETC2 compression ratio of roughly 4:1
     * (16 bytes per 4x4 block vs 64 bytes raw). Actual savings vary.
     * 
     * @param rawTextureMB Size of uncompressed texture in MiB.
     * @return Estimated VRAM savings in MiB.
     */
    static uint32_t ProjectedSavingsMB(uint32_t rawTextureMB) noexcept {
        return rawTextureMB * 3u / 4u;
    }

private:
    uint64_t totalBudget_ = 0;  ///< Total process budget (or static heap size) in bytes
    uint64_t usedBudget_  = 0;  ///< Currently used budget in bytes
};

}   // namespace GoCL