#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>


namespace GoCL {

class VRAMBudgetTracker {
public:
    explicit VRAMBudgetTracker(VkPhysicalDevice physDev) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
        VkPhysicalDeviceMemoryProperties2 memProps2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 };
        memProps2.pNext = &budget;
        vkGetPhysicalDeviceMemoryProperties2(physDev, &memProps2);

        for (uint32_t i = 0; i < memProps2.memoryProperties.memoryHeapCount; ++i) {
            if (memProps2.memoryProperties.memoryHeaps[i].flags &
                VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                totalVRAM_ += budget.heapBudget[i];
                usedVRAM_  += budget.heapUsage[i];
            }
        }
    }

    uint32_t RemainingMB() const noexcept {
        // Guard underflow — some drivers report usage > budget transiently
        if (usedVRAM_ >= totalVRAM_) return 0;
        return static_cast<uint32_t>((totalVRAM_ - usedVRAM_) / (1024u * 1024u));
    }

    // ETC2 compression ratio is 4:1 — 75% savings vs RGBA8
    static uint32_t ProjectedSavingsMB(uint32_t rawTextureMB) noexcept {
        return rawTextureMB * 3u / 4u;
    }

private:
    uint64_t totalVRAM_ = 0;
    uint64_t usedVRAM_  = 0;
};

} // namespace GoCL