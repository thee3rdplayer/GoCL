#include "DynamicResolutionHelper.h"

namespace GoCL {

// ---------------------------------------------------------------------------
// Helper: Creates a device‑local image, allocates memory, and creates a view.
// Used to create the downscaled render target that DRS renders into.
// Returns false on any failure; cleans up partially allocated resources.
// ---------------------------------------------------------------------------
static bool CreateRenderTarget(VkDevice device, VkPhysicalDevice physDev,
                               VkFormat format, VkExtent2D extent,
                               VkImageUsageFlags usage,
                               VkImage& image, VkDeviceMemory& memory,
                               VkImageView& view) {
    VkImageCreateInfo imgCI{};
    imgCI.sType        = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType    = VK_IMAGE_TYPE_2D;
    imgCI.format       = format;
    imgCI.extent       = { extent.width, extent.height, 1 };
    imgCI.mipLevels    = 1;
    imgCI.arrayLayers  = 1;
    imgCI.samples      = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling       = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage        = usage;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device, &imgCI, nullptr, &image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(device, image, &memReq);

    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);

    // Find a memory type that is DEVICE_LOCAL (preferred for performance).
    uint32_t memIdx = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memIdx = i;
            break;
        }
    }

    if (memIdx == UINT32_MAX) {
        vkDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo allocCI{};
    allocCI.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocCI.allocationSize  = memReq.size;
    allocCI.memoryTypeIndex = memIdx;

    if (vkAllocateMemory(device, &allocCI, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }

    VkResult r = vkBindImageMemory(device, image, memory, 0);
    if (r != VK_SUCCESS) {
        vkFreeMemory(device, memory, nullptr);
        vkDestroyImage(device, image, nullptr);
        memory = VK_NULL_HANDLE;
        image  = VK_NULL_HANDLE;
        return false;
    }

    VkImageViewCreateInfo viewCI{};
    viewCI.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image      = image;
    viewCI.viewType   = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format     = format;
    viewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCI.subresourceRange.levelCount = 1;
    viewCI.subresourceRange.layerCount = 1;

    r = vkCreateImageView(device, &viewCI, nullptr, &view);
    if (r != VK_SUCCESS) {
        vkFreeMemory(device, memory, nullptr);
        vkDestroyImage(device, image, nullptr);
        view   = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        image  = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Initialises the DRS helper: creates the downscaled target, descriptor set,
// pipeline layout, and render pass. The actual compute/upscale pipeline is
// commented out because shader binaries are not yet embedded.
// ---------------------------------------------------------------------------
bool DynamicResolutionHelper::Init(const GoCLContext& ctx,
                                   VkExtent2D swapchainExtent,
                                   float scale,
                                   QualityPreset preset,
                                   VkFormat swapchainFormat) {
    m_device        = ctx.device;
    m_physDev       = ctx.physDev;
    m_qualityPreset = preset;
    m_swapchainFormat = swapchainFormat;

    // Clamp scale to reasonable range [0.25, 1.0].
    if (scale < 0.25f) scale = 0.25f;
    if (scale > 1.0f)  scale = 1.0f;
    m_downscaledExtent = {
        std::max(1u, static_cast<uint32_t>(swapchainExtent.width  * scale)),
        std::max(1u, static_cast<uint32_t>(swapchainExtent.height * scale))
    };

    if (!CreateRenderTarget(m_device, m_physDev, m_swapchainFormat, m_downscaledExtent,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            m_downscaledImage, m_downscaledMemory,
                            m_downscaledView)) {
        Destroy();
        return false;
    }

    // Sampler: linear filtering, clamp to edge.
    VkSamplerCreateInfo samplerCI{};
    samplerCI.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerCI.magFilter    = VK_FILTER_LINEAR;
    samplerCI.minFilter    = VK_FILTER_LINEAR;
    samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    VkResult r = vkCreateSampler(m_device, &samplerCI, nullptr, &m_sampler);
    if (r != VK_SUCCESS) {
        Destroy();
        return false;
    }

    // Descriptor set layout: one combined image sampler.
    VkDescriptorSetLayoutBinding bind{};
    bind.binding         = 0;
    bind.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bind.descriptorCount = 1;
    bind.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslCI{};
    dslCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslCI.bindingCount = 1;
    dslCI.pBindings    = &bind;

    r = vkCreateDescriptorSetLayout(m_device, &dslCI, nullptr, &m_descSetLayout);
    if (r != VK_SUCCESS) {
        Destroy();
        return false;
    }

    // Descriptor pool – single combined image sampler.
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes    = &poolSize;

    r = vkCreateDescriptorPool(m_device, &poolCI, nullptr, &m_descPool);
    if (r != VK_SUCCESS) {
        Destroy();
        return false;
    }

    // Allocate descriptor set.
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = m_descPool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts        = &m_descSetLayout;

    r = vkAllocateDescriptorSets(m_device, &alloc, &m_descSet);
    if (r != VK_SUCCESS) {
        Destroy();
        return false;
    }

    // Update the descriptor set with the downscaled target and sampler.
    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler     = m_sampler;
    imageInfo.imageView   = m_downscaledView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = m_descSet;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &imageInfo;

    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

    // Pipeline layout (shared by all upscaling pipelines).
    VkPipelineLayoutCreateInfo plCI{};
    plCI.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts    = &m_descSetLayout;

    r = vkCreatePipelineLayout(m_device, &plCI, nullptr, &m_pipelineLayout);
    if (r != VK_SUCCESS) {
        Destroy();
        return false;
    }

    // Render pass – matches the swapchain format.
    {
        VkAttachmentDescription colorAtt{};
        colorAtt.format         = m_swapchainFormat;
        colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
        colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtt.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &colorRef;

        VkRenderPassCreateInfo rpCI{};
        rpCI.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpCI.attachmentCount = 1;
        rpCI.pAttachments    = &colorAtt;
        rpCI.subpassCount    = 1;
        rpCI.pSubpasses      = &subpass;

        r = vkCreateRenderPass(m_device, &rpCI, nullptr, &m_renderPass);
        if (r != VK_SUCCESS) {
            Destroy();
            return false;
        }
    }

    // ── Upscale pipeline (commented out – uncomment when shader binaries are embedded) ──
    // When you are ready to ship the shaders:
    //   1.  glslangValidator -V dynamic_resolution_upscale.vert -o dynamic_resolution_upscale.vert.spv
    //       glslangValidator -V dynamic_resolution_upscale.frag -o dynamic_resolution_upscale.frag.spv
    //   2.  xxd -i dynamic_resolution_upscale.vert.spv > dynamic_resolution_upscale_vert.h
    //       xxd -i dynamic_resolution_upscale.frag.spv > dynamic_resolution_upscale_frag_bilinear.h
    //   3.  Uncomment the entire block below and the #includes at the top of this file.
    //
    // #include "dynamic_resolution_upscale_vert.h"
    // #include "dynamic_resolution_upscale_frag_bilinear.h"
    //
    // VkShaderModuleCreateInfo smCI{};
    // smCI.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    //
    // // Vertex shader
    // smCI.codeSize = dynamic_resolution_upscale_vert_spv_len;
    // smCI.pCode    = dynamic_resolution_upscale_vert_spv;
    // VkShaderModule vertModule;
    // VkResult vertRes = vkCreateShaderModule(m_device, &smCI, nullptr, &vertModule);
    // if (vertRes != VK_SUCCESS) return false;
    //
    // // Fragment shader – select based on quality preset
    // const uint32_t* fragCode = nullptr;
    // size_t          fragLen  = 0;
    // switch (m_qualityPreset) {
    //     case QualityPreset::Low:
    //     case QualityPreset::Medium:
    //     case QualityPreset::High:
    //     case QualityPreset::Ultra:
    //     default:
    //         // All presets currently use the bilinear upscaler.
    //         // Replace with sharper kernels when the shaders are embedded.
    //         fragCode = dynamic_resolution_upscale_frag_bilinear_spv;
    //         fragLen  = dynamic_resolution_upscale_frag_bilinear_spv_len;
    //         break;
    // }
    // smCI.codeSize = fragLen;
    // smCI.pCode    = fragCode;
    // VkShaderModule fragModule;
    // VkResult fragRes = vkCreateShaderModule(m_device, &smCI, nullptr, &fragModule);
    // if (fragRes != VK_SUCCESS) {
    //     vkDestroyShaderModule(m_device, vertModule, nullptr);
    //     return false;
    // }
    //
    // VkPipelineShaderStageCreateInfo stages[2] = {};
    // stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    // stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    // stages[0].module = vertModule;
    // stages[0].pName  = "main";
    // stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    // stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    // stages[1].module = fragModule;
    // stages[1].pName  = "main";
    //
    // VkPipelineVertexInputStateCreateInfo vertexInput{};
    // vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    //
    // VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    // inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    // inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    //
    // VkPipelineViewportStateCreateInfo viewportState{};
    // viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    // viewportState.viewportCount = 1;
    // viewportState.scissorCount  = 1;
    //
    // VkPipelineRasterizationStateCreateInfo rasterizer{};
    // rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    // rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    // rasterizer.cullMode    = VK_CULL_MODE_NONE;
    // rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    // rasterizer.lineWidth   = 1.0f;
    //
    // VkPipelineMultisampleStateCreateInfo multisampling{};
    // multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    // multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    //
    // VkPipelineColorBlendAttachmentState blendAtt{};
    // blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
    //                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    // blendAtt.blendEnable    = VK_FALSE;
    //
    // VkPipelineColorBlendStateCreateInfo colorBlend{};
    // colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    // colorBlend.attachmentCount = 1;
    // colorBlend.pAttachments    = &blendAtt;
    //
    // VkPipelineDynamicStateCreateInfo dynamicState{};
    // dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    // VkDynamicState dynStates[]     = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    // dynamicState.dynamicStateCount = 2;
    // dynamicState.pDynamicStates    = dynStates;
    //
    // VkGraphicsPipelineCreateInfo gpCI{};
    // gpCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    // gpCI.stageCount          = 2;
    // gpCI.pStages             = stages;
    // gpCI.pVertexInputState   = &vertexInput;
    // gpCI.pInputAssemblyState = &inputAssembly;
    // gpCI.pViewportState      = &viewportState;
    // gpCI.pRasterizationState = &rasterizer;
    // gpCI.pMultisampleState   = &multisampling;
    // gpCI.pColorBlendState    = &colorBlend;
    // gpCI.pDynamicState       = &dynamicState;
    // gpCI.layout              = m_pipelineLayout;
    // gpCI.renderPass          = m_renderPass;
    // gpCI.subpass             = 0;
    //
    // VkResult pipeRes = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &gpCI, nullptr, &m_upscalePipeline);
    // if (pipeRes != VK_SUCCESS) {
    //     vkDestroyShaderModule(m_device, vertModule, nullptr);
    //     vkDestroyShaderModule(m_device, fragModule, nullptr);
    //     return false;
    // }

    return true;
}

// ---------------------------------------------------------------------------
// Destroys all Vulkan resources created by the helper.
// Idempotent and safe to call multiple times.
// ---------------------------------------------------------------------------
void DynamicResolutionHelper::Destroy() noexcept {
    if (m_upscalePipeline) {
        vkDestroyPipeline(m_device, m_upscalePipeline, nullptr);
        m_upscalePipeline = VK_NULL_HANDLE;
    }

    for (auto fb : m_pendingFramebuffers) {
        vkDestroyFramebuffer(m_device, fb, nullptr);
    }
    m_pendingFramebuffers.clear();

    for (auto view : m_pendingImageViews) {
        vkDestroyImageView(m_device, view, nullptr);
    }
    m_pendingImageViews.clear();

    if (m_pipelineLayout) {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }

    if (m_descSetLayout) {
        vkDestroyDescriptorSetLayout(m_device, m_descSetLayout, nullptr);
        m_descSetLayout = VK_NULL_HANDLE;
    }

    if (m_descPool) {
        vkDestroyDescriptorPool(m_device, m_descPool, nullptr);
        m_descPool = VK_NULL_HANDLE;
    }

    if (m_sampler) {
        vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }

    if (m_renderPass) {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }

    if (m_downscaledView) {
        vkDestroyImageView(m_device, m_downscaledView, nullptr);
        m_downscaledView = VK_NULL_HANDLE;
    }

    if (m_downscaledImage) {
        vkDestroyImage(m_device, m_downscaledImage, nullptr);
        m_downscaledImage = VK_NULL_HANDLE;
    }

    if (m_downscaledMemory) {
        vkFreeMemory(m_device, m_downscaledMemory, nullptr);
        m_downscaledMemory = VK_NULL_HANDLE;
    }

    m_device = VK_NULL_HANDLE;
    m_physDev = VK_NULL_HANDLE;
    m_downscaledExtent = {};
    m_descSet = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Records commands to upscale the downscaled target into the swapchain image.
// Must be called within a render pass that has the swapchain image as colour attachment.
// This function creates a temporary framebuffer and image view for the swapchain
// image, destroys them on the next call (deferred cleanup).
// ---------------------------------------------------------------------------
void DynamicResolutionHelper::Upscale(VkCommandBuffer cmd, VkImage swapchainImage,
                                      VkExtent2D swapchainExtent) {
    if (!m_upscalePipeline) return;

    // Destroy resources from the previous Upscale() call.
    for (auto fb : m_pendingFramebuffers) {
        vkDestroyFramebuffer(m_device, fb, nullptr);
    }
    m_pendingFramebuffers.clear();

    for (auto view : m_pendingImageViews) {
        vkDestroyImageView(m_device, view, nullptr);
    }
    m_pendingImageViews.clear();

    // Transition the swapchain image to COLOR_ATTACHMENT_OPTIMAL layout.
    VkImageMemoryBarrier preBarrier{};
    preBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preBarrier.srcAccessMask       = 0;
    preBarrier.dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    preBarrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    preBarrier.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    preBarrier.image               = swapchainImage;
    preBarrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0,
                         0, nullptr,
                         0, nullptr,
                         1, &preBarrier);

    // Create a temporary image view for the swapchain image.
    VkImageViewCreateInfo viewCI{};
    viewCI.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image      = swapchainImage;
    viewCI.viewType   = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format     = m_swapchainFormat;
    viewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCI.subresourceRange.levelCount = 1;
    viewCI.subresourceRange.layerCount = 1;

    VkImageView swapchainView;
    vkCreateImageView(m_device, &viewCI, nullptr, &swapchainView);

    // Create a framebuffer using that view.
    VkFramebufferCreateInfo fbCI{};
    fbCI.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbCI.renderPass      = m_renderPass;
    fbCI.attachmentCount = 1;
    fbCI.pAttachments    = &swapchainView;
    fbCI.width           = swapchainExtent.width;
    fbCI.height          = swapchainExtent.height;
    fbCI.layers          = 1;

    VkFramebuffer framebuffer;
    VkResult r = vkCreateFramebuffer(m_device, &fbCI, nullptr, &framebuffer);
    if (r != VK_SUCCESS) {
        vkDestroyImageView(m_device, swapchainView, nullptr);
        return;
    }

    // Record the upscale pass.
    VkRenderPassBeginInfo rpBI{};
    rpBI.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBI.renderPass        = m_renderPass;
    rpBI.framebuffer       = framebuffer;
    rpBI.renderArea.extent = swapchainExtent;

    vkCmdBeginRenderPass(cmd, &rpBI, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_upscalePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 0, 1, &m_descSet, 0, nullptr);

    // Set viewport and scissor to cover the whole swapchain.
    VkViewport vp{ 0.f, 0.f, (float)swapchainExtent.width, (float)swapchainExtent.height, 0.f, 1.f };
    VkRect2D sc{ {0, 0}, swapchainExtent };

    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    // Draw a full‑screen triangle (3 vertices).
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    // Keep the view and framebuffer alive until the next Upscale() call.
    m_pendingFramebuffers.push_back(framebuffer);
    m_pendingImageViews.push_back(swapchainView);
}

}   // namespace GoCL