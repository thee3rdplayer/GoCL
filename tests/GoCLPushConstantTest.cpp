// =============================================================================
// Validates PipelineBuilder::CreateLayout push‑constant safety.
//
// The engine must refuse to create a layout whose push‑constant range
// exceeds the hardware limit.  This guard is essential for older GPUs
// that only support the Vulkan‑minimum 128 bytes.
//
// Coverage:
//   1. Layout with push‑constant size ≤ device limit succeeds
//   2. Layout with push‑constant size > device limit → GoCL rejects and returns VK_NULL_HANDLE
//   3. Layout with maxPushConstSize=0 (no check) always succeeds (backward compat)
//   4. Zero push constants (no range) always succeeds
//
// Note: This test validates GoCL's guard logic, not Vulkan validation.
//       If the guard is removed, test behavior becomes driver‑dependent.
// =============================================================================

#include "render/PipelineBuilder.h"
#include "core/GoCLContext.h"
#include <vulkan/vulkan.h>

#include <cstdio>
#include <exception>

static int g_passed = 0, g_failed = 0;

#define PASS(name)        do { std::printf("  PASS  %s\n", name); ++g_passed; } while(0)
#define FAIL(name, msg)   do { std::printf("  FAIL  %s — %s\n", name, msg); ++g_failed; } while(0)
#define CHECK(name, cond, msg) do { if (cond) PASS(name); else FAIL(name, msg); } while(0)

int main() {
    std::printf("=== GoCLPushConstantTest ===\n\n");

    // -----------------------------------------------------------------------
    // 1. Bootstrap Vulkan instance and GoCL context.
    //    The context provides the device capabilities, including the hardware
    //    push‑constant limit (maxPushConstantsSize).
    // -----------------------------------------------------------------------
    VkApplicationInfo appInfo{};
    appInfo.sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_1;   // Vulkan 1.1 minimum

    VkInstanceCreateInfo instCI{};
    instCI.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instCI.pApplicationInfo = &appInfo;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instCI, nullptr, &instance) != VK_SUCCESS) {
        std::printf("  FATAL: vkCreateInstance failed\n");
        return 1;
    }

    GoCL::GoCLContext ctx{};
    try {
        ctx = GoCL::GoCLContext::Create(instance, 4ull * 1024 * 1024);
    } catch (const std::exception& e) {
        std::printf("  FATAL: GoCLContext::Create threw: %s\n", e.what());
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    // The hardware limit is stored in DeviceCapabilities::maxPushConstantsSize.
    uint32_t maxPush = ctx.caps.maxPushConstantsSize;
    std::printf("  Device: %s  maxPushConstantsSize: %u\n\n",
                ctx.caps.deviceName, maxPush);

    // -----------------------------------------------------------------------
    // Test 1: valid push constant size (≤ hardware limit).
    // Use a safe value (min(64, maxPush)) to ensure we stay within the limit.
    // -----------------------------------------------------------------------
    uint32_t validSize = std::min(64u, maxPush);
    VkPipelineLayout layout = GoCL::PipelineBuilder::CreateLayout(
        ctx.device, VK_NULL_HANDLE, validSize, maxPush);
    CHECK("valid_push_constants_succeeds",
          layout != VK_NULL_HANDLE,
          "expected a valid layout, got VK_NULL_HANDLE");

    if (layout) {
        vkDestroyPipelineLayout(ctx.device, layout, nullptr);
    }

    // -----------------------------------------------------------------------
    // Test 2: oversized push constant size (exceeds hardware limit).
    // Compute a size safely larger than maxPush without overflowing uint32_t.
    // -----------------------------------------------------------------------
    uint32_t oversized = (maxPush < UINT32_MAX - 64) ? maxPush + 64 : UINT32_MAX;
    if (oversized <= maxPush) {
        oversized = maxPush + 1;  // extreme edge case (rare)
    }

    VkPipelineLayout badLayout = GoCL::PipelineBuilder::CreateLayout(
        ctx.device, VK_NULL_HANDLE, oversized, maxPush);
    CHECK("exceeds_limit_returns_null",
          badLayout == VK_NULL_HANDLE,
          "expected VK_NULL_HANDLE for oversized push constants");

    // -----------------------------------------------------------------------
    // Test 3: backward compatibility path (maxPushConstSize=0 disables the guard).
    // Even for a large size (1024 bytes), the engine should allow creation
    // because the caller explicitly bypassed the limit check.
    // -----------------------------------------------------------------------
    VkPipelineLayout noCheck = GoCL::PipelineBuilder::CreateLayout(
        ctx.device, VK_NULL_HANDLE, 1024, 0);
    CHECK("engine_limit_check_can_be_disabled",
          noCheck != VK_NULL_HANDLE,
          "expected valid layout when maxPushConstSize=0");

    if (noCheck) {
        vkDestroyPipelineLayout(ctx.device, noCheck, nullptr);
    }

    // -----------------------------------------------------------------------
    // Test 4: zero push constants (no range). Always succeeds.
    // -----------------------------------------------------------------------
    VkPipelineLayout zero = GoCL::PipelineBuilder::CreateLayout(
        ctx.device, VK_NULL_HANDLE, 0, maxPush);
    CHECK("zero_size_always_succeeds",
          zero != VK_NULL_HANDLE,
          "expected valid layout for zero push constants");

    if (zero) {
        vkDestroyPipelineLayout(ctx.device, zero, nullptr);
    }

    // Clean up.
    ctx.Destroy();
    vkDestroyInstance(instance, nullptr);

    std::printf("\n--- Results: %d passed, %d failed ---\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}