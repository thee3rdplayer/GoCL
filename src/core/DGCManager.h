#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace GoCL {

struct GoCLContext;

/**
 * @brief Manager for VK_EXT_device_generated_commands functionality.
 * 
 * Handles creation and management of indirect commands layout, execution set,
 * and associated buffers for GPU-driven command generation.
 */
class DGCManager {
public:
    DGCManager() = default;
    ~DGCManager() { destroy(); }

    // Delete copy constructors
    DGCManager(const DGCManager&) = delete;
    DGCManager& operator=(const DGCManager&) = delete;

    // Move constructors
    DGCManager(DGCManager&& other) noexcept;
    DGCManager& operator=(DGCManager&& other) noexcept;

    /**
     * @brief Initialize the DGC manager with a Vulkan context.
     * 
     * @param ctx GoCLContext that must outlive this DGCManager instance.
     *            The manager stores the device and physical device handles from ctx.
     * @return true on success, false on failure.
     */
    bool init(GoCLContext& ctx);

    /**
     * @brief Creates the indirect execution set.
     * 
     * @param maxPipelines Maximum number of different pipelines we can switch between.
     * @return true on success, false on failure.
     */
    bool initExecutionSet(uint32_t maxPipelines);

    /**
     * @brief Allocates the input stream and preprocess buffers.
     * 
     * @param maxSequences Maximum number of draw sequences we can store.
     * @param pushConstantSize Size of push constant data per sequence.
     * @return true on success, false on failure.
     */
    bool initBuffers(uint32_t maxSequences, uint32_t pushConstantSize);

    /**
     * @brief Fill the input stream buffer with per-sequence data.
     * 
     * The buffer must be mapped before calling this.
     * 
     * @param outSize Returns the size of the mapped buffer.
     * @return Pointer to mapped buffer memory.
     */
    void* mapInputBuffer(VkDeviceSize& outSize);

    /**
     * @brief Unmap the input buffer.
     */
    void unmapInputBuffer();

    /**
     * @brief Associate a pipeline with an index in the execution set.
     * 
     * Pipelines in the same execution set must be compatible:
     * - Same VkPipelineLayout
     * - Same indirect commands layout
     * - Same primitive topology (if using graphics)
     * 
     * @param pipeline Pipeline to associate.
     * @param index Index in the execution set.
     */
    void updateExecutionSetPipeline(VkPipeline pipeline, uint32_t index);

    /**
     * @brief Destroy all allocated resources.
     */
    void destroy();

    // ─── Getters ──────────────────────────────────────────────────────────────
    VkIndirectCommandsLayoutEXT layout() const { return m_layout; }
    VkIndirectExecutionSetEXT   executionSet() const { return m_executionSet; }
    VkBuffer inputBuffer() const { return m_inputBuffer; }
    VkBuffer preprocessBuffer() const { return m_preprocessBuffer; }
    uint32_t maxSequences() const { return m_maxSequences; }

    VkDeviceAddress inputDeviceAddress() const { return m_inputAddress; }
    VkDeviceAddress preprocessDeviceAddress() const { return m_preprocessAddress; }
    VkDeviceSize   preprocessSize() const { return m_preprocessSize; }

private:
    /**
     * @brief Create a buffer with the specified size and usage flags.
     * 
     * @param size Buffer size in bytes.
     * @param usage Buffer usage flags.
     * @param buffer Output buffer handle.
     * @param memory Output memory handle.
     * @return true on success, false on failure.
     */
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkBuffer& buffer, VkDeviceMemory& memory);

    // ─── Vulkan handles ──────────────────────────────────────────────────────
    VkDevice         m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physDev = VK_NULL_HANDLE;

    VkIndirectCommandsLayoutEXT m_layout = VK_NULL_HANDLE;
    VkIndirectExecutionSetEXT   m_executionSet = VK_NULL_HANDLE;

    // ─── Input stream buffers ──────────────────────────────────────────────
    VkBuffer       m_inputBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_inputMemory = VK_NULL_HANDLE;
    void*          m_inputMapped = nullptr;
    VkDeviceSize   m_inputSize = 0;

    // ─── Driver-generated preprocess storage ──────────────────────────────
    // Required only when vkGetGeneratedCommandsMemoryRequirementsEXT returns non-zero.
    VkBuffer       m_preprocessBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_preprocessMemory = VK_NULL_HANDLE;

    // ─── Buffer properties ──────────────────────────────────────────────────
    VkDeviceSize   m_preprocessSize = 0;
    VkDeviceAddress m_inputAddress = 0;
    VkDeviceAddress m_preprocessAddress = 0;

    // ─── Configuration ──────────────────────────────────────────────────────
    uint32_t m_maxSequences = 0;
    uint32_t m_currentSequenceCount = 0;
    uint32_t m_pushConstantSize = 0;
    uint32_t m_maxPipelines = 0;
};

}   // namespace GoCL