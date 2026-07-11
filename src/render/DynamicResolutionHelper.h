#pragma once

#include "core/GoCLContext.h"
#include <vulkan/vulkan.h>

#include <vector>

namespace GoCL {

/**
 * @brief Quality presets for the high‑quality upscaling shader.
 * 
 * Higher quality presets may use more complex filters (e.g., Lanczos, FSR).
 * Currently only the bilinear upscaler is implemented; presets are reserved for future use.
 */
enum class QualityPreset {
    Low,    ///< Fastest, lowest quality (nearest neighbour)
    Medium, ///< Default (bilinear)
    High,   ///< Sharper filter (e.g., Lanczos)
    Ultra   ///< Highest quality, most expensive
};

/**
 * @brief Helper that implements dynamic resolution scaling for the engine.
 * 
 * Renders the scene into a smaller off‑screen target, then upscales the result
 * to the swapchain image using a high‑quality filter.
 *
 * Requires that the application uses a graphics pipeline with dynamic viewport/scissor
 * and that the downscaled render target is used as the colour attachment.
 * Not used by the proxy; only available when linking against the static library.
 */
class DynamicResolutionHelper {
public:
    DynamicResolutionHelper() = default;
    ~DynamicResolutionHelper() noexcept { Destroy(); }

    DynamicResolutionHelper(DynamicResolutionHelper&&) noexcept = default;
    DynamicResolutionHelper& operator=(DynamicResolutionHelper&&) noexcept = default;

    /**
     * @brief Initialises the helper: creates the downscaled target, descriptor set,
     *        pipeline layout and render pass.
     * 
     * The actual upscaling pipeline is not yet created because shader binaries are
     * not embedded. See the .cpp file for how to enable it by uncommenting the
     * pipeline creation block and embedding the SPIR‑V shaders via xxd.
     *
     * @param ctx GoCLContext containing device and physical device handles.
     * @param swapchainExtent Original swapchain dimensions.
     * @param scale Render scale factor (clamped to 0.25–1.0).
     * @param preset Quality preset (currently ignored; only bilinear upscale is available).
     * @param swapchainFormat Must match the swapchain image format (default B8G8R8A8_UNORM).
     * @return true on success, false on failure.
     */
    bool Init(const GoCLContext& ctx, VkExtent2D swapchainExtent,
              float scale, QualityPreset preset,
              VkFormat swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM);

    /**
     * @brief Destroys all Vulkan resources created by the helper.
     * 
     * Idempotent and safe to call multiple times.
     */
    void Destroy() noexcept;

    /**
     * @brief Returns true if the downscaled target was successfully created.
     */
    [[nodiscard]]
    bool IsInitialized() const noexcept {
        return m_downscaledImage != VK_NULL_HANDLE;
    }

    // ─── Accessors ──────────────────────────────────────────────────────────────

    /**
     * @brief Returns the image view of the downscaled render target.
     */
    [[nodiscard]]
    VkImageView RenderTargetView() const noexcept { return m_downscaledView; }

    /**
     * @brief Returns the extent of the downscaled render target.
     */
    [[nodiscard]]
    VkExtent2D RenderExtent() const noexcept { return m_downscaledExtent; }

    /**
     * @brief Returns the image handle of the downscaled render target.
     */
    [[nodiscard]]
    VkImage RenderTargetImage() const noexcept { return m_downscaledImage; }

    /**
     * @brief Records commands to upscale the downscaled target into the swapchain image.
     * 
     * Must be called after the scene has been rendered into the downscaled target.
     * Creates temporary framebuffer and image view for the swapchain image;
     * destroys them on the next call to avoid synchronisation issues.
     *
     * @param cmd Command buffer to record into.
     * @param swapchainImage Swapchain image to upscale into.
     * @param swapchainExtent Extent of the swapchain image.
     */
    void Upscale(VkCommandBuffer cmd, VkImage swapchainImage, VkExtent2D swapchainExtent);

private:
    VkDevice        m_device         = VK_NULL_HANDLE;
    VkPhysicalDevice m_physDev       = VK_NULL_HANDLE;

    // ─── Downscaled render target ──────────────────────────────────────────────
    VkImage         m_downscaledImage  = VK_NULL_HANDLE;  ///< Colour attachment, sampled, transfer source
    VkDeviceMemory  m_downscaledMemory = VK_NULL_HANDLE;
    VkImageView     m_downscaledView   = VK_NULL_HANDLE;
    VkExtent2D      m_downscaledExtent{};
    VkFormat        m_swapchainFormat  = VK_FORMAT_UNDEFINED;

    // ─── Upscale pipeline objects ──────────────────────────────────────────────
    VkPipeline       m_upscalePipeline    = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout     = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet  m_descSet            = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool           = VK_NULL_HANDLE;
    VkSampler        m_sampler            = VK_NULL_HANDLE;
    VkRenderPass     m_renderPass         = VK_NULL_HANDLE;
    QualityPreset    m_qualityPreset      = QualityPreset::Medium;

    // ─── Deferred destruction ──────────────────────────────────────────────────
    // Objects created during Upscale() (framebuffer, image view) are stored here
    // and destroyed at the beginning of the next Upscale() call, after the previous
    // frame's fence has been waited on. This avoids destroying resources while
    // the GPU may still be using them.
    std::vector<VkFramebuffer> m_pendingFramebuffers;
    std::vector<VkImageView>   m_pendingImageViews;
};

}   // namespace GoCL