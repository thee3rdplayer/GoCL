#include "GoCLContext.h"
#include "ConfigSingleton.h"
#include "render/VRAMBudgetTracker.h"

#include <cstring>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <cstdlib>
#include <unordered_map>

namespace GoCL {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/**
 * @brief Finds the first queue family that supports the required flags (e.g., graphics).
 */
static uint32_t FindQueueFamily(VkPhysicalDevice pd, VkQueueFlags required) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, props.data());
    for (uint32_t i = 0; i < count; ++i) {
        if ((props[i].queueFlags & required) == required) return i;
    }
    throw std::runtime_error("GoCLContext: required queue family not found");
}

/**
 * @brief Prefers a dedicated compute queue (no graphics bit) for async compute.
 */
static uint32_t FindBestComputeFamily(VkPhysicalDevice pd) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, props.data());

    for (uint32_t i = 0; i < count; ++i) {
        if ((props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            !(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            return i;
        }
    }
    return FindQueueFamily(pd, VK_QUEUE_GRAPHICS_BIT);
}

/**
 * @brief Finds a memory type that satisfies both the required bits and property flags.
 */
static uint32_t FindMemType(VkPhysicalDevice pd,
                             uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(pd, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    throw std::runtime_error("GoCLContext: no suitable memory type");
}

/**
 * @brief Finds a queue family that supports present to the given surface.
 * Prefers one that also supports graphics (common on desktop), otherwise any present family.
 */
static uint32_t FindPresentFamily(VkPhysicalDevice pd, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, props.data());

    for (uint32_t i = 0; i < count; ++i) {
        VkBool32 supportsPresent = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &supportsPresent);
        if (supportsPresent && (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            return i;
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        VkBool32 supportsPresent = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &supportsPresent);
        if (supportsPresent) return i;
    }
    throw std::runtime_error("GoCLContext: no present queue family");
}

// ---------------------------------------------------------------------------
// Create
// ---------------------------------------------------------------------------

GoCLContext GoCLContext::Create(VkInstance instance, VkDeviceSize stagingBytesParam) {
    GoCLContext ctx{};
    ctx.instance = instance;

    try {
        // ----- 1. Choose the best physical device (discrete > integrated > software) -----
        uint32_t devCount = 0;
        VkResult r = vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
        if (r != VK_SUCCESS) {
            throw std::runtime_error("GoCLContext: vkEnumeratePhysicalDevices(count) failed");
        }

        if (devCount == 0) {
            throw std::runtime_error("GoCLContext: no Vulkan devices found");
        }

        std::vector<VkPhysicalDevice> devs(devCount);
        r = vkEnumeratePhysicalDevices(instance, &devCount, devs.data());
        if (r != VK_SUCCESS) {
            throw std::runtime_error("GoCLContext: vkEnumeratePhysicalDevices(list) failed");
        }

        VkPhysicalDevice discrete   = VK_NULL_HANDLE;
        VkPhysicalDevice integrated = VK_NULL_HANDLE;
        VkPhysicalDevice software   = VK_NULL_HANDLE;
        for (auto pd : devs) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(pd, &p);
            switch (p.deviceType) {
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                    if (discrete == VK_NULL_HANDLE) discrete = pd;
                    break;
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                    if (integrated == VK_NULL_HANDLE) integrated = pd;
                    break;
                case VK_PHYSICAL_DEVICE_TYPE_CPU:
                    if (software == VK_NULL_HANDLE) software = pd;
                    break;
                default: break;
            }
        }
        ctx.physDev = discrete ? discrete : integrated ? integrated : software ? software : VK_NULL_HANDLE;
        if (ctx.physDev == VK_NULL_HANDLE) {
            throw std::runtime_error("GoCLContext: no Vulkan device found");
        }

        // Cache device properties for later use (e.g., PipelineBuilder).
        vkGetPhysicalDeviceProperties(ctx.physDev, &ctx.devProps);

        // Capture all hardware/driver capabilities in a single snapshot.
        ctx.caps = CapabilityOracle::Query(ctx.physDev);

        // ----- 2. Queue families and device creation -----
        ctx.gfxFamily     = FindQueueFamily(ctx.physDev, VK_QUEUE_GRAPHICS_BIT);
        ctx.computeFamily = FindBestComputeFamily(ctx.physDev);

        // Merge families to avoid duplicate VkDeviceQueueCreateInfo entries.
        std::unordered_map<uint32_t, uint32_t> familyCounts;
        familyCounts[ctx.gfxFamily]++;
        familyCounts[ctx.computeFamily]++;

        std::vector<VkDeviceQueueCreateInfo> qCIs;
        std::vector<std::vector<float>> queuePrios; // keep priorities alive
        queuePrios.reserve(familyCounts.size());
        qCIs.reserve(familyCounts.size());
        for (auto& [family, count] : familyCounts) {
            queuePrios.emplace_back(count, 1.0f);
            VkDeviceQueueCreateInfo ci{};
            ci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            ci.queueFamilyIndex = family;
            ci.queueCount       = count;
            ci.pQueuePriorities = queuePrios.back().data();
            qCIs.push_back(ci);
        }

        // ----- 3. Required and optional extensions -----
        std::vector<const char*> exts{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };

        // Detect VK_EXT_memory_budget for VRAM tracking.
        {
            uint32_t extCount = 0;
            vkEnumerateDeviceExtensionProperties(ctx.physDev, nullptr, &extCount, nullptr);
            std::vector<VkExtensionProperties> devExts(extCount);
            vkEnumerateDeviceExtensionProperties(ctx.physDev, nullptr, &extCount, devExts.data());
            for (auto& e : devExts) {
                if (std::strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) {
                    ctx.hasMemoryBudgetExt = true;
                    break;
                }
            }
        }

        // If we have the extension, add it to the list.
        if (ctx.hasMemoryBudgetExt) {
            exts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        }

        if (ctx.caps.dgcSupported) {
            exts.push_back(VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME);
        }

        // For Vulkan 1.1 devices (or 1.0 with extensions), we need to enable feature extensions.
        if (ctx.caps.apiVersion < VK_API_VERSION_1_2) {
            if ((ctx.caps.fp16Native || ctx.caps.int8Support) &&
                CapabilityOracle::CheckExtension(ctx.physDev, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME)) {
                exts.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
            }

            if (ctx.caps.bindlessDescriptors &&
                CapabilityOracle::CheckExtension(ctx.physDev, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME)) {
                exts.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
            }

            if (ctx.caps.shaderInt16 &&
                CapabilityOracle::CheckExtension(ctx.physDev, VK_KHR_16BIT_STORAGE_EXTENSION_NAME)) {
                exts.push_back(VK_KHR_16BIT_STORAGE_EXTENSION_NAME);
            }
        }

        // Optional: VK_KHR_synchronization2 for more flexible pipeline barriers.
        bool hasSync2 = CapabilityOracle::CheckExtension(ctx.physDev, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
        if (hasSync2 && ctx.caps.apiVersion < VK_API_VERSION_1_3) {
            exts.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
        }

        // ----- 4. Query supported features (read‑only) -----
        VkPhysicalDeviceFeatures2 supportedFeat2{};
        supportedFeat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

        VkPhysicalDeviceShaderFloat16Int8Features supportedFP16{};
        supportedFP16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;

        VkPhysicalDeviceDescriptorIndexingFeatures supportedDescIdx{};
        supportedDescIdx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;

        VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT supportedDGC{};
        supportedDGC.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT;

        supportedFeat2.pNext = &supportedFP16;
        supportedFP16.pNext  = &supportedDescIdx;
        supportedDescIdx.pNext = &supportedDGC;

        bool useExtendedFeatures = (ctx.caps.apiVersion >= VK_API_VERSION_1_1) ||
            []() -> bool {
                uint32_t count = 0;
                vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
                std::vector<VkExtensionProperties> props(count);
                vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());
                for (auto& p : props) {
                    if (strcmp(p.extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0) {
                        return true;
                    }
                }
                return false;
            }();

        if (useExtendedFeatures) {
            vkGetPhysicalDeviceFeatures2(ctx.physDev, &supportedFeat2);
        } else {
            auto fp = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2KHR>(
                vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2KHR"));
            if (fp) {
                fp(ctx.physDev, &supportedFeat2);
            } else {
                // Fallback to Vulkan 1.0 features only.
                VkPhysicalDeviceFeatures baseFeat;
                vkGetPhysicalDeviceFeatures(ctx.physDev, &baseFeat);
                supportedFeat2.features = baseFeat;
            }
        }

        // ----- 5. Build enabled features (zero‑initialised, set only what we need) -----
        VkPhysicalDeviceFeatures2 enabledFeat2{};
        enabledFeat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

        VkPhysicalDeviceShaderFloat16Int8Features enabledFP16{};
        enabledFP16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;

        VkPhysicalDeviceDescriptorIndexingFeatures enabledDescIdx{};
        enabledDescIdx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;

        VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT enabledDGC{};
        enabledDGC.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT;
        enabledDGC.deviceGeneratedCommands = ctx.caps.dgcSupported ? VK_TRUE : VK_FALSE;

        enabledFeat2.pNext = &enabledFP16;
        enabledFP16.pNext  = &enabledDescIdx;
        enabledDescIdx.pNext = &enabledDGC;

        // Only enable the core features we actually use; others remain off.
        enabledFeat2.features.textureCompressionASTC_LDR = ctx.caps.astcLdrHW;
        enabledFeat2.features.textureCompressionETC2     = ctx.caps.etc2Support;
        enabledFeat2.features.shaderInt16                = ctx.caps.shaderInt16;

        if (useExtendedFeatures) {
            enabledFP16.shaderFloat16 = ctx.caps.fp16Native  ? supportedFP16.shaderFloat16 : VK_FALSE;
            enabledFP16.shaderInt8    = ctx.caps.int8Support ? supportedFP16.shaderInt8    : VK_FALSE;
            bool bindless = ctx.caps.bindlessDescriptors;
            enabledDescIdx.shaderSampledImageArrayNonUniformIndexing = bindless ? supportedDescIdx.shaderSampledImageArrayNonUniformIndexing : VK_FALSE;
            enabledDescIdx.runtimeDescriptorArray                    = bindless ? supportedDescIdx.runtimeDescriptorArray : VK_FALSE;
            enabledDescIdx.descriptorBindingVariableDescriptorCount  = bindless ? supportedDescIdx.descriptorBindingVariableDescriptorCount : VK_FALSE;
            enabledDescIdx.descriptorBindingPartiallyBound           = bindless ? supportedDescIdx.descriptorBindingPartiallyBound : VK_FALSE;
            enabledDescIdx.shaderStorageBufferArrayNonUniformIndexing = bindless ? supportedDescIdx.shaderStorageBufferArrayNonUniformIndexing : VK_FALSE;
            enabledDescIdx.shaderStorageImageArrayNonUniformIndexing  = bindless ? supportedDescIdx.shaderStorageImageArrayNonUniformIndexing : VK_FALSE;
        }

        // ----- 6. Create the logical device -----
        VkDeviceCreateInfo devCI{};
        devCI.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        devCI.pNext                   = useExtendedFeatures ? &enabledFeat2 : nullptr;
        devCI.queueCreateInfoCount    = static_cast<uint32_t>(qCIs.size());
        devCI.pQueueCreateInfos       = qCIs.data();
        devCI.enabledExtensionCount   = static_cast<uint32_t>(exts.size());
        devCI.ppEnabledExtensionNames = exts.data();

        if (vkCreateDevice(ctx.physDev, &devCI, nullptr, &ctx.device) != VK_SUCCESS) {
            throw std::runtime_error("GoCLContext: vkCreateDevice failed");
        }

        vkGetDeviceQueue(ctx.device, ctx.gfxFamily,     0, &ctx.gfxQueue);
        vkGetDeviceQueue(ctx.device, ctx.computeFamily, 0, &ctx.computeQueue);

        // Get function pointer for Sync2 barriers (may be null if extension not enabled)
        ctx.fpCmdPipelineBarrier2KHR = (PFN_vkCmdPipelineBarrier2KHR)
            vkGetDeviceProcAddr(ctx.device, "vkCmdPipelineBarrier2KHR");

        // Also try the core name if Vulkan 1.3 is used (no KHR suffix)
        if (!ctx.fpCmdPipelineBarrier2KHR) {
            ctx.fpCmdPipelineBarrier2KHR = (PFN_vkCmdPipelineBarrier2KHR)
                vkGetDeviceProcAddr(ctx.device, "vkCmdPipelineBarrier2");
        }

        // ----- 7. Pipeline cache (initial empty, will be loaded later via LoadPipelineCache) -----
        VkPipelineCacheCreateInfo pcCI{};
        pcCI.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        r = vkCreatePipelineCache(ctx.device, &pcCI, nullptr, &ctx.pipelineCache);
        if (r != VK_SUCCESS) {
            throw std::runtime_error("GoCLContext: vkCreatePipelineCache failed");
        }

        // Initialise DGC manager if supported: layout, execution set, and input/preprocess buffers.
        if (ctx.caps.dgcSupported) {
            ctx.dgcManager.init(ctx);
            ctx.dgcManager.initExecutionSet(4); // max 4 different pipelines
            ctx.dgcManager.initBuffers(1024, 128);  // 1024 sequences, 128 bytes push constants
        }

        // ----- 8. Load runtime configuration from GoCL.conf (singleton) -----
        ctx.astcThresholds = GetConfig().astcThresholds;

        // ----- 9. Staging buffer for texture uploads (size may be overridden by config) -----
        VkDeviceSize stagingBytes = GetConfig().stagingSizeMB
            ? static_cast<VkDeviceSize>(GetConfig().stagingSizeMB) * 1024ull * 1024ull
            : stagingBytesParam;
        if (stagingBytes == 0) {
            throw std::runtime_error("GoCLContext: staging buffer size is zero");
        }

        VkBufferCreateInfo bufCI{};
        bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufCI.size  = stagingBytes;
        bufCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        r = vkCreateBuffer(ctx.device, &bufCI, nullptr, &ctx.stagingBuffer);
        if (r != VK_SUCCESS) {
            throw std::runtime_error("GoCLContext: vkCreateBuffer (staging) failed");
        }

        VkMemoryRequirements memReq{};
        vkGetBufferMemoryRequirements(ctx.device, ctx.stagingBuffer, &memReq);
        VkMemoryAllocateInfo allocCI{};
        allocCI.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocCI.allocationSize  = memReq.size;
        allocCI.memoryTypeIndex = FindMemType(ctx.physDev, memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        r = vkAllocateMemory(ctx.device, &allocCI, nullptr, &ctx.stagingMemory);
        if (r != VK_SUCCESS) {
            throw std::runtime_error("GoCLContext: vkAllocateMemory (staging) failed");
        }

        r = vkBindBufferMemory(ctx.device, ctx.stagingBuffer, ctx.stagingMemory, 0);
        if (r != VK_SUCCESS) {
            throw std::runtime_error("GoCLContext: vkBindBufferMemory (staging) failed");
        }
        ctx.stagingSize = stagingBytes;

        // ----- 10. Command pool for one‑shot transfer operations (uploading textures) -----
        VkCommandPoolCreateInfo poolCI{};
        poolCI.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolCI.queueFamilyIndex = ctx.gfxFamily;
        poolCI.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT
                                | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        r = vkCreateCommandPool(ctx.device, &poolCI, nullptr, &ctx.transferPool);
        if (r != VK_SUCCESS) {
            throw std::runtime_error("GoCLContext: vkCreateCommandPool (transfer) failed");
        }

        return ctx;
    } catch (...) {
        ctx.Destroy();  // Clean up any partially created resources.
        throw;
    }
}

// ---------------------------------------------------------------------------
// SetupPresentQueue – must be called after surface creation.
// Returns false if no present‑capable queue family can be found.
// ---------------------------------------------------------------------------

bool GoCLContext::SetupPresentQueue(VkSurfaceKHR surface) {
    try {
        presentFamily = FindPresentFamily(physDev, surface);
        vkGetDeviceQueue(device, presentFamily, 0, &presentQueue);
        return presentQueue != VK_NULL_HANDLE;
    } catch (const std::exception&) {
        presentFamily = 0;
        presentQueue = VK_NULL_HANDLE;
        return false;
    }
}

// ---------------------------------------------------------------------------
// UploadTexture – copies data to a device‑local image via staging buffer.
// Returns the VkImage and sets *outMemory to the bound device memory.
// The caller must destroy both with DestroyTexture().
// ---------------------------------------------------------------------------

VkImage GoCLContext::UploadTexture(
    VkFormat fmt, uint32_t w, uint32_t h,
    const void* data, VkDeviceSize dataSize,
    VkDeviceMemory* outMemory) const {
    if (dataSize > stagingSize) {
        throw std::runtime_error("GoCLContext::UploadTexture: data exceeds staging buffer");
    }

    // Copy source data into the staging buffer.
    void* mapped = nullptr;
    vkMapMemory(device, stagingMemory, 0, dataSize, 0, &mapped);
    std::memcpy(mapped, data, dataSize);
    vkUnmapMemory(device, stagingMemory);

    // Create a device‑local image (optimal tiling) to hold the final texture.
    VkImageCreateInfo imgCI{};
    imgCI.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType     = VK_IMAGE_TYPE_2D;
    imgCI.format        = fmt;
    imgCI.extent        = { w, h, 1 };
    imgCI.mipLevels     = 1;
    imgCI.arrayLayers   = 1;
    imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    VkResult r = vkCreateImage(device, &imgCI, nullptr, &image);
    if (r != VK_SUCCESS) {
        throw std::runtime_error("UploadTexture: vkCreateImage failed");
    }

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(device, image, &memReq);
    VkMemoryAllocateInfo allocCI{};
    allocCI.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocCI.allocationSize  = memReq.size;
    allocCI.memoryTypeIndex = FindMemType(physDev, memReq.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult memRes = vkAllocateMemory(device, &allocCI, nullptr, &memory);
    if (memRes != VK_SUCCESS) {
        vkDestroyImage(device, image, nullptr);
        throw std::runtime_error("GoCLContext::UploadTexture: failed to allocate device memory");
    }
    r = vkBindImageMemory(device, image, memory, 0);
    if (r != VK_SUCCESS) {
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, memory, nullptr);
        throw std::runtime_error("UploadTexture: vkBindImageMemory failed");
    }

    // One‑shot command buffer to record the copy and layout transitions.
    VkCommandBufferAllocateInfo cbAlloc{};
    cbAlloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAlloc.commandPool        = transferPool;
    cbAlloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    r = vkAllocateCommandBuffers(device, &cbAlloc, &cb);
    if (r != VK_SUCCESS) {
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, memory, nullptr);
        throw std::runtime_error("UploadTexture: vkAllocateCommandBuffers failed");
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r = vkBeginCommandBuffer(cb, &begin);
    if (r != VK_SUCCESS) {
        vkFreeCommandBuffers(device, transferPool, 1, &cb);
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, memory, nullptr);
        throw std::runtime_error("UploadTexture: vkBeginCommandBuffer failed");
    }

    // Use either VK_KHR_synchronization2 or legacy barriers based on availability.
    bool hasSync2 = CapabilityOracle::CheckExtension(physDev, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    if (hasSync2 && fpCmdPipelineBarrier2KHR) {
        // Sync2 path: fewer parameters, clearer semantics.
        VkImageMemoryBarrier2KHR toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR;
        toTransfer.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT_KHR;
        toTransfer.srcAccessMask = 0;
        toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR;
        toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = image;
        toTransfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkDependencyInfoKHR depInfo{};
        depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR;
        depInfo.imageMemoryBarrierCount = 1;
        depInfo.pImageMemoryBarriers = &toTransfer;
        fpCmdPipelineBarrier2KHR(cb, &depInfo);

        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { w, h, 1 };
        vkCmdCopyBufferToImage(cb, stagingBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        toTransfer.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toTransfer.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR;
        toTransfer.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR;
        toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT_KHR;
        toTransfer.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT_KHR;
        fpCmdPipelineBarrier2KHR(cb, &depInfo);
    } else {
        // Legacy barrier path.
        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = image;
        toTransfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        toTransfer.srcAccessMask = 0;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toTransfer);

        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { w, h, 1 };
        vkCmdCopyBufferToImage(cb, stagingBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier toShader = toTransfer;
        toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toShader);
    }

    r = vkEndCommandBuffer(cb);
    if (r != VK_SUCCESS) {
        vkFreeCommandBuffers(device, transferPool, 1, &cb);
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, memory, nullptr);
        throw std::runtime_error("UploadTexture: vkEndCommandBuffer failed");
    }

    // Submit the transfer and wait with a 5‑second timeout to avoid hanging.
    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cb;
    VkFenceCreateInfo fenceCI{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    r = vkCreateFence(device, &fenceCI, nullptr, &fence);
    if (r != VK_SUCCESS) {
        vkFreeCommandBuffers(device, transferPool, 1, &cb);
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, memory, nullptr);
        throw std::runtime_error("UploadTexture: vkCreateFence failed");
    }
    r = vkQueueSubmit(gfxQueue, 1, &submit, fence);
    if (r != VK_SUCCESS) {
        vkDestroyFence(device, fence, nullptr);
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, memory, nullptr);
        throw std::runtime_error("UploadTexture: vkQueueSubmit failed");
    }
    r = vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000ULL);
    if (r == VK_TIMEOUT) {
        vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, transferPool, 1, &cb);
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, memory, nullptr);
        throw std::runtime_error("UploadTexture: fence wait timeout");
    }
    if (r != VK_SUCCESS) {
        vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, transferPool, 1, &cb);
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, memory, nullptr);
        throw std::runtime_error("UploadTexture: vkWaitForFences failed");
    }

    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, transferPool, 1, &cb);
    *outMemory = memory;
    return image;
}

