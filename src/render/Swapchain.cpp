#include "Swapchain.h"
#include "core/ConfigSingleton.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>

namespace GoCL {

// =============================================================================
// Swapchain implementation.
//
// The init sequence:
//   1. Query surface capabilities.
//   2. Choose format, present mode, and extent.
//   3. Determine image count.
//   4. Create the VkSwapchainKHR.
//   5. Retrieve the actual images.
//   6. Create VkImageViews for each image.
//
// If the present queue family differs from graphics, CONCURRENT sharing mode
// is used so both queues can access the images without explicit ownership
// transfers.
// =============================================================================

// ---------------------------------------------------------------------------
// Query surface capabilities, formats, and present modes.
// Returns an empty structure on any failure; the caller must check.
// ---------------------------------------------------------------------------
SwapchainSupportInfo Swapchain::querySupport(VkPhysicalDevice gpu, VkSurfaceKHR surface) {
    SwapchainSupportInfo info;

    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &info.capabilities);
    if (r != VK_SUCCESS) {
        return {};
    }

    uint32_t n = 0;
    r = vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, nullptr);
    if (r != VK_SUCCESS) {
        return {};
    }
    info.formats.resize(n);
    r = vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, info.formats.data());
    if (r != VK_SUCCESS) {
        return {};
    }

    r = vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &n, nullptr);
    if (r != VK_SUCCESS) {
        return {};
    }
    info.presentModes.resize(n);
    r = vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &n, info.presentModes.data());
    if (r != VK_SUCCESS) {
        return {};
    }

    return info;
}

// ---------------------------------------------------------------------------
// Chooses the best surface format: B8G8R8A8_SRGB (or R8G8B8A8_SRGB) if available,
// otherwise falls back to any format reported by the driver.
// The choice favours sRGB colour space for correct colour handling.
// ---------------------------------------------------------------------------
VkSurfaceFormatKHR Swapchain::chooseSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& formats) {
    if (formats.empty()) {
        fprintf(stderr, "GoCL: No surface formats available.\n");
        return { VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    }

    // Prefer B8G8R8A8 with sRGB (widest desktop support)
    for (const auto& f : formats) {
        if (f.format == kPreferredFormat && f.colorSpace == kPreferredColorSpace) {
            return f;
        }
    }

    // Prefer explicit sRGB format before falling back to UNORM+sRGB
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_R8G8B8A8_SRGB && f.colorSpace == kPreferredColorSpace) {
            return f;
        }
    }

    // Legacy fallback: UNORM with sRGB color space
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM && f.colorSpace == kPreferredColorSpace) {
            return f;
        }
    }

    return formats[0];
}

// ---------------------------------------------------------------------------
// Selects the present mode, respecting the user's preference from GoCL.conf.
// Falls back to MAILBOX (low latency) if available, otherwise FIFO (VSync).
// ---------------------------------------------------------------------------
static VkPresentModeKHR ChoosePresentModeWithConfig(
    const std::vector<VkPresentModeKHR>& availableModes,
    const std::string& preferred) {
    VkPresentModeKHR desired = VK_PRESENT_MODE_MAX_ENUM_KHR;

    if (preferred == "fifo") {
        desired = VK_PRESENT_MODE_FIFO_KHR;
    } else if (preferred == "fifo_relaxed") {
        desired = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    } else if (preferred == "mailbox") {
        desired = VK_PRESENT_MODE_MAILBOX_KHR;
    } else if (preferred == "immediate") {
        desired = VK_PRESENT_MODE_IMMEDIATE_KHR;
    } else if (preferred.empty()) {
        desired = VK_PRESENT_MODE_MAX_ENUM_KHR;
    } else {
        fprintf(stderr, "GoCL: unknown present mode '%s', using default\n", preferred.c_str());
        desired = VK_PRESENT_MODE_MAX_ENUM_KHR;
    }

    if (desired != VK_PRESENT_MODE_MAX_ENUM_KHR) {
        for (auto m : availableModes) {
            if (m == desired) {
                return m;
            }
        }
        fprintf(stderr, "GoCL: present mode '%s' not supported, falling back\n", preferred.c_str());
    }

    // MAILBOX is preferred for low latency; FIFO is the universal fallback.
    for (auto m : availableModes) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
            return m;
        }
    }

    return VK_PRESENT_MODE_FIFO_KHR;
}

