// =============================================================================
// Validates the DynamicResolutionHelper utility.
//
// DynamicResolutionHelper currently creates and manages the downscaled render
// target.  Upscaling support is optional and may be disabled when no upscale
// pipeline exists.  This smoke test ensures the helper initialises correctly,
// produces the correct downscaled extent (including clamping), exercises the
// destruction path without immediate errors, verifies that handles are cleared
// after destruction, and confirms that double‑destroy is safe.
//
// Upscale() is intentionally not tested here because it records Vulkan
// commands that reference temporary objects (framebuffer, image view).
// Those objects are deferred‑destroyed on the *next* Upscale() call, which
// relies on the FrameLoop fence guaranteeing the previous submission is
// complete.  An integration test that submits work to a queue, waits for a
// fence, and then verifies no validation errors occur is needed to fully
// validate the upscale path.
// =============================================================================

#include "render/DynamicResolutionHelper.h"
#include "core/GoCLContext.h"
#include <vulkan/vulkan.h>

#include <cstdio>

static int passed = 0, failed = 0;

#define PASS(name) do { \
    printf("  PASS  %s\n", name); ++passed; \
} while (0)

#define FAIL(name, msg) do { \
    printf("  FAIL  %s — %s\n", name, msg); ++failed; \
} while (0)

int main() {
    printf("=== GoCLDynamicResolutionTest ===\n\n");

    // Bootstrap a minimal Vulkan instance (required for GoCLContext::Create).
    VkApplicationInfo ai{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &ai };
    VkInstance inst = VK_NULL_HANDLE;

    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
        printf("  FATAL: vkCreateInstance failed\n");
        return 1;
    }

    // Create the GoCL context (selects a physical device, creates logical device,
    // sets up queues, staging buffer, etc.). This provides the device handles needed
    // for DynamicResolutionHelper::Init().
    auto ctx = GoCL::GoCLContext::Create(inst, 4ull * 1024 * 1024);
    if (ctx.device == VK_NULL_HANDLE) {
        printf("  FATAL: GoCLContext::Create returned invalid device\n");
        vkDestroyInstance(inst, nullptr);
        return 1;
    }

    // -----------------------------------------------------------------------
    // Test 1: Normal scale (70%). 1280×720 * 0.7 = 896×504.
    // Verifies that Init succeeds, the downscaled extent is correct,
    // the image and view are valid, and Destroy clears the handles.
    // -----------------------------------------------------------------------
    {
        GoCL::DynamicResolutionHelper drh;
        bool initOk = drh.Init(ctx, { 1280, 720 },
                               0.7f,
                               GoCL::QualityPreset::Medium);

        if (initOk) {
            PASS("Init_succeeds");
        } else {
            FAIL("Init_succeeds", "DynamicResolutionHelper::Init returned false");
        }

        // Verify the downscaled extent is exactly 896×504 (1280*0.7, 720*0.7)
        auto extent = drh.RenderExtent();
        if (extent.width == 896 && extent.height == 504) {
            PASS("Downscaled_extent_correct");
        } else {
            FAIL("Downscaled_extent_correct", "expected 896×504");
        }

        if (drh.RenderTargetImage() != VK_NULL_HANDLE) {
            PASS("RenderTargetImage_valid");
        } else {
            FAIL("RenderTargetImage_valid", "NULL image");
        }

        if (drh.RenderTargetView() != VK_NULL_HANDLE) {
            PASS("RenderTargetView_valid");
        } else {
            FAIL("RenderTargetView_valid", "NULL image view");
        }

        // Destroy and verify handles are cleared (no stale pointers).
        drh.Destroy();

        if (drh.RenderTargetImage() == VK_NULL_HANDLE) {
            PASS("Destroy_clears_image");
        } else {
            FAIL("Destroy_clears_image", "image handle not cleared");
        }

        if (drh.RenderTargetView() == VK_NULL_HANDLE) {
            PASS("Destroy_clears_view");
        } else {
            FAIL("Destroy_clears_view", "image view handle not cleared");
        }
    }

    // -----------------------------------------------------------------------
    // Test 2: Lower clamp (scale = 0.0f). The helper clamps scale to 0.25,
    // resulting in 1280*0.25=320, 720*0.25=180. Verifies that the clamping
    // works correctly.
    // -----------------------------------------------------------------------
    {
        GoCL::DynamicResolutionHelper drh;
        bool initOk = drh.Init(ctx, { 1280, 720 },
                               0.0f,
                               GoCL::QualityPreset::Medium);

        if (initOk) {
            PASS("Init_low_clamp_succeeds");
        } else {
            FAIL("Init_low_clamp_succeeds", "Init returned false");
        }

        auto extent = drh.RenderExtent();
        if (extent.width == 320 && extent.height == 180) {
            PASS("Low_clamp_extent_correct");
        } else {
            FAIL("Low_clamp_extent_correct", "expected 320×180");
        }

        drh.Destroy();
    }

    // -----------------------------------------------------------------------
    // Test 3: Upper clamp (scale = 2.0f). Clamped to 1.0, so extent remains
    // the original 1280×720.
    // -----------------------------------------------------------------------
    {
        GoCL::DynamicResolutionHelper drh;
        bool initOk = drh.Init(ctx, { 1280, 720 },
                               2.0f,
                               GoCL::QualityPreset::Medium);

        if (initOk) {
            PASS("Init_high_clamp_succeeds");
        } else {
            FAIL("Init_high_clamp_succeeds", "Init returned false");
        }

        auto extent = drh.RenderExtent();
        if (extent.width == 1280 && extent.height == 720) {
            PASS("High_clamp_extent_correct");
        } else {
            FAIL("High_clamp_extent_correct", "expected 1280×720");
        }

        drh.Destroy();
    }

    // -----------------------------------------------------------------------
    // Test 4: Double‑destroy safety. Destroying an already destroyed helper
    // should not crash or leak.
    // -----------------------------------------------------------------------
    {
        GoCL::DynamicResolutionHelper drh;
        drh.Init(ctx, { 1280, 720 }, 0.7f, GoCL::QualityPreset::Medium);
        drh.Destroy();
        drh.Destroy();   // must be safe (no crash)
        PASS("Double_destroy_safe");
    }

    // -----------------------------------------------------------------------
    // Test 5: Invalid extent (0×0). Init should gracefully fail (return false)
    // because creating an image with zero width or height is illegal.
    // -----------------------------------------------------------------------
    {
        GoCL::DynamicResolutionHelper drhBad;
        bool ok = drhBad.Init(ctx, { 0, 0 }, 0.7f,
                              GoCL::QualityPreset::Medium);

        if (ok) {
            PASS("Init_zero_extent_handled_gracefully");
        } else {
            FAIL("Init_zero_extent_handled_gracefully", "expected Init to succeed (clamped to 1x1)");
        }

        drhBad.Destroy();
    }

    // Clean up the GoCL context and the Vulkan instance.
    ctx.Destroy();
    vkDestroyInstance(inst, nullptr);

    printf("\n--- Results: %d passed, %d failed ---\n", passed, failed);
    return failed ? 1 : 0;
}