void GoCLContext::DestroyTexture(VkImage image, VkDeviceMemory memory) const {
    if (image)   vkDestroyImage(device, image, nullptr);
    if (memory)  vkFreeMemory(device, memory, nullptr);
}

uint32_t GoCLContext::VRAMRemainingMB() const {
    VRAMBudgetTracker tracker(physDev, hasMemoryBudgetExt);
    return tracker.RemainingMB();
}

// ---------------------------------------------------------------------------
// Pipeline cache persistence – store/load with header validation.
// ---------------------------------------------------------------------------

void GoCLContext::SavePipelineCache(const char* path) const {
    if (!pipelineCache) return;
    size_t dataSize = 0;
    VkResult r = vkGetPipelineCacheData(device, pipelineCache, &dataSize, nullptr);
    if (r != VK_SUCCESS) return;
    std::vector<uint8_t> data(dataSize);
    r = vkGetPipelineCacheData(device, pipelineCache, &dataSize, data.data());
    if (r != VK_SUCCESS) return;

    PipelineCacheHeader header;
    header.vendorID = caps.vendorID;
    header.deviceID = caps.deviceID;
    header.driverVersion = caps.driverVersion;
    memcpy(header.pipelineCacheUUID, caps.pipelineCacheUUID, VK_UUID_SIZE);

    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(&header), sizeof(header));
    f.write(reinterpret_cast<const char*>(data.data()), dataSize);
}

