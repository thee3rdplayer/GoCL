#include "FrameLoop.h"
#include "core/ConfigSingleton.h"
#include "core/DGCManager.h"

#include <stdexcept>
#include <cstdio>
#include <cstring>

namespace GoCL {

// ---------------------------------------------------------------------------
// Helper: find a memory type index that matches the given typeFilter and properties.
// Used for depth image allocation.
// ---------------------------------------------------------------------------
uint32_t FrameLoop::findMemoryType(VkPhysicalDevice gpu,
                                    uint32_t typeFilter,
                                    VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    throw std::runtime_error("GoCL::FrameLoop — no suitable memory type");
}

// ---------------------------------------------------------------------------
// Initialises the frame loop: creates swapchain, render pass, synchronisation,
// command pools, depth resources and framebuffers.
// The number of in‑flight frames is read from GoCL.conf (max_frames_in_flight);
// defaults to 2 if not set or zero.
// ---------------------------------------------------------------------------
bool FrameLoop::init(GoCLContext& ctx, VkSurfaceKHR surface,
                     uint32_t width, uint32_t height,
                     const FrameLoopConfig& cfg) {
    const auto& rc = GetConfig();   // singleton config (GoCL.conf)
    uint32_t framesInFlight = rc.maxFramesInFlight;
    if (framesInFlight == 0) {
        framesInFlight = 2;    // safe fallback
    }

    m_device      = ctx.device;
    m_graphicsQ   = ctx.gfxQueue;
    m_presentQ    = ctx.presentQueue;

    // Depth format selection: try the preferred format first (given in cfg),
    // then fall back to common depth/stencil formats in order of decreasing quality.
    m_depthFormat = cfg.depthFormat;
    const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D16_UNORM
    };

    bool found = false;
    for (VkFormat fmt : candidates) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(ctx.physDev, fmt, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            m_depthFormat = fmt;
            found = true;
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "GoCL: no supported depth format found\n");
        return false;
    }

    // Create all core rendering objects in order.
    if (!m_swapchain.init(ctx, surface, width, height)) {
        return false;
    }
    if (!m_renderPass.init(ctx, m_swapchain.format(), m_depthFormat)) {
        return false;
    }
    if (!m_frameSync.init(ctx, framesInFlight)) {
        return false;
    }
    if (!m_cmdPool.init(ctx, framesInFlight)) {
        return false;
    }
    if (!createDepthResources(ctx)) {
        return false;
    }
    if (!createFramebuffers(ctx)) {
        return false;
    }

    // Per‑swapchain‑image fence tracking (for synchronising with presentation).
    m_imageInFlight.resize(m_swapchain.imageCount(), VK_NULL_HANDLE);
    return true;
}

