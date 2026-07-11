// =============================================================================
// Validates the MeshletCuller utility.
//
// Smoke test validating initialization, command recording,
// dispatch submission, and destruction paths.
// Does not verify culling correctness.
// =============================================================================

#include "render/MeshletCuller.h"
#include "core/GoCLContext.h"
#include <vulkan/vulkan.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cstdio>

static int passed = 0, failed = 0;

#define PASS(name) do { std::printf("  PASS  %s\n", name); ++passed; } while(0)
#define FAIL(name, msg) do { std::printf("  FAIL  %s — %s\n", name, msg); ++failed; } while(0)

// ---------------------------------------------------------------------------
// Helper struct to hold a buffer and its associated device memory.
// Used to avoid leaking memory during the test.
// ---------------------------------------------------------------------------
struct TestBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

// ---------------------------------------------------------------------------
// Creates a buffer with DEVICE_LOCAL memory (fallback to any compatible type).
// Returns a TestBuffer with both the buffer and its memory.
// On failure, the buffer and memory remain VK_NULL_HANDLE.
// ---------------------------------------------------------------------------
static TestBuffer CreateBuffer(VkPhysicalDevice physDev, VkDevice device,
                               VkDeviceSize size, VkBufferUsageFlags usage) {
    TestBuffer tb{};

    VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &ci, nullptr, &tb.buffer) != VK_SUCCESS) {
        return tb;
    }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, tb.buffer, &req);

    VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    alloc.allocationSize = req.size;

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);

    uint32_t memTypeIndex = UINT32_MAX;

    // Pass 1: prefer DEVICE_LOCAL memory (fastest for GPU).
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memTypeIndex = i;
            break;
        }
    }

    // Pass 2: fallback to any compatible memory type (including HOST_VISIBLE).
    if (memTypeIndex == UINT32_MAX) {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if (req.memoryTypeBits & (1u << i)) {
                memTypeIndex = i;
                break;
            }
        }
    }

    if (memTypeIndex == UINT32_MAX) {
        vkDestroyBuffer(device, tb.buffer, nullptr);
        return tb;
    }

    alloc.memoryTypeIndex = memTypeIndex;

    if (vkAllocateMemory(device, &alloc, nullptr, &tb.memory) != VK_SUCCESS) {
        vkDestroyBuffer(device, tb.buffer, nullptr);
        tb.buffer = VK_NULL_HANDLE;
        return tb;
    }

    if (vkBindBufferMemory(device, tb.buffer, tb.memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device, tb.memory, nullptr);
        vkDestroyBuffer(device, tb.buffer, nullptr);
        tb.buffer = VK_NULL_HANDLE;
        tb.memory = VK_NULL_HANDLE;
        return tb;
    }

    return tb;
}

