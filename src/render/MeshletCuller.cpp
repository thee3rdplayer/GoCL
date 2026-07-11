// =============================================================================
// GPU‑driven frustum culling for hardware without mesh shaders.
//
// This utility replaces the modern mesh‑shading pipeline with a compute‑shader
// approach that targets Vulkan devices supporting compute pipelines.
// Applications feed a buffer of meshlet descriptors
// (bounding spheres + draw parameters) and receive an
// indirect draw buffer containing only the visible instances.
//
// The compute shader must be compiled to SPIR‑V with glslangValidator and
// embedded via xxd.  See the placeholder include below – replace it with the
// generated header to enable the compute pipeline.
// =============================================================================

#include "MeshletCuller.h"

#include <cstdio>

// ---------------------------------------------------------------------------
// Placeholder for the embedded meshlet culling SPIR‑V.
//
// When you are ready to ship the shader:
//   1.  glslangValidator -V meshlet_cull.comp -o meshlet_cull.spv
//   2.  xxd -i meshlet_cull.spv > meshlet_cull.h
//   3.  Uncomment the #include and the pipeline‑creation block below.
// ---------------------------------------------------------------------------

namespace GoCL {

// ---------------------------------------------------------------------------
// Initialises the compute pipeline, descriptor set layout, pool, and set.
// The actual compute pipeline creation is disabled until the shader header
// is provided. Without a valid pipeline, Cull() will be a no‑op.
// ---------------------------------------------------------------------------
bool MeshletCuller::Init(const GoCLContext& ctx, uint32_t maxMeshlets) {
    m_device = ctx.device;

    if (maxMeshlets == 0) {
        fprintf(stderr, "GoCL: MeshletCuller requires maxMeshlets > 0\n");
        return false;
    }

    // ── Descriptor set layout ─────────────────────────────────────────────
    // Three storage buffers: meshlet data (input), indirect draw buffer (output),
    // and visible count (output).
    VkDescriptorSetLayoutBinding bindings[3] = {};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dslCI{};
    dslCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslCI.bindingCount = 3;
    dslCI.pBindings    = bindings;

    VkResult r = vkCreateDescriptorSetLayout(m_device, &dslCI, nullptr, &m_descSetLayout);
    if (r != VK_SUCCESS) {
        return false;
    }

    // ── Descriptor pool (kept alive) ──────────────────────────────────────
    // One descriptor set with three storage buffer bindings.
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 3;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes    = &poolSize;

    r = vkCreateDescriptorPool(m_device, &poolCI, nullptr, &m_descriptorPool);
    if (r != VK_SUCCESS) {
        return false;
    }

    // ── Descriptor set (allocated once, updated later via UpdateBuffers) ──
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = m_descriptorPool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts        = &m_descSetLayout;

    r = vkAllocateDescriptorSets(m_device, &alloc, &m_descSet);
    if (r != VK_SUCCESS) {
        return false;
    }

    // ── Push constant range ───────────────────────────────────────────────
    // Contains the view‑projection matrix and meshlet count.
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(CullPushConstants);

    VkPipelineLayoutCreateInfo plCI{};
    plCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount         = 1;
    plCI.pSetLayouts            = &m_descSetLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges    = &pushRange;

    r = vkCreatePipelineLayout(m_device, &plCI, nullptr, &m_pipelineLayout);
    if (r != VK_SUCCESS) {
        return false;
    }

    // ── Compute pipeline (disabled until shader header is provided) ─────
    // When meshlet_cull.h is available, uncomment the block below to
    // create the compute pipeline.  Without it, Cull() will be a no‑op.
    //
    // #include "meshlet_cull.h"
    //
    // VkShaderModuleCreateInfo smCI{};
    // smCI.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    // smCI.codeSize = meshlet_cull_spv_len;
    // smCI.pCode    = meshlet_cull_spv;
    // VkShaderModule module;
    // vkCreateShaderModule(m_device, &smCI, nullptr, &module);
    //
    // VkComputePipelineCreateInfo cpCI{};
    // cpCI.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    // cpCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    // cpCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    // cpCI.stage.module = module;
    // cpCI.stage.pName  = "main";
    // cpCI.layout       = m_pipelineLayout;
    // r = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpCI, nullptr, &m_pipeline);
    // if (r != VK_SUCCESS) return false;
    //
    // vkDestroyShaderModule(m_device, module, nullptr);

    return true;
}

// ---------------------------------------------------------------------------
// Destroys all Vulkan objects owned by the culler. Idempotent.
// The descriptor set is automatically destroyed when the pool is destroyed.
// ---------------------------------------------------------------------------
void MeshletCuller::Destroy() {
    if (m_pipeline) {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }

    if (m_pipelineLayout) {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }

    if (m_descSetLayout) {
        vkDestroyDescriptorSetLayout(m_device, m_descSetLayout, nullptr);
        m_descSetLayout = VK_NULL_HANDLE;
    }

    if (m_descriptorPool) {
        // Destroying the pool invalidates all descriptor sets allocated from it.
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
        m_descSet        = VK_NULL_HANDLE;
    }

    m_device = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Records the compute dispatch that evaluates meshlet visibility.
// The compute shader reads meshlet bounding spheres from buffers.meshletData,
// writes the visible meshlets as an indirect draw buffer, and stores the
// count in buffers.visibleCount.
//
// If the pipeline or descriptor set is not initialised (e.g., shader not yet
// embedded), this function silently returns (no‑op).
// ---------------------------------------------------------------------------
void MeshletCuller::Cull(VkCommandBuffer cmd, const MeshletCullerBuffers& buffers,
                         const glm::mat4& viewProj, uint32_t meshletCount) {
    if (!m_pipeline || !m_descSet) {
        return;    // pipeline or descriptor set not ready – no‑op
    }

    if (!cmd || !buffers.meshletData || !buffers.indirectDraw || !buffers.visibleCount) {
        return;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &m_descSet, 0, nullptr);

    CullPushConstants push{ viewProj, meshletCount };
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);

    // Dispatch one workgroup per 64 meshlets (each thread processes one meshlet).
    uint32_t groups = meshletCount / 64 + ((meshletCount % 64) ? 1u : 0u);
    vkCmdDispatch(cmd, groups, 1, 1);
}

// ---------------------------------------------------------------------------
// Updates the descriptor set to point to the current buffer resources.
// Must be called before Cull() whenever the buffers change, and outside any
// render pass. Do not call while a command buffer that references this set
// is still in flight (i.e., before the fence for that frame is signalled).
// ---------------------------------------------------------------------------
void MeshletCuller::UpdateBuffers(const MeshletCullerBuffers& buffers) {
    VkDescriptorBufferInfo bufInfo[3];
    bufInfo[0].buffer = buffers.meshletData;
    bufInfo[0].offset = 0;
    bufInfo[0].range  = VK_WHOLE_SIZE;

    bufInfo[1].buffer = buffers.indirectDraw;
    bufInfo[1].offset = 0;
    bufInfo[1].range  = VK_WHOLE_SIZE;

    bufInfo[2].buffer = buffers.visibleCount;
    bufInfo[2].offset = 0;
    bufInfo[2].range  = VK_WHOLE_SIZE;

    VkWriteDescriptorSet writes[3] = {};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = m_descSet;
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo     = &bufInfo[0];

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = m_descSet;
    writes[1].dstBinding      = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo     = &bufInfo[1];

    writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet          = m_descSet;
    writes[2].dstBinding      = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo     = &bufInfo[2];

    vkUpdateDescriptorSets(m_device, 3, writes, 0, nullptr);
}

}   // namespace GoCL