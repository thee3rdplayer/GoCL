#include "GoCLContext.h"
#include "../src/render/VRAMBudgetTracker.h"
 
#include <cstring>
#include <fstream>
#include <vector>


namespace GoCL {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
static uint32_t FindQueueFamily(VkPhysicalDevice pd, VkQueueFlags required) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, props.data());
    for (uint32_t i = 0; i < count; ++i)
        if ((props[i].queueFlags & required) == required) return i;
    throw std::runtime_error("GoCLContext: required queue family not found");
}

static uint32_t FindMemType(VkPhysicalDevice pd,
                             uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(pd, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    throw std::runtime_error("GoCLContext: no suitable memory type");
}

// ---------------------------------------------------------------------------
// GoCLContext::Create
// ---------------------------------------------------------------------------
GoCLContext GoCLContext::Create(VkInstance instance, VkDeviceSize stagingBytes) {
    GoCLContext ctx{};
    ctx.instance = instance;

    // 3-tier: discrete → integrated → software (Lavapipe/SwiftShader)
    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
    std::vector<VkPhysicalDevice> devs(devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, devs.data());

    VkPhysicalDevice discrete   = VK_NULL_HANDLE;
    VkPhysicalDevice integrated = VK_NULL_HANDLE;
    VkPhysicalDevice software   = VK_NULL_HANDLE;

    for (auto pd : devs) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(pd, &p);
        switch (p.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                if (discrete == VK_NULL_HANDLE) discrete = pd;   break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                if (integrated == VK_NULL_HANDLE) integrated = pd; break;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:
                if (software == VK_NULL_HANDLE) software = pd;   break;
            default: break;
        }
    }

    ctx.physDev = discrete   ? discrete   :
                integrated ? integrated :
                software   ? software   : VK_NULL_HANDLE;

    if (ctx.physDev == VK_NULL_HANDLE)
        throw std::runtime_error("GoCLContext: no Vulkan device found");

    // Capability oracle
    ctx.caps = CapabilityOracle::Query(ctx.physDev);

    // Queue families — prefer dedicated compute
    ctx.gfxFamily     = FindQueueFamily(ctx.physDev, VK_QUEUE_GRAPHICS_BIT);
    ctx.computeFamily = FindQueueFamily(ctx.physDev, VK_QUEUE_COMPUTE_BIT);

    const float prio = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> qCIs;
    auto addQ = [&](uint32_t family) {
        for (auto& q : qCIs) if (q.queueFamilyIndex == family) return;
        VkDeviceQueueCreateInfo ci{};
        ci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        ci.queueFamilyIndex = family;
        ci.queueCount       = 1;
        ci.pQueuePriorities = &prio;
        qCIs.push_back(ci);
    };
    addQ(ctx.gfxFamily);
    addQ(ctx.computeFamily);

    // Required extensions
    const char* exts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
    };

    // Feature chains
    VkPhysicalDeviceShaderFloat16Int8Features fp16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES };
    fp16.shaderFloat16 = ctx.caps.fp16Native  ? VK_TRUE : VK_FALSE;
    fp16.shaderInt8    = ctx.caps.int8Support ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceDescriptorIndexingFeatures descIdx{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
    descIdx.pNext = &fp16;
    descIdx.shaderSampledImageArrayNonUniformIndexing  = ctx.caps.descriptorTier2;
    descIdx.runtimeDescriptorArray                     = ctx.caps.descriptorTier2;
    descIdx.descriptorBindingVariableDescriptorCount   = ctx.caps.descriptorTier2;

    VkPhysicalDeviceFeatures2 feat2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    feat2.pNext = &descIdx;
    feat2.features.textureCompressionASTC_LDR = ctx.caps.astcLdrHW;
    feat2.features.textureCompressionETC2     = ctx.caps.etc2Support;
    feat2.features.shaderInt16                = ctx.caps.shaderInt16;

    VkDeviceCreateInfo devCI{};
    devCI.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    devCI.pNext                   = &feat2;
    devCI.queueCreateInfoCount    = static_cast<uint32_t>(qCIs.size());
    devCI.pQueueCreateInfos       = qCIs.data();
    devCI.enabledExtensionCount   = 2;
    devCI.ppEnabledExtensionNames = exts;

    if (vkCreateDevice(ctx.physDev, &devCI, nullptr, &ctx.device) != VK_SUCCESS)
        throw std::runtime_error("GoCLContext: vkCreateDevice failed");

    vkGetDeviceQueue(ctx.device, ctx.gfxFamily,     0, &ctx.gfxQueue);
    vkGetDeviceQueue(ctx.device, ctx.computeFamily, 0, &ctx.computeQueue);

    // Pipeline cache (empty — caller may call LoadPipelineCache after)
    VkPipelineCacheCreateInfo pcCI{};
    pcCI.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    vkCreatePipelineCache(ctx.device, &pcCI, nullptr, &ctx.pipelineCache);

    // Staging buffer — HOST_VISIBLE | HOST_COHERENT
    VkBufferCreateInfo bufCI{};
    bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCI.size  = stagingBytes;
    bufCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(ctx.device, &bufCI, nullptr, &ctx.stagingBuffer);

    VkMemoryRequirements memReq{};
    vkGetBufferMemoryRequirements(ctx.device, ctx.stagingBuffer, &memReq);

    VkMemoryAllocateInfo allocCI{};
    allocCI.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocCI.allocationSize  = memReq.size;
    allocCI.memoryTypeIndex = FindMemType(ctx.physDev, memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(ctx.device, &allocCI, nullptr, &ctx.stagingMemory);
    vkBindBufferMemory(ctx.device, ctx.stagingBuffer, ctx.stagingMemory, 0);
    ctx.stagingSize = stagingBytes;

    // Transfer command pool
    VkCommandPoolCreateInfo poolCI{};
    poolCI.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCI.queueFamilyIndex = ctx.gfxFamily;
    poolCI.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    vkCreateCommandPool(ctx.device, &poolCI, nullptr, &ctx.transferPool);

    return ctx;
}

// ---------------------------------------------------------------------------
// GoCLContext::UploadTexture
// Staging → OPTIMAL tiling. Handles all compressed formats correctly.
// ---------------------------------------------------------------------------
VkImage GoCLContext::UploadTexture(
    VkFormat fmt, uint32_t w, uint32_t h,
    const void* data, VkDeviceSize dataSize) const
{
    if (dataSize > stagingSize)
        throw std::runtime_error("GoCLContext::UploadTexture: data exceeds staging buffer");

    // Copy into staging
    void* mapped = nullptr;
    vkMapMemory(device, stagingMemory, 0, dataSize, 0, &mapped);
    std::memcpy(mapped, data, dataSize);
    vkUnmapMemory(device, stagingMemory);

    // Create OPTIMAL image
    VkImageCreateInfo imgCI{};
    imgCI.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType     = VK_IMAGE_TYPE_2D;
    imgCI.format        = fmt;
    imgCI.extent        = { w, h, 1 };
    imgCI.mipLevels     = 1;
    imgCI.arrayLayers   = 1;
    imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;  // GPU-native, works for all compressed fmts
    imgCI.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    vkCreateImage(device, &imgCI, nullptr, &image);

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(device, image, &memReq);

    VkMemoryAllocateInfo allocCI{};
    allocCI.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocCI.allocationSize  = memReq.size;
    allocCI.memoryTypeIndex = FindMemType(physDev, memReq.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkDeviceMemory mem = VK_NULL_HANDLE;
    vkAllocateMemory(device, &allocCI, nullptr, &mem);
    vkBindImageMemory(device, image, mem, 0);

    // One-shot command buffer: UNDEFINED→TRANSFER_DST, copy, TRANSFER_DST→SHADER_READ
    VkCommandBufferAllocateInfo cbAlloc{};
    cbAlloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAlloc.commandPool        = transferPool;
    cbAlloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandBufferCount = 1;

    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &cbAlloc, &cb);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);

    // Barrier: UNDEFINED → TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image               = image;
    toTransfer.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    toTransfer.srcAccessMask       = 0;
    toTransfer.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    // Copy staging → image
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent      = { w, h, 1 };
    vkCmdCopyBufferToImage(cb, stagingBuffer, image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Barrier: TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
    VkImageMemoryBarrier toShader = toTransfer;
    toShader.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toShader);

    vkEndCommandBuffer(cb);

    // Submit and wait (synchronous — async compute queue path is future work)
    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cb;

    VkFenceCreateInfo fenceCI{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(device, &fenceCI, nullptr, &fence);
    vkQueueSubmit(gfxQueue, 1, &submit, fence);
    vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, transferPool, 1, &cb);

    return image;
}

