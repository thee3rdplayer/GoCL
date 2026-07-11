#include "RenderPass.h"

namespace GoCL {

// ---------------------------------------------------------------------------
// Creates a simple render pass with one colour attachment (swapchain image)
// and an optional depth/stencil attachment. The render pass has a single
// subpass and an external dependency to ensure proper synchronization.
//
// The depth attachment uses DONT_CARE for storeOp, which is bandwidth‑friendly
// on tile‑based GPUs (Mali, Adreno, Maxwell) because the depth buffer does
// not need to be preserved after the render pass.
// ---------------------------------------------------------------------------
bool RenderPass::init(GoCLContext& ctx,
                      VkFormat colorFormat,
                      VkFormat depthFormat,
                      VkSampleCountFlagBits samples,
                      VkImageLayout         finalColorLayout,
                      bool                  hasDepth) noexcept {
    // If this object is being reinitialised, destroy the existing render pass
    // to prevent a resource leak.
    if (m_renderPass != VK_NULL_HANDLE) {
        destroy();
    }

    m_device = ctx.device;

    // Both formats must be valid; the caller is responsible for querying
    // supported formats (e.g., from Swapchain and device limits).
    if (colorFormat == VK_FORMAT_UNDEFINED || depthFormat == VK_FORMAT_UNDEFINED) {
        return false;
    }

    // ---- Colour attachment description ----
    // The colour attachment is cleared at the start of the render pass,
    // stored to the swapchain image, and its final layout is determined by
    // the caller (typically VK_IMAGE_LAYOUT_PRESENT_SRC_KHR).
    VkAttachmentDescription color{};
    color.format         = colorFormat;
    color.samples        = samples;                         // MSAA sample count
    color.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;     // clear to a colour
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;    // store result
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;       // we don't care about previous content
    color.finalLayout    = finalColorLayout;                // depends on swapchain usage

    // Colour attachment reference (used in the subpass description).
    VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

    // ---- Subpass description ----
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &colorRef;

    // ---- Optional depth/stencil attachment ----
    VkAttachmentDescription depth{};
    VkAttachmentReference   depthRef{};

    if (hasDepth) {
        depth.format         = depthFormat;
        depth.samples        = samples;
        depth.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR; // clear depth
        depth.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;    // no need to keep depth after pass
        depth.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        depthRef.attachment = 1;
        depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        subpass.pDepthStencilAttachment = &depthRef;
    }

    // ---- Subpass dependency: external → subpass 0 ----
    // This ensures that the colour attachment write (and depth write, if present)
    // happens after any previous operations that might have been writing to the
    // same images (e.g., from previous frames or other passes).
    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    if (hasDepth) {
        dep.srcStageMask  |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.dstStageMask  |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    // ---- Render pass creation ----
    VkAttachmentDescription attachments[2] = { color, depth };
    VkRenderPassCreateInfo ci{};
    ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = hasDepth ? 2 : 1;
    ci.pAttachments    = attachments;
    ci.subpassCount    = 1;
    ci.pSubpasses      = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies   = &dep;

    VkResult r = vkCreateRenderPass(m_device, &ci, nullptr, &m_renderPass);
    if (r != VK_SUCCESS) {
        m_renderPass = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Destroys the Vulkan render pass object and resets the handle to null.
// Idempotent and safe to call multiple times.
// ---------------------------------------------------------------------------
void RenderPass::destroy() noexcept {
    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }

    m_device = VK_NULL_HANDLE;
}

}   // namespace GoCL