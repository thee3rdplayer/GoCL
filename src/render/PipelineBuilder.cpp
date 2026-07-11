#include "PipelineBuilder.h"
#include "LegacyShaderEmulator.h"
#include "core/ConfigSingleton.h"
#include <vulkan/vulkan.h>

#include <cstdio>

#ifndef VK_PIPELINE_CREATE_INDIRECT_BINDABLE_BIT_EXT
#define VK_PIPELINE_CREATE_INDIRECT_BINDABLE_BIT_EXT 0x00000040
#endif

namespace GoCL {

// ---------------------------------------------------------------------------
// CreateGraphicsPipeline – builds a Vulkan graphics pipeline with
// cross‑generational adaptations: shader patching, optional tessellation,
// configurable MSAA, and dynamic viewport/scissor.
// ---------------------------------------------------------------------------
VkPipeline PipelineBuilder::CreateGraphicsPipeline(
    const GoCLContext& ctx,
    const ShaderSource& src,
    PipelineDesc&      desc
) noexcept {
    // ----- 1. Patch and validate SPIR‑V against the actual hardware caps -----
    auto opts        = LegacyShaderEmulator::BuildOptions(ctx.caps);
    auto vertPatched = LegacyShaderEmulator::Process(src.vertexSPIRV,   opts);
    auto fragPatched = LegacyShaderEmulator::Process(src.fragmentSPIRV, opts);

    if (!LegacyShaderEmulator::Validate(vertPatched, ctx.caps)) {
        fprintf(stderr, "GoCL: Vertex SPIR-V validation failed.\n");
        return VK_NULL_HANDLE;
    }

    if (!LegacyShaderEmulator::Validate(fragPatched, ctx.caps)) {
        fprintf(stderr, "GoCL: Fragment SPIR-V validation failed.\n");
        return VK_NULL_HANDLE;
    }

    // ----- 2. Apply safe defaults from GoCL.conf (singleton) -----
    const auto& cfg = GetConfig();

    // MSAA: only override if the pipeline uses 1x (the engine default) and the config
    // requests a higher value. Clamp to what the device actually supports.
    if (desc.msaaSamples == VK_SAMPLE_COUNT_1_BIT && cfg.msaaSamples > 1) {
        const auto& limits = ctx.devProps.limits;
        VkSampleCountFlags supported =
            limits.framebufferColorSampleCounts &
            limits.framebufferDepthSampleCounts;
        desc.msaaSamples = ClampMSAASamples(supported, cfg.msaaSamples);
    }

    // Tessellation warning: config may ask for it, but the caller must provide
    // the tessellation state create info (desc.pTessellationState). If missing,
    // the pipeline will be created without tessellation.
    if (cfg.enableTessellation && !desc.pTessellationState) {
        fprintf(stderr, "GoCL: Tessellation enabled in GoCL.conf but no "
                        "VkPipelineTessellationStateCreateInfo provided. "
                        "The pipeline will be created without tessellation.\n");
    }

    // Guards: if patching failed and returned empty vectors, we cannot continue.
    if (vertPatched.empty() || fragPatched.empty()) {
        fprintf(stderr, "GoCL: Empty SPIR‑V module – cannot create pipeline.\n");
        return VK_NULL_HANDLE;
    }

    // ----- 3. Create Vulkan shader modules from the (possibly patched) SPIR‑V -----
    static_assert(sizeof(decltype(vertPatched)::value_type) == 4,
                  "SPIR‑V words must be 32-bit");
    static_assert(sizeof(decltype(fragPatched)::value_type) == 4,
                  "SPIR‑V words must be 32-bit");

    const size_t vertSizeBytes = vertPatched.size() * sizeof(vertPatched[0]);
    const size_t fragSizeBytes = fragPatched.size() * sizeof(fragPatched[0]);

    VkShaderModuleCreateInfo vertCI{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
        vertSizeBytes, vertPatched.data()
    };

    VkShaderModuleCreateInfo fragCI{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
        fragSizeBytes, fragPatched.data()
    };

    VkShaderModule vertMod = VK_NULL_HANDLE;
    VkShaderModule fragMod = VK_NULL_HANDLE;

    VkResult vertRes = vkCreateShaderModule(ctx.device, &vertCI, nullptr, &vertMod);
    if (vertRes != VK_SUCCESS) {
        fprintf(stderr, "GoCL: Vertex shader module creation failed (%d).\n", vertRes);
        return VK_NULL_HANDLE;
    }

    VkResult fragRes = vkCreateShaderModule(ctx.device, &fragCI, nullptr, &fragMod);
    if (fragRes != VK_SUCCESS) {
        fprintf(stderr, "GoCL: Fragment shader module creation failed (%d).\n", fragRes);
        vkDestroyShaderModule(ctx.device, vertMod, nullptr);
        return VK_NULL_HANDLE;
    }

    // ----- 4. Build specialization constants (either from the default struct
    //          or from a user‑provided override) -----
    VkSpecializationInfo specInfo{};
    if (desc.specializationOverride.entries.empty()) {
        static constexpr VkSpecializationMapEntry kSpecMap[] = {
            { 0, offsetof(SpecConstants, enableAlphaTest), sizeof(uint32_t) },
            { 1, offsetof(SpecConstants, enableFog),       sizeof(uint32_t) },
            { 2, offsetof(SpecConstants, maxLights),       sizeof(uint32_t) },
        };
        specInfo.mapEntryCount = 3;
        specInfo.pMapEntries   = kSpecMap;
        specInfo.dataSize      = sizeof(SpecConstants);
        specInfo.pData         = &desc.specConsts;
    } else {
        specInfo.mapEntryCount = static_cast<uint32_t>(desc.specializationOverride.entries.size());
        specInfo.pMapEntries   = desc.specializationOverride.entries.data();
        specInfo.dataSize      = desc.specializationOverride.data.size();
        specInfo.pData         = desc.specializationOverride.data.data();
    }

    // Specialization may be enabled independently for vertex and fragment stages.
    const VkSpecializationInfo* vertSpec = desc.vertexSpecEnabled ? &specInfo : nullptr;
    const VkSpecializationInfo* fragSpec = desc.fragmentSpecEnabled ? &specInfo : nullptr;

    // ----- 5. Shader stages -----
    const VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
          VK_SHADER_STAGE_VERTEX_BIT,   vertMod, "main", vertSpec },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
          VK_SHADER_STAGE_FRAGMENT_BIT, fragMod, "main", fragSpec },
    };

    // ----- 6. Validate vertex attribute bindings (defensive) -----
    for (const auto& attr : desc.vertexInput.attributes) {
        if (attr.binding >= desc.vertexInput.bindingCount()) {
            fprintf(stderr,
                    "GoCL: Vertex attribute references invalid binding %u.\n",
                    attr.binding);
            vkDestroyShaderModule(ctx.device, vertMod, nullptr);
            vkDestroyShaderModule(ctx.device, fragMod, nullptr);
            return VK_NULL_HANDLE;
        }
    }

    // ----- 7. Vertex input state -----
    const VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0,
        desc.vertexInput.bindingCount(),
        desc.vertexInput.bindingCount() > 0 ? &desc.vertexInput.binding : nullptr,
        static_cast<uint32_t>(desc.vertexInput.attributes.size()),
        desc.vertexInput.attributes.empty()
            ? nullptr
            : desc.vertexInput.attributes.data()
    };

    // ----- 8. Input assembly (primitive topology) -----
    // The engine assumes triangle lists; support for patch lists (tessellation)
    // is handled by the caller via desc.pTessellationState.
    const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        nullptr,
        0,
        desc.topology,
        VK_FALSE
    };

    // ----- 9. Dynamic viewport and scissor (essential for resolution scaling) -----
    static constexpr VkDynamicState kDynStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    const VkPipelineDynamicStateCreateInfo dynState{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0,
        2, kDynStates
    };

    const VkPipelineViewportStateCreateInfo viewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0,
        1, nullptr, 1, nullptr
    };

    // ----- 10. Rasterizer (standard fill, backface culling) -----
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode    = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth   = 1.0f;
    rasterizer.depthClampEnable        = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.depthBiasEnable         = VK_FALSE;

    // ----- 11. Multisampling – controlled by desc.msaaSamples (configurable) -----
    const VkPipelineMultisampleStateCreateInfo multisampling{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, nullptr, 0,
        desc.msaaSamples,
        VK_FALSE, 1.0f, nullptr, VK_FALSE, VK_FALSE
    };

    // ----- 12. Depth/stencil – can be disabled via desc.depthTest -----
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = desc.depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp   = desc.depthCompareOp;

    // ----- 13. Colour blending (simple alpha blending or disabled) -----
    const VkPipelineColorBlendAttachmentState blendAttachment{
        desc.blendEnable ? VK_TRUE : VK_FALSE,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
        VK_BLEND_FACTOR_ONE,       VK_BLEND_FACTOR_ZERO,                VK_BLEND_OP_ADD,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
    };

    const VkPipelineColorBlendStateCreateInfo colorBlend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr, 0,
        VK_FALSE, VK_LOGIC_OP_COPY, 1, &blendAttachment, { 0.f, 0.f, 0.f, 0.f }
    };

    // ----- 14. Validate layout and render pass handles -----
    if (desc.layout == VK_NULL_HANDLE) {
        fprintf(stderr, "GoCL: Invalid pipeline layout.\n");
        return VK_NULL_HANDLE;
    }

    if (desc.renderPass == VK_NULL_HANDLE) {
        fprintf(stderr, "GoCL: Invalid render pass.\n");
        return VK_NULL_HANDLE;
    }

    VkPipelineCreateFlags pipelineFlags = 0;
    if (ctx.caps.dgcSupported) {
        pipelineFlags |= VK_PIPELINE_CREATE_INDIRECT_BINDABLE_BIT_EXT;
    }

    // ----- 15. Create the pipeline (using the shared pipeline cache) -----
    const VkGraphicsPipelineCreateInfo createInfo{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, nullptr, pipelineFlags,
        2,        stages,
        &vertexInput,
        &inputAssembly,
        desc.pTessellationState,    // may be nullptr
        &viewportState,
        &rasterizer,
        &multisampling,
        &depthStencil,
        &colorBlend,
        &dynState,
        desc.layout,
        desc.renderPass,
        desc.subpass,
        VK_NULL_HANDLE, -1
    };

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r = vkCreateGraphicsPipelines(
        ctx.device, ctx.pipelineCache, 1, &createInfo, nullptr, &pipeline);

    if (r != VK_SUCCESS) {
        fprintf(stderr, "GoCL: Pipeline creation failed (%d)\n", r);
    }

    // ----- 16. Clean up shader modules (they are no longer needed) -----
    vkDestroyShaderModule(ctx.device, vertMod, nullptr);
    vkDestroyShaderModule(ctx.device, fragMod, nullptr);

    return (r == VK_SUCCESS) ? pipeline : VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// CreateLayout – creates a VkPipelineLayout with an optional descriptor set
