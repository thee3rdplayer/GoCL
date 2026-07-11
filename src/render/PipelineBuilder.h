#pragma once

#include "core/GoCLContext.h"
#include <vulkan/vulkan.h>

#include <vector>
#include <cstdint>

namespace GoCL {

// ---------------------------------------------------------------------------
// Helper: returns the highest supported MSAA sample count that is ≤ the
// requested count.  If the requested count is not supported, falls back to
// the next lower supported count, or 1x if none are supported.
// ---------------------------------------------------------------------------
[[nodiscard]]
inline VkSampleCountFlagBits ClampMSAASamples(VkSampleCountFlags supported,
                                               uint32_t requested) {
    // Check from highest to lowest supported sample count.
    if (requested >= 64 && (supported & VK_SAMPLE_COUNT_64_BIT)) return VK_SAMPLE_COUNT_64_BIT;
    if (requested >= 32 && (supported & VK_SAMPLE_COUNT_32_BIT)) return VK_SAMPLE_COUNT_32_BIT;
    if (requested >= 16 && (supported & VK_SAMPLE_COUNT_16_BIT)) return VK_SAMPLE_COUNT_16_BIT;
    if (requested >=  8 && (supported & VK_SAMPLE_COUNT_8_BIT))  return VK_SAMPLE_COUNT_8_BIT;
    if (requested >=  4 && (supported & VK_SAMPLE_COUNT_4_BIT))  return VK_SAMPLE_COUNT_4_BIT;
    if (requested >=  2 && (supported & VK_SAMPLE_COUNT_2_BIT))  return VK_SAMPLE_COUNT_2_BIT;
    return VK_SAMPLE_COUNT_1_BIT;
}

// ---------------------------------------------------------------------------
// ShaderSource holds the SPIR‑V binaries for the vertex and fragment stages.
// The SPIR‑V vectors must be populated with valid, well‑formed code.
// ---------------------------------------------------------------------------
struct ShaderSource {
    std::vector<uint32_t> vertexSPIRV;
    std::vector<uint32_t> fragmentSPIRV;
};

// ---------------------------------------------------------------------------
// VertexInputDesc describes the vertex layout: a single binding (position + UV)
// and a list of attribute descriptions (location, binding, format, offset).
// The binding stride is calculated from 5 floats (pos 3 + uv 2) = 5*4 = 20 bytes.
// bindingCount() returns 1 if the binding is active (stride > 0), else 0.
// ---------------------------------------------------------------------------
struct VertexInputDesc {
    VkVertexInputBindingDescription binding{
        0, 5 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX   // stride = 20 bytes
    };
    std::vector<VkVertexInputAttributeDescription> attributes{};

    /**
     * @brief Returns 1 if the binding is valid (stride > 0), otherwise 0.
     */
    uint32_t bindingCount() const noexcept {
        return (binding.stride > 0) ? 1 : 0;
    }
};

// ---------------------------------------------------------------------------
// Default specialization constants used by the engine's shaders.
// The constants match the IDs 0,1,2 used in the shader source.
// ---------------------------------------------------------------------------
struct SpecConstants {
    uint32_t enableAlphaTest = 0;   ///< ID 0 – toggles alpha test (0 = off, 1 = on)
    uint32_t enableFog       = 0;   ///< ID 1 – toggles fog (0 = off, 1 = on)
    uint32_t maxLights       = 4;   ///< ID 2 – maximum number of lights in the scene
};

// ---------------------------------------------------------------------------
// Override the default specialization map and data. If entries is empty,
// the default map (IDs 0,1,2) and the SpecConstants struct are used.
// This allows pipelines to use arbitrary specialization constants without
// changing the engine's assumption that the shader uses the default IDs.
// ---------------------------------------------------------------------------
struct SpecializationOverride {
    std::vector<VkSpecializationMapEntry> entries;   ///< If empty → use default map
    std::vector<uint8_t>                  data;      ///< Raw bytes for the specialization data
};

// ---------------------------------------------------------------------------
// PipelineDesc holds all parameters needed to create a graphics pipeline.
// Most fields have safe defaults; MSAA can be overridden via GoCL.conf.
// The layout and render pass must be provided by the caller.
// ---------------------------------------------------------------------------
struct PipelineDesc {
    VkRenderPass     renderPass  = VK_NULL_HANDLE;  ///< Required
    VkPipelineLayout layout      = VK_NULL_HANDLE;  ///< Required