// ---------------------------------------------------------------------------
// Creates the depth/stencil image, allocates device‑local memory,
// and creates an image view.
// ---------------------------------------------------------------------------
bool FrameLoop::createDepthResources(GoCLContext& ctx) {
    VkImageCreateInfo imgCI{};
    imgCI.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType     = VK_IMAGE_TYPE_2D;
    imgCI.format        = m_depthFormat;
    imgCI.extent        = { m_swapchain.extent().width, m_swapchain.extent().height, 1 };
    imgCI.mipLevels     = 1;
    imgCI.arrayLayers   = 1;
    imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imgCI.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &imgCI, nullptr, &m_depthImage) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(m_device, m_depthImage, &memReq);

    VkMemoryAllocateInfo allocCI{};
    allocCI.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocCI.allocationSize  = memReq.size;
    allocCI.memoryTypeIndex = findMemoryType(ctx.physDev, memReq.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocCI, nullptr, &m_depthMem) != VK_SUCCESS) {
        vkDestroyImage(m_device, m_depthImage, nullptr);
        m_depthImage = VK_NULL_HANDLE;
        return false;
    }

    if (vkBindImageMemory(m_device, m_depthImage, m_depthMem, 0) != VK_SUCCESS) {
        // Memory already allocated; image already created – both must be cleaned.
        vkFreeMemory(m_device, m_depthMem, nullptr);
        vkDestroyImage(m_device, m_depthImage, nullptr);
        m_depthMem   = VK_NULL_HANDLE;
        m_depthImage = VK_NULL_HANDLE;
        return false;
    }

    VkImageViewCreateInfo viewCI{};
    viewCI.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image                           = m_depthImage;
    viewCI.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format                          = m_depthFormat;
    viewCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;

    if (m_depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
        m_depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        viewCI.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }

    viewCI.subresourceRange.baseMipLevel   = 0;
    viewCI.subresourceRange.levelCount     = 1;
    viewCI.subresourceRange.baseArrayLayer = 0;
    viewCI.subresourceRange.layerCount     = 1;

    if (vkCreateImageView(m_device, &viewCI, nullptr, &m_depthView) != VK_SUCCESS) {
        vkFreeMemory(m_device, m_depthMem, nullptr);
        vkDestroyImage(m_device, m_depthImage, nullptr);
        m_depthMem   = VK_NULL_HANDLE;
        m_depthImage = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Creates one framebuffer per swapchain image, attaching the swapchain image
// and the depth image. The render pass determines how attachments are used.
// ---------------------------------------------------------------------------
bool FrameLoop::createFramebuffers(GoCLContext& ctx) {
    const uint32_t count = m_swapchain.imageCount();
    m_framebuffers.resize(count, VK_NULL_HANDLE);

    for (uint32_t i = 0; i < count; ++i) {
        VkImageView attachments[] = { m_swapchain.imageView(i), m_depthView };

        VkFramebufferCreateInfo fbCI{};
        fbCI.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbCI.renderPass      = m_renderPass.handle();
        fbCI.attachmentCount = 2;
        fbCI.pAttachments    = attachments;
        fbCI.width           = m_swapchain.extent().width;
        fbCI.height          = m_swapchain.extent().height;
        fbCI.layers          = 1;

        if (vkCreateFramebuffer(m_device, &fbCI, nullptr, &m_framebuffers[i]) != VK_SUCCESS) {
            return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Clean up depth resources (image, memory, view).
// ---------------------------------------------------------------------------
void FrameLoop::destroyDepthResources() {
    if (m_depthView) {
        vkDestroyImageView(m_device, m_depthView, nullptr);
        m_depthView = VK_NULL_HANDLE;
    }
    if (m_depthImage) {
        vkDestroyImage(m_device, m_depthImage, nullptr);
        m_depthImage = VK_NULL_HANDLE;
    }
    if (m_depthMem) {
        vkFreeMemory(m_device, m_depthMem, nullptr);
        m_depthMem = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Destroy all framebuffers and clear per‑image flight tracking.
// ---------------------------------------------------------------------------
void FrameLoop::destroyFramebuffers() {
    for (auto fb : m_framebuffers) {
        if (fb) {
            vkDestroyFramebuffer(m_device, fb, nullptr);
        }
    }
    m_framebuffers.clear();
    m_imageInFlight.clear();
}

// ---------------------------------------------------------------------------
// Destroys all resources owned by the frame loop in correct order.
// Waits for the device to idle to avoid destroying objects still in use.
// ---------------------------------------------------------------------------
void FrameLoop::destroy() {
    if (!m_device) return;

    vkDeviceWaitIdle(m_device);
    destroyFramebuffers();
    destroyDepthResources();
    m_cmdPool.destroy();
    m_frameSync.destroy();
    m_renderPass.destroy();
    m_swapchain.destroy();
    m_device = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Recreates framebuffers and depth resources after a swapchain resize.
// The old swapchain is destroyed and a new one is created with the new dimensions.
// ---------------------------------------------------------------------------
void FrameLoop::recordDGC(GoCLContext& ctx, VkCommandBuffer cmd, uint32_t frameIndex, const std::vector<DrawData>& draws) {
    if (!ctx.caps.dgcSupported || draws.empty()) {
        return;
    }

    auto& dgc = ctx.dgcManager;
    VkDeviceSize inputSize;
    uint8_t* mapped = static_cast<uint8_t*>(dgc.mapInputBuffer(inputSize));

    uint32_t pushSize = 128;    // must match initBuffers
    uint32_t maxSequences = dgc.maxSequences();
    uint32_t numDraws = std::min(static_cast<uint32_t>(draws.size()), maxSequences);

    for (uint32_t i = 0; i < numDraws; ++i) {
        const DrawData& draw = draws[i];

        // TODO: serialize push constant data (e.g., material index, model matrix)
        std::memcpy(mapped + i * pushSize, &draw.materialIndex, sizeof(uint32_t));
    }

    dgc.unmapInputBuffer();

    // Use device addresses
    VkGeneratedCommandsInfoEXT genInfo{};
    genInfo.sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT;
    genInfo.pNext = nullptr;
    genInfo.indirectExecutionSet = dgc.executionSet();
    genInfo.indirectCommandsLayout = dgc.layout();

    genInfo.indirectAddress = dgc.inputDeviceAddress();
    genInfo.indirectAddressSize = inputSize;    // the actual size of the input data

    genInfo.preprocessAddress = dgc.preprocessDeviceAddress();
    genInfo.preprocessSize = dgc.preprocessSize();  // must be the actual queried size

    genInfo.maxSequenceCount = numDraws;
    genInfo.shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS;    // or just VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT

    // We don't need preprocess for simple DGC; if used, ensure the buffer is valid.
    // preprocessSize = 0 means no preprocessing.
    // If preprocessSize == 0, the driver may ignore preprocessAddress.
    // But we must still set it correctly.
    if (dgc.preprocessSize() == 0) {
        genInfo.preprocessAddress = 0;
        genInfo.preprocessSize = 0;
    }

    vkCmdExecuteGeneratedCommandsEXT(cmd, VK_FALSE, &genInfo);
}

}   // namespace GoCL