// and push constants. Guards against exceeding the device’s maximum push‑
// constant size (an essential cross‑generational safety check).
// ---------------------------------------------------------------------------
VkPipelineLayout PipelineBuilder::CreateLayout(
    VkDevice              device,
    VkDescriptorSetLayout descSetLayout,
    uint32_t              pushConstantSize,
    uint32_t              maxPushConstSize,
    VkShaderStageFlags    pushConstantStages
) noexcept {
    // Enforce hardware limit (if a non‑zero max is given).
    if (maxPushConstSize > 0 && pushConstantSize > maxPushConstSize) {
        fprintf(stderr, "GoCL: Push constant size %u exceeds device limit %u.\n",
                pushConstantSize, maxPushConstSize);
        return VK_NULL_HANDLE;
    }

    // Push constant size must be a multiple of 4 bytes (Vulkan requirement).
    if ((pushConstantSize & 3u) != 0) {
        fprintf(stderr, "GoCL: Push constant size must be 4-byte aligned.\n");
        return VK_NULL_HANDLE;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = pushConstantStages;
    pushRange.offset     = 0;
    pushRange.size       = pushConstantSize;

    VkPipelineLayoutCreateInfo ci{};
    ci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount         = (descSetLayout != VK_NULL_HANDLE) ? 1 : 0;
    ci.pSetLayouts            = (descSetLayout != VK_NULL_HANDLE) ? &descSetLayout : nullptr;
    ci.pushConstantRangeCount = (pushConstantSize > 0) ? 1 : 0;
    ci.pPushConstantRanges    = (pushConstantSize > 0) ? &pushRange : nullptr;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkResult r = vkCreatePipelineLayout(device, &ci, nullptr, &layout);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "GoCL: vkCreatePipelineLayout failed (%d)\n", r);
        return VK_NULL_HANDLE;
    }

    return layout;
}

}   // namespace GoCL