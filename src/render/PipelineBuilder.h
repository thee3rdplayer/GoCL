#pragma once

#include "../core/CapabilityOracle.h"

#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>


namespace GoCL {

struct ShaderSource {
    std::vector<uint32_t> vertexSPIRV;
    std::vector<uint32_t> fragmentSPIRV;
};

struct VertexInputDesc {
    VkVertexInputBindingDescription binding {
        0, 5 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX
    };
    std::vector<VkVertexInputAttributeDescription> attributes {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0                },  // pos
        { 1, 0, VK_FORMAT_R32G32_SFLOAT,    3 * sizeof(float) }, // uv
    };
};

// Specialization constants — GPU resolves these at pipeline compile time.
// Map directly to constant_id in SPIR-V. Keep POD, stack-allocated.
struct SpecConstants {
    uint32_t enableAlphaTest  = 0;  // constant_id = 0
    uint32_t enableFog        = 0;  // constant_id = 1
    uint32_t maxLights        = 4;  // constant_id = 2
};

struct PipelineDesc {
    VkRenderPass     renderPass  = VK_NULL_HANDLE;
    VkPipelineLayout layout      = VK_NULL_HANDLE;
    VkPipelineCache  cache       = VK_NULL_HANDLE;
    VkExtent2D       viewport    = { 1280, 720 };
    bool             depthTest   = true;
    bool             blendEnable = false;
    VertexInputDesc  vertexInput = {};
    SpecConstants    specConsts  = {};  // baked into pipeline by GPU driver
};

class PipelineBuilder {
public:
    [[nodiscard]] static VkPipeline CreateGraphicsPipeline(
        VkDevice                  device,
        const DeviceCapabilities& caps,
        const ShaderSource&       src,
        const PipelineDesc&       desc
    ) noexcept;
};

} // namespace GoCL