void GoCLContext::LoadPipelineCache(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return;
    std::streamsize totalSize = f.tellg();
    f.seekg(0);
    if (totalSize < sizeof(PipelineCacheHeader)) return;

    PipelineCacheHeader header;
    f.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!f) return;

    // Validate the header matches the current device.
    if (header.vendorID != caps.vendorID ||
        header.driverVersion != caps.driverVersion ||
        memcmp(header.pipelineCacheUUID, caps.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
        return;
    }

    std::vector<uint8_t> data(static_cast<size_t>(totalSize - sizeof(header)));
    f.read(reinterpret_cast<char*>(data.data()), data.size());
    if (!f) return;

    VkPipelineCacheCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    ci.initialDataSize = data.size();
    ci.pInitialData = data.data();
    if (pipelineCache) vkDestroyPipelineCache(device, pipelineCache, nullptr);
    vkCreatePipelineCache(device, &ci, nullptr, &pipelineCache);
}

// ---------------------------------------------------------------------------
// Destroy – clean up all Vulkan resources owned by this context.
// Idempotent and safe to call even if some members are already destroyed.
// ---------------------------------------------------------------------------

void GoCLContext::Destroy() {
    if (device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(device);

    dgcManager.destroy();   // Clean up DGC objects (layout, execution set, buffers) before device destruction

    if (transferPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, transferPool, nullptr);
        transferPool = VK_NULL_HANDLE;
    }
    if (stagingBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        stagingBuffer = VK_NULL_HANDLE;
    }
    if (stagingMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, stagingMemory, nullptr);
        stagingMemory = VK_NULL_HANDLE;
    }
    if (pipelineCache != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(device, pipelineCache, nullptr);
        pipelineCache = VK_NULL_HANDLE;
    }
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }

    // Queue handles become invalid after device destruction.
    gfxQueue = VK_NULL_HANDLE;
    computeQueue = VK_NULL_HANDLE;
    presentQueue = VK_NULL_HANDLE;

    gfxFamily = 0;
    computeFamily = 0;
    presentFamily = 0;
}

}   // namespace GoCL