// ---------------------------------------------------------------------------
// VRAMRemainingMB
// ---------------------------------------------------------------------------
uint32_t GoCLContext::VRAMRemainingMB() const {
    VRAMBudgetTracker tracker(physDev);
    return tracker.RemainingMB();
}

// ---------------------------------------------------------------------------
// Pipeline cache persistence
// ---------------------------------------------------------------------------
void GoCLContext::SavePipelineCache(const char* path) const {
    size_t size = 0;
    vkGetPipelineCacheData(device, pipelineCache, &size, nullptr);
    std::vector<uint8_t> data(size);
    vkGetPipelineCacheData(device, pipelineCache, &size, data.data());
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), size);
}

void GoCLContext::LoadPipelineCache(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return; // first run — no cache yet
    std::streamsize sz = f.tellg(); f.seekg(0);
    std::vector<uint8_t> data(sz);
    f.read(reinterpret_cast<char*>(data.data()), sz);

    if (pipelineCache != VK_NULL_HANDLE)
        vkDestroyPipelineCache(device, pipelineCache, nullptr);

    VkPipelineCacheCreateInfo ci{};
    ci.sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    ci.initialDataSize = data.size();
    ci.pInitialData    = data.data();
    vkCreatePipelineCache(device, &ci, nullptr, &pipelineCache);
}

// ---------------------------------------------------------------------------
// Destroy
// ---------------------------------------------------------------------------
void GoCLContext::Destroy() {
    if (device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(device);
    if (transferPool   != VK_NULL_HANDLE) vkDestroyCommandPool(device, transferPool, nullptr);
    if (stagingBuffer  != VK_NULL_HANDLE) vkDestroyBuffer(device, stagingBuffer, nullptr);
    if (stagingMemory  != VK_NULL_HANDLE) vkFreeMemory(device, stagingMemory, nullptr);
    if (pipelineCache  != VK_NULL_HANDLE) vkDestroyPipelineCache(device, pipelineCache, nullptr);
    vkDestroyDevice(device, nullptr);
    device = VK_NULL_HANDLE;
}

} // namespace GoCL