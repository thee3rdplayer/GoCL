#pragma once

#include "core/GoCLContext.h"
#include <vulkan/vulkan.h>

#include <glm/mat4x4.hpp>

namespace GoCL {

/**
 * @brief Buffers used by the meshlet culling compute shader.
 * 
 * All buffers must have the specified usage flags and be device‑local.
 */
struct MeshletCullerBuffers {
    VkBuffer meshletData;   ///< Input: array of meshlet descriptors (bounding sphere + draw parameters).
                            ///< Must have VK_BUFFER_USAGE_STORAGE_BUFFER_BIT.

    VkBuffer indirectDraw;  ///< Output: indirect draw commands for visible meshlets.
                            ///< Must have VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT.

    VkBuffer visibleCount;  ///< Output: atomic counter storing the number of visible meshlets.
                            ///< Must have VK_BUFFER_USAGE_STORAGE_BUFFER_BIT.
};

/**
 * @brief Push constants passed to the culling compute shader.
 * 
 * Must match the layout in the embedded SPIR‑V shader exactly.
 */
struct CullPushConstants {
    glm::mat4 viewProj;     ///< Combined view‑projection matrix for frustum culling.
    uint32_t  meshletCount; ///< Number of meshlets to process (input).
};

/**
 * @brief GPU‑driven frustum culling for devices without mesh shaders.
 *
 * This class is part of the **engine static library only** – it is not
 * exposed through the proxy layer.  It offloads culling to a compute shader,
 * generating an indirect draw buffer that can be consumed directly by
 * vkCmdDrawIndexedIndirect (or similar).
 *
 * The compute shader must be compiled to SPIR‑V and embedded via xxd.
 * See the .cpp file for the required steps.
 */
class MeshletCuller {
public:
    MeshletCuller()  = default;
    ~MeshletCuller() { Destroy(); }

    // Move‑only (no copy).
    MeshletCuller(MeshletCuller&&) noexcept = default;
    MeshletCuller& operator=(MeshletCuller&&) noexcept = default;

    // -----------------------------------------------------------------------
    /**
     * @brief Initialises the Vulkan objects: descriptor set layout, pool, set,
     *        pipeline layout, and (optionally) the compute pipeline.
     *
     * The compute pipeline creation is disabled by default because the
     * shader header is not yet generated.  Uncomment the block in the .cpp
     * file and provide the SPIR‑V binary to enable it.
     *
     * @param ctx GoCLContext containing device and physical device handles.
     * @param maxMeshlets Maximum number of meshlets that can be processed
     *                    (used only for sanity checks; actual buffer sizes are caller‑managed).
     * @return true on success, false on failure.
     */
    bool Init(const GoCLContext& ctx, uint32_t maxMeshlets);

    /**
     * @brief Destroys all Vulkan objects owned by the culler. Idempotent.
     */
    void Destroy();

    // -----------------------------------------------------------------------
    /**
     * @brief Records the compute dispatch that evaluates meshlet visibility.
     *
     * Requires that the compute pipeline has been created (shader embedded)
     * and that UpdateBuffers() has been called with the current buffers.
     *
     * @param cmd Command buffer (must be in recording state).
     * @param buffers Storage buffers for input meshlet data and output.
     * @param viewProj View‑projection matrix used for frustum tests.
     * @param meshletCount Number of meshlets to process (must match buffer size).
     */
    void Cull(VkCommandBuffer cmd, const MeshletCullerBuffers& buffers,
              const glm::mat4& viewProj, uint32_t meshletCount);

    // -----------------------------------------------------------------------
    /**
     * @brief Updates the internal descriptor set to point to the given buffers.
     *
     * Must be called **before** Cull() and **outside** any render pass.
     * Do **not** call while a command buffer that references this set is
     * still in flight (i.e., before the frame's fence is signalled).
     *
     * @param buffers Buffers to bind to the descriptor set.
     */
    void UpdateBuffers(const MeshletCullerBuffers& buffers);

private:
    VkDevice        m_device              = VK_NULL_HANDLE;

    VkPipeline       m_pipeline           = VK_NULL_HANDLE; ///< Compute pipeline (culling shader)
    VkPipelineLayout m_pipelineLayout     = VK_NULL_HANDLE; ///< Pipeline layout (descriptor set + push constants)
    VkDescriptorSetLayout m_descSetLayout = VK_NULL_HANDLE; ///< Layout of the storage buffer set
    VkDescriptorSet  m_descSet            = VK_NULL_HANDLE; ///< Descriptor set pointing to the three buffers
    VkDescriptorPool m_descriptorPool     = VK_NULL_HANDLE; ///< Pool from which m_descSet is allocated
};

}   // namespace GoCL