    // ─── Depth/stencil configuration ──────────────────────────────────────────
    bool             depthTest   = true;    ///< Enable depth testing
    bool             depthWrite  = true;    ///< Allow depth writes
    VkCompareOp      depthCompareOp = VK_COMPARE_OP_LESS;   ///< Depth comparison function

    // ─── Blending ─────────────────────────────────────────────────────────────
    bool             blendEnable = false;   ///< Simple alpha blending or off

    // ─── Vertex layout ────────────────────────────────────────────────────────
    VertexInputDesc  vertexInput = {};      ///< Vertex layout
    SpecConstants    specConsts  = {};      ///< Default specialization constants

    // ─── Multisampling and tessellation ──────────────────────────────────────
    VkSampleCountFlagBits               msaaSamples       = VK_SAMPLE_COUNT_1_BIT;
    const VkPipelineTessellationStateCreateInfo* pTessellationState = nullptr;  ///< May be nullptr
    uint32_t                            subpass           = 0;  ///< Render pass subpass index

    // ─── Primitive topology ───────────────────────────────────────────────────
    VkPrimitiveTopology                 topology          = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // ─── Specialization ───────────────────────────────────────────────────────
    SpecializationOverride specializationOverride;

    bool vertexSpecEnabled   = true;   ///< Enable specialization constants for vertex stage
    bool fragmentSpecEnabled = true;   ///< Enable specialization constants for fragment stage
};

// ---------------------------------------------------------------------------
// PipelineBuilder – static utility class for creating Vulkan graphics pipelines
// and pipeline layouts. All functions are stateless and thread‑safe.
// ---------------------------------------------------------------------------
class PipelineBuilder {
public:
    PipelineBuilder() = delete; // pure static – no instances

    // -----------------------------------------------------------------------
    /**
     * @brief Creates a full graphics pipeline using the provided shader sources and description.
     * 
     * The shaders are first patched (if needed) via LegacyShaderEmulator,
     * then compiled into Vulkan shader modules. The pipeline is created using
     * the device's pipeline cache.
     * 
     * @param ctx GoCLContext containing device and pipeline cache.
     * @param src ShaderSource with vertex and fragment SPIR‑V.
     * @param desc PipelineDesc with all creation parameters.
     * @return VkPipeline handle, or VK_NULL_HANDLE on failure.
     */
    [[nodiscard]] static VkPipeline CreateGraphicsPipeline(
        const GoCLContext& ctx,
        const ShaderSource& src,
        PipelineDesc&      desc
    ) noexcept;

    // -----------------------------------------------------------------------
    /**
     * @brief Creates a pipeline layout with an optional descriptor set layout and push constants.
     * 
     * The push constant size is validated against the device limit (maxPushConstSize) if provided.
     * 
     * @param device Vulkan device.
     * @param descSetLayout Descriptor set layout (may be VK_NULL_HANDLE).
     * @param pushConstantSize Size of push constants in bytes (must be 4-byte aligned).
     * @param maxPushConstSize Maximum push constant size allowed by the device (0 = no limit).
     * @param pushConstantStages Shader stages that use push constants.
     * @return VkPipelineLayout handle, or VK_NULL_HANDLE on failure.
     */
    [[nodiscard]] static VkPipelineLayout CreateLayout(
        VkDevice                         device,
        VkDescriptorSetLayout            descSetLayout,
        uint32_t                         pushConstantSize      = 0,
        uint32_t                         maxPushConstSize      = 0,
        VkShaderStageFlags               pushConstantStages =
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
    ) noexcept;
};

}   // namespace GoCL