// ---------------------------------------------------------------------------
// Chooses the swapchain extent. If the surface forces a specific extent
// (currentExtent != max), that extent is used. Otherwise, the requested
// dimensions are clamped to the surface's min/max image extents.
// ---------------------------------------------------------------------------
VkExtent2D Swapchain::chooseExtent(const VkSurfaceCapabilitiesKHR& caps,
                                   uint32_t w, uint32_t h) {
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return caps.currentExtent;
    }

    return {
        std::clamp(w, caps.minImageExtent.width,  caps.maxImageExtent.width),
        std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height)
    };
}

// ---------------------------------------------------------------------------
// Initialises the swapchain. Parameters:
//   desiredImageCount : 0 = auto (min+1), otherwise caller‑specified.
//   presentFamily     : queue family that will be used for presentation
//                       (may be different from graphics family).
//   oldSwapchain      : previous swapchain for reuse (when resizing).
// ---------------------------------------------------------------------------
bool Swapchain::init(GoCLContext& ctx, VkSurfaceKHR surface,
                     uint32_t width, uint32_t height,
                     uint32_t desiredImageCount,
                     uint32_t presentFamily,
                     VkSwapchainKHR oldSwapchain) {
    if (m_swapchain != VK_NULL_HANDLE) {
        destroy();
    }

    m_device        = ctx.device;
    m_surface       = surface;
    m_presentFamily = (presentFamily == UINT32_MAX) ? ctx.gfxFamily : presentFamily;

    auto support = querySupport(ctx.physDev, surface);
    if (support.formats.empty()) {
        fprintf(stderr, "GoCL: Surface reports no formats.\n");
        return false;
    }

    if (support.presentModes.empty()) {
        fprintf(stderr, "GoCL: Surface reports no present modes.\n");
        return false;
    }

    auto fmt    = chooseSurfaceFormat(support.formats);
    const auto& cfg = GetConfig();
    auto mode   = ChoosePresentModeWithConfig(support.presentModes, cfg.preferredPresentMode);
    auto extent = chooseExtent(support.capabilities, width, height);

    // Image count: caller override -> auto (minImageCount+1) -> clamp to surface limits.
    uint32_t imgCount = desiredImageCount;
    if (imgCount == 0) {
        imgCount = support.capabilities.minImageCount + 1;
    }
    imgCount = std::max(imgCount, support.capabilities.minImageCount);
    if (support.capabilities.maxImageCount > 0) {
        imgCount = std::min(imgCount, support.capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR ci{};
    ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface          = surface;
    ci.minImageCount    = imgCount;
    ci.imageFormat      = fmt.format;
    ci.imageColorSpace  = fmt.colorSpace;
    ci.imageExtent      = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;   // allow transfers for DRS upscale
    ci.preTransform     = support.capabilities.currentTransform;

    // Choose a supported composite alpha (prefer opaque).
    if (support.capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    } else {
        ci.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(
            support.capabilities.supportedCompositeAlpha &
            -support.capabilities.supportedCompositeAlpha);
    }

    ci.presentMode      = mode;
    ci.clipped          = VK_TRUE;
    ci.oldSwapchain     = oldSwapchain;   // enables resource reuse during resize

    // Concurrent sharing mode if graphics and present queues differ.
    uint32_t queueFamilies[] = { ctx.gfxFamily, m_presentFamily };
    if (ctx.gfxFamily != m_presentFamily) {
        ci.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices   = queueFamilies;
    } else {
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VkResult r = vkCreateSwapchainKHR(m_device, &ci, nullptr, &m_swapchain);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "GoCL: vkCreateSwapchainKHR failed (%d)\n", r);
        return false;
    }

    m_format = fmt.format;
    m_extent = extent;

    uint32_t count = 0;
    VkResult imgRes = vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, nullptr);
    if (imgRes != VK_SUCCESS) {
        fprintf(stderr, "GoCL: Failed to get swapchain image count.\n");
        return false;
    }

    m_images.resize(count);
    imgRes = vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, m_images.data());
    if (imgRes != VK_SUCCESS) {
        fprintf(stderr, "GoCL: Failed to retrieve swapchain images.\n");
        return false;
    }

    return createImageViews(m_device);
}

