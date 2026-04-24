#include "core/GoCLContext.h"
#include <vulkan/vulkan.h>
#include <cstdio> 
#include <vector>

static int passed = 0, failed = 0;

#define PASS(name)        do { printf("  PASS  %s\n", name); ++passed; } while(0)
#define FAIL(name, msg)   do { printf("  FAIL  %s — %s\n", name, msg); ++failed; } while(0)
#define CHECK(name, cond, msg) do { if (cond) PASS(name); else FAIL(name, msg); } while(0)

int main() {
    printf("=== GoCLContextTest ===\n\n");

    // Bootstrap a minimal instance
    VkApplicationInfo appInfo{};
    appInfo.sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instCI{};
    instCI.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instCI.pApplicationInfo = &appInfo;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instCI, nullptr, &instance) != VK_SUCCESS) {
        printf("  FATAL: vkCreateInstance failed\n");
        return 1;
    }

    // Enumerate raw devices for ground-truth comparison
    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
    std::vector<VkPhysicalDevice> devs(devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, devs.data());

    bool hasDiscrete   = false;
    bool hasIntegrated = false;
    for (auto pd : devs) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(pd, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)   hasDiscrete   = true;
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) hasIntegrated = true;
    }

    // Create context — small staging buffer sufficient for test
    GoCL::GoCLContext ctx{};
    try {
        ctx = GoCL::GoCLContext::Create(instance, 4ull * 1024 * 1024);
    } catch (const std::exception& e) {
        printf("  FATAL: GoCLContext::Create threw: %s\n", e.what());
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    VkPhysicalDeviceProperties chosen{};
    vkGetPhysicalDeviceProperties(ctx.physDev, &chosen);
    printf("  Selected device: %s\n\n", chosen.deviceName);

    // 1. Must select discrete if available
    if (hasDiscrete) {
        CHECK("Selects_discrete_GPU_when_available",
            chosen.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
            "discrete GPU present but not selected");
    } else {
        PASS("Selects_discrete_GPU_when_available (no discrete present — skipped)");
    }

    // 2. Falls back to integrated when no discrete
    if (!hasDiscrete && hasIntegrated) {
        CHECK("Falls_back_to_integrated_GPU",
            chosen.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
            "integrated GPU present but not selected as fallback");
    } else {
        PASS("Falls_back_to_integrated_GPU (not applicable)");
    }

    // 3. physDev is not null
    CHECK("physDev_is_not_null",
        ctx.physDev != VK_NULL_HANDLE,
        "physDev is VK_NULL_HANDLE");

    // 4. device is not null
    CHECK("device_is_not_null",
        ctx.device != VK_NULL_HANDLE,
        "logical device is VK_NULL_HANDLE");

    // 5. Queues resolved
    CHECK("gfxQueue_resolved",
        ctx.gfxQueue != VK_NULL_HANDLE,
        "gfxQueue is null");

    // 6. Staging buffer allocated
    CHECK("stagingBuffer_allocated",
        ctx.stagingBuffer != VK_NULL_HANDLE,
        "staging buffer not allocated");

    // 7. Pipeline cache initialised
    CHECK("pipelineCache_initialised",
        ctx.pipelineCache != VK_NULL_HANDLE,
        "pipeline cache not initialised");

    ctx.Destroy();
    vkDestroyInstance(instance, nullptr);

    printf("\n--- Results: %d passed, %d failed ---\n", passed, failed);
    return failed ? 1 : 0;
}