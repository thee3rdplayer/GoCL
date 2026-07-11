#pragma once

#include "core/GoCLContext.h"
#include <vulkan/vulkan.h>

#include <vector>
#include <cstdint>

namespace GoCL {

/**
 * @brief Container for surface capabilities, formats, and present modes queried from the physical device.
 */
struct SwapchainSupportInfo {
    VkSurfaceCapabilitiesKHR        capabilities{}; ///< Min/max image count, extents, etc.
    std::vector<VkSurfaceFormatKHR> formats;        ///< Supported colour formats + colour spaces
    std::vector<VkPresentModeKHR>   presentModes;   ///< Supported presentation modes (FIFO, MAILBOX, …)
};

/**
 * @brief Manages the Vulkan swapchain (images that are presented to the window).
 * 
 * Handles creation, resizing, and destruction. The swapchain is tied to a surface.
 */
class Swapchain {
public:
    /// Preferred (not guaranteed) surface format: B8G8R8A8_UNORM with sRGB colour space.
    static constexpr VkFormat        kPreferredFormat     = VK_FORMAT_B8G8R8A8_UNORM;
    static constexpr VkColorSpaceKHR kPreferredColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

    Swapchain()  = default;
    ~Swapchain() { destroy(); }

    // Move‑only (no copy).
    Swapchain(Swapchain&&) noexcept = default;
    Swapchain& operator=(Swapchain&&) noexcept = default;

    // -----------------------------------------------------------------------
    /**
     * @brief Initialises the swapchain.
     * 
     * @param ctx GoCLContext containing device and physical device handles.
     * @param surface Vulkan surface for presentation.
     * @param width Initial swapchain width.
     * @param height Initial swapchain height.
     * @param desiredImageCount 0 → auto (minImageCount + 1, clamped to max). Default: 0.
     * @param presentFamily Queue family used for presentation (UINT32_MAX → use ctx.gfxFamily). Default: UINT32_MAX.
     * @param oldSwapchain Previous swapchain when resizing (VK_NULL_HANDLE for first creation). Default: VK_NULL_HANDLE.
     * @return true on success, false on failure (e.g., unsupported surface, out of memory).
     */
    [[nodiscard]] bool init(GoCLContext& ctx, VkSurfaceKHR surface,
              uint32_t width, uint32_t height,
              uint32_t desiredImageCount = 0,
              uint32_t presentFamily     = UINT32_MAX,
              VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE);

    /**
     * @brief Destroys the swapchain and all associated resources. Idempotent.
     */
    void destroy();

    /**
     * @brief Recreates the swapchain with new dimensions (used after window resize).
     * 
     * Preserves the old swapchain for resource reuse.
     * 
     * @param ctx GoCLContext containing device and physical device handles.
     * @param width New swapchain width.
     * @param height New swapchain height.
     * @return true on success, false on failure.
     */
    [[nodiscard]] bool recreate(GoCLContext& ctx, uint32_t width, uint32_t height);

    /**
     * @brief Acquires the next available swapchain image index.
     * 
     * @param device Vulkan device.
     * @param imageReady Semaphore that will be signalled when the image is available.
     * @param outIndex Output index of the acquired image.
     * @return VkResult. The caller must handle VK_SUBOPTIMAL_KHR and VK_ERROR_OUT_OF_DATE_KHR by calling recreate().
     */
    [[nodiscard]] VkResult acquireNextImage(VkDevice device, VkSemaphore imageReady, uint32_t& outIndex);

    // ─── Getters ──────────────────────────────────────────────────────────────

    [[nodiscard]] VkSwapchainKHR handle()              const { return m_swapchain; }
    [[nodiscard]] VkFormat       format()              const { return m_format; }
    [[nodiscard]] VkExtent2D     extent()              const { return m_extent; }
    [[nodiscard]] uint32_t       imageCount()          const { return static_cast<uint32_t>(m_imageViews.size()); }

    /**
     * @brief Returns the image view for the given swapchain image index.
     * 
     * Valid until recreate() or destroy().
     */
    [[nodiscard]] VkImageView    imageView(uint32_t i) const { return m_imageViews.at(i); }

    /**
     * @brief Returns the image handle for the given swapchain image index.
     * 
     * Valid until recreate() or destroy().
     */
    [[nodiscard]] VkImage        image(uint32_t i)     const { return m_images.at(i); }

private:
    /**
     * @brief Queries surface support from the physical device.
     */
    static SwapchainSupportInfo querySupport(VkPhysicalDevice gpu, VkSurfaceKHR surface);

    /**
     * @brief Chooses the best surface format (prefers B8G8R8A8_SRGB).
     */
    static VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats);

    /**
     * @brief Determines the swapchain extent (clamped to surface limits).
     */
    static VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& caps,
                                   uint32_t w, uint32_t h);

    /**
     * @brief Creates VkImageViews for all swapchain images.
     * 
     * @param device Vulkan device.
     * @return true on success, false on failure.
     */
    bool createImageViews(VkDevice device);

    // ─── Vulkan handles ──────────────────────────────────────────────────────
    VkDevice             m_device        = VK_NULL_HANDLE;  ///< Logical device (for destruction)

    // Non-owning handles (owned by Vulkan).
    VkSurfaceKHR         m_surface       = VK_NULL_HANDLE;
    VkSwapchainKHR       m_swapchain     = VK_NULL_HANDLE;
    VkFormat             m_format        = VK_FORMAT_UNDEFINED;
    VkExtent2D           m_extent        = {};
    uint32_t             m_presentFamily = UINT32_MAX;  ///< Queue family used for present

    // Swapchain images and their views (owned by the swapchain).
    std::vector<VkImage>     m_images;
    std::vector<VkImageView> m_imageViews;
};

}   // namespace GoCL