// ---------------------------------------------------------------------------
// Creates VkImageViews for each swapchain image.
// On failure, destroys any already created views and returns false.
// ---------------------------------------------------------------------------
bool Swapchain::createImageViews(VkDevice device) {
    if (m_format == VK_FORMAT_UNDEFINED) {
        fprintf(stderr, "GoCL: Swapchain has no valid format.\n");
        return false;
    }

    m_imageViews.resize(m_images.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < m_images.size(); ++i) {
        VkImageViewCreateInfo ci{};
        ci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image                           = m_images[i];
        ci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        ci.format                          = m_format;
        ci.components                      = { VK_COMPONENT_SWIZZLE_IDENTITY,
                                               VK_COMPONENT_SWIZZLE_IDENTITY,
                                               VK_COMPONENT_SWIZZLE_IDENTITY,
                                               VK_COMPONENT_SWIZZLE_IDENTITY };
        ci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        ci.subresourceRange.baseMipLevel   = 0;
        ci.subresourceRange.levelCount     = 1;
        ci.subresourceRange.baseArrayLayer = 0;
        ci.subresourceRange.layerCount     = 1;

        if (vkCreateImageView(device, &ci, nullptr, &m_imageViews[i]) != VK_SUCCESS) {
            // Rollback all views created so far.
            for (size_t j = 0; j < i; ++j) {
                if (m_imageViews[j]) {
                    vkDestroyImageView(device, m_imageViews[j], nullptr);
                    m_imageViews[j] = VK_NULL_HANDLE;
                }
            }
            m_imageViews.clear();
            return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Destroys the swapchain, its images, and image views.
// Idempotent and safe to call multiple times.
// ---------------------------------------------------------------------------
void Swapchain::destroy() {
    if (!m_device) {
        return;
    }

    for (auto v : m_imageViews) {
        if (v) {
            vkDestroyImageView(m_device, v, nullptr);
        }
    }
    m_imageViews.clear();
    m_images.clear();

    if (m_swapchain) {
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }

    m_format  = VK_FORMAT_UNDEFINED;
    m_extent  = {};
    m_surface = VK_NULL_HANDLE;
    m_device  = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Recreates the swapchain with new dimensions, preserving the old one for
// resource reuse. Used during window resize or when the surface becomes
// out‑of‑date.
// ---------------------------------------------------------------------------
bool Swapchain::recreate(GoCLContext& ctx, uint32_t width, uint32_t height) {
    VkSwapchainKHR old = m_swapchain;
    m_swapchain = VK_NULL_HANDLE;

    for (auto v : m_imageViews) {
        if (v) {
            vkDestroyImageView(m_device, v, nullptr);
        }
    }
    m_imageViews.clear();
    m_images.clear();

    // Pass the old swapchain to init for resource reuse.
    bool ok = init(ctx, m_surface, width, height, 0, m_presentFamily, old);

    // Regardless of success or failure, destroy the old handle.
    if (old != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_device, old, nullptr);
    }

    if (!ok) {
        destroy();   // clean up partially‑created resources
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Acquires the next swapchain image index.
// The timeout is 1 second; the caller must handle VK_SUBOPTIMAL_KHR and
// VK_ERROR_OUT_OF_DATE_KHR by calling recreate().
// ---------------------------------------------------------------------------
VkResult Swapchain::acquireNextImage(VkDevice device,
                                     VkSemaphore imageReady,
                                     uint32_t& outIndex) {
    return vkAcquireNextImageKHR(device, m_swapchain,
                                  1'000'000'000ULL,
                                  imageReady, VK_NULL_HANDLE, &outIndex);
}

}   // namespace GoCL