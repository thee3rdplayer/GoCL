#include "PipelineBuilder.h"
#include "LegacyShaderEmulator.h"

#include <vulkan/vulkan.h>


namespace GoCL {

VkPipeline PipelineBuilder::CreateGraphicsPipeline(
    VkDevice                  device,
    const DeviceCapabilities& caps,
    const ShaderSource&       src,
    const PipelineDesc&       desc
) noexcept {

    auto opts        = LegacyShaderEmulator::BuildOptions(caps);
    auto vertPatched = LegacyShaderEmulator::Process(src.vertexSPIRV,   opts);
    auto fragPatched = LegacyShaderEmulator::Process(src.fragmentSPIRV, opts);

    const VkShaderModuleCreateInfo vertCI {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
        vertPatched.size() * 4, vertPatched.data()
    };
    const VkShaderModuleCreateInfo fragCI {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
        fragPatched.size() * 4, fragPatched.data()
    };

    VkShaderModule vertMod = VK_NULL_HANDLE;
    VkShaderModule fragMod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &vertCI, nullptr, &vertMod) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &fragCI, nullptr, &fragMod) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vertMod, nullptr);
        return VK_NULL_HANDLE;
    }

    // Specialization constant map — all three entries, contiguous in SpecConstants
    static constexpr VkSpecializationMapEntry kSpecMap[] = {
        { 0, offsetof(SpecConstants, enableAlphaTest), sizeof(uint32_t) },
        { 1, offsetof(SpecConstants, enableFog),       sizeof(uint32_t) },
        { 2, offsetof(SpecConstants, maxLights),       sizeof(uint32_t) },
    };
    const VkSpecializationInfo specInfo {
        3, kSpecMap,
        sizeof(SpecConstants), &desc.specConsts
    };

    // Both stages share the same spec constants block — driver resolves per-stage
    const VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
          VK_SHADER_STAGE_VERTEX_BIT,   vertMod, "main", &specInfo },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
          VK_SHADER_STAGE_FRAGMENT_BIT, fragMod, "main", &specInfo },
    };

    const VkPipelineVertexInputStateCreateInfo vertexInput {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0,
        1,          &desc.vertexInput.binding,
        static_cast<uint32_t>(desc.vertexInput.attributes.size()),
                    desc.vertexInput.attributes.data()
    };

    const VkPipelineInputAssemblyStateCreateInfo inputAssembly {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE
    };

    static constexpr VkDynamicState kDynStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    const VkPipelineDynamicStateCreateInfo dynState {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0,
        2, kDynStates
    };
    const VkPipelineViewportStateCreateInfo viewportState {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0,
        1, nullptr, 1, nullptr
    };

    VkPipelineRasterizationStateCreateInfo rasterizer {};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode    = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth   = 1.0f;

    const VkPipelineMultisampleStateCreateInfo multisampling {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, nullptr, 0,
        VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 1.0f, nullptr, VK_FALSE, VK_FALSE
    };

    VkPipelineDepthStencilStateCreateInfo depthStencil {};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = desc.depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = desc.depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;

    const VkPipelineColorBlendAttachmentState blendAttachment {
        desc.blendEnable ? VK_TRUE : VK_FALSE,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
        VK_BLEND_FACTOR_ONE,       VK_BLEND_FACTOR_ZERO,                VK_BLEND_OP_ADD,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
    };
    const VkPipelineColorBlendStateCreateInfo colorBlend {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr, 0,
        VK_FALSE, VK_LOGIC_OP_COPY, 1, &blendAttachment, { 0.f, 0.f, 0.f, 0.f }
    };

    const VkGraphicsPipelineCreateInfo createInfo {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, nullptr, 0,
        2,        stages,
        &vertexInput,
        &inputAssembly,
        nullptr,
        &viewportState,
        &rasterizer,
        &multisampling,
        &depthStencil,
        &colorBlend,
        &dynState,
        desc.layout,
        desc.renderPass,
        0,
        VK_NULL_HANDLE, -1
    };

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r = vkCreateGraphicsPipelines(
        device, desc.cache, 1, &createInfo, nullptr, &pipeline);

    vkDestroyShaderModule(device, vertMod, nullptr);
    vkDestroyShaderModule(device, fragMod, nullptr);

    return (r == VK_SUCCESS) ? pipeline : VK_NULL_HANDLE;
}

} // namespace GoCL