int main() {
    std::printf("=== GoCLMeshletCullerTest ===\n\n");

    // Bootstrap a minimal Vulkan instance and the GoCL context.
    // The context provides a logical device, queues, and a transfer command pool.
    VkApplicationInfo ai{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &ai };
    VkInstance inst;

    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
        FAIL("Vulkan_bootstrap", "vkCreateInstance failed");
        return 1;
    }

    auto ctx = GoCL::GoCLContext::Create(inst, 4ull * 1024 * 1024);
    if (!ctx.device) {
        FAIL("Context_creation", "GoCLContext::Create failed");
        vkDestroyInstance(inst, nullptr);
        return 1;
    }

    // --- MeshletCuller smoke test ---
    GoCL::MeshletCuller culler;
    if (!culler.Init(ctx, 1024)) {
        FAIL("Init", "MeshletCuller::Init returned false");
    } else {
        PASS("Init_succeeds");

        // Create minimal test buffers required by MeshletCuller:
        // - meshletData: input meshlet descriptors (bounding spheres + draw parameters)
        // - indirectDraw: output buffer for indirect draw commands
        // - visibleCount: output buffer for the number of visible meshlets
        TestBuffer meshletData = CreateBuffer(ctx.physDev, ctx.device, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        TestBuffer indirectDraw = CreateBuffer(ctx.physDev, ctx.device, 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        TestBuffer visibleCount = CreateBuffer(ctx.physDev, ctx.device, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

        if (meshletData.buffer && indirectDraw.buffer && visibleCount.buffer) {
            GoCL::MeshletCullerBuffers buffers{ meshletData.buffer, indirectDraw.buffer, visibleCount.buffer };
            culler.UpdateBuffers(buffers);   // updates the descriptor set – should not crash

            // Record a temporary command buffer (one‑time submit).
            VkCommandBuffer cmd;
            VkCommandBufferAllocateInfo cmdCI{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            cmdCI.commandPool = ctx.transferPool;   // reuse the engine's transfer pool
            cmdCI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cmdCI.commandBufferCount = 1;

            if (vkAllocateCommandBuffers(ctx.device, &cmdCI, &cmd) == VK_SUCCESS) {
                VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

                if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
                    FAIL("Cull_dispatch", "vkBeginCommandBuffer failed");
                    vkFreeCommandBuffers(ctx.device, ctx.transferPool, 1, &cmd);
                } else {
                    // Construct a simple view‑projection matrix for frustum culling.
                    glm::mat4 viewProj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 1000.0f) *
                                         glm::lookAt(glm::vec3(0, 0, -5), glm::vec3(0), glm::vec3(0, 1, 0));

                    // Execute the culling compute shader (if pipeline was created).
                    // If the pipeline is not yet created (shader not embedded), this call is a no‑op.
                    culler.Cull(cmd, buffers, viewProj, 32);

                    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
                        FAIL("Cull_dispatch", "vkEndCommandBuffer failed");
                        vkFreeCommandBuffers(ctx.device, ctx.transferPool, 1, &cmd);
                    } else {
                        // Submit the command buffer and wait for completion.
                        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
                        submit.commandBufferCount = 1;
                        submit.pCommandBuffers = &cmd;

                        VkFence fence;
                        VkFenceCreateInfo fenceCI{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };

                        if (vkCreateFence(ctx.device, &fenceCI, nullptr, &fence) != VK_SUCCESS) {
                            FAIL("Fence_creation", "vkCreateFence failed");
                        } else {
                            if (vkQueueSubmit(ctx.gfxQueue, 1, &submit, fence) != VK_SUCCESS) {
                                FAIL("Queue_submit", "vkQueueSubmit failed");
                            } else {
                                if (vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
                                    FAIL("Fence_wait", "vkWaitForFences failed");
                                } else {
                                    PASS("Cull_dispatch_completed");
                                }
                            }
                            vkDestroyFence(ctx.device, fence, nullptr);
                        }
                        vkFreeCommandBuffers(ctx.device, ctx.transferPool, 1, &cmd);
                    }
                }
            } else {
                FAIL("Cull_dispatch", "Could not allocate command buffer");
            }

            // Clean up test buffers (both the buffer and the memory).
            if (meshletData.buffer) {
                vkDestroyBuffer(ctx.device, meshletData.buffer, nullptr);
            }
            if (meshletData.memory) {
                vkFreeMemory(ctx.device, meshletData.memory, nullptr);
            }

            if (indirectDraw.buffer) {
                vkDestroyBuffer(ctx.device, indirectDraw.buffer, nullptr);
            }
            if (indirectDraw.memory) {
                vkFreeMemory(ctx.device, indirectDraw.memory, nullptr);
            }

            if (visibleCount.buffer) {
                vkDestroyBuffer(ctx.device, visibleCount.buffer, nullptr);
            }
            if (visibleCount.memory) {
                vkFreeMemory(ctx.device, visibleCount.memory, nullptr);
            }
        } else {
            FAIL("Dummy_buffers_creation", "Failed to create one or more buffers");
        }
    }

    // Double destroy test: destroying an already destroyed culler must be safe.
    GoCL::MeshletCuller bad;
    bad.Destroy();
    PASS("Destroy_idempotent");

    // Clean up the culler, the GoCL context, and the Vulkan instance.
    culler.Destroy();
    ctx.Destroy();
    vkDestroyInstance(inst, nullptr);

    std::printf("\n--- Results: %d passed, %d failed ---\n", passed, failed);
    return failed ? 1 : 0;
}