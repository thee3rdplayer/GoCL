// Exhaustive tests for ASTCBlockSelector + compressedSize/bpp math.
// No external test framework — intentional: zero extra CI dependencies.
//
// Coverage:
//   1. Block selection: all TextureUsage branches + VRAM pressure + AnimatedUV
//   2. Boundary conditions at the rawMB > 4 and rawMB > 8 thresholds
//   3. compressedSize() — aligned & non-aligned (ceil) dimensions
//   4. bpp() — spot-check all footprints the selector actually emits
//   5. ASTC→ETC2 fallback stub (extend once TextureManager exposes isFallback())

#include "../src/render/ASTCBlockSelector.h"

#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <cmath>
#include <string> 

// ---------------------------------------------------------------------------
// Minimal test harness
// ---------------------------------------------------------------------------
static int g_passed = 0, g_failed = 0;

#define ASSERT(cond)                                                        \
    do {                                                                    \
        if (!(cond))                                                        \
            throw std::runtime_error("ASSERT failed: " #cond               \
                                     "  [" __FILE__ ":" + std::to_string(__LINE__) + "]"); \
    } while (0)

#define ASSERT_EQ(a, b)                                                     \
    do {                                                                    \
        auto _a = (a); auto _b = (b);                                       \
        if (_a != _b)                                                       \
            throw std::runtime_error(                                       \
                std::string("ASSERT_EQ failed: ") + #a + " == " + #b +     \
                "  got " + std::to_string(_a) + " vs " + std::to_string(_b) + \
                "  [" __FILE__ ":" + std::to_string(__LINE__) + "]");       \
    } while (0)

#define ASSERT_NEAR(a, b, eps)                                              \
    do {                                                                    \
        float _a = (a), _b = (b);                                           \
        if (std::fabs(_a - _b) > (eps))                                     \
            throw std::runtime_error(                                       \
                std::string("ASSERT_NEAR failed: |") + #a + " - " + #b +   \
                "| > " #eps "  got " + std::to_string(_a) +                 \
                " vs " + std::to_string(_b) +                               \
                "  [" __FILE__ ":" + std::to_string(__LINE__) + "]");       \
    } while (0)

#define RUN_TEST(name)                                                      \
    do {                                                                    \
        try { test_##name(); ++g_passed;                                    \
              std::printf("  PASS  %s\n", #name); }                        \
        catch (const std::exception& e) {                                   \
            ++g_failed;                                                     \
            std::printf("  FAIL  %s\n    %s\n", #name, e.what()); }        \
    } while (0)

// ---------------------------------------------------------------------------
// Helpers — mirrors the rawMB formula in ASTCBlockSelector::Select()
// ---------------------------------------------------------------------------
static float rawMB(uint32_t w, uint32_t h) {
    return (w * h * 4.0f) / (1024.0f * 1024.0f);
}

using namespace GoCL;

// ===== 1. AnimatedUV always forces 4×4 regardless of everything else =======

void test_AnimatedUV_overrides_large_texture() {
    // 4096×4096 = 64 MB raw — without flag this would be 6×6
    auto p = ASTCBlockSelector::Select(TextureUsage::Diffuse, 4096, 4096, 64, true);
    ASSERT_EQ(p.blockW, 4);
    ASSERT_EQ(p.blockH, 4);
}

void test_AnimatedUV_overrides_low_vram() {
    auto p = ASTCBlockSelector::Select(TextureUsage::Diffuse, 256, 256, 16, true);
    ASSERT_EQ(p.blockW, 4);
    ASSERT_EQ(p.blockH, 4);
}

void test_AnimatedUV_overrides_normal_map() {
    auto p = ASTCBlockSelector::Select(TextureUsage::Normal, 2048, 2048, 256, true);
    ASSERT_EQ(p.blockW, 4);
    ASSERT_EQ(p.blockH, 4);
}

// ===== 2. Normal maps =======================================================

void test_Normal_small_texture_4x4() {
    // width == 1024: boundary — spec says > 1024 for 5×5
    auto p = ASTCBlockSelector::Select(TextureUsage::Normal, 1024, 1024, 1024, false);
    ASSERT_EQ(p.blockW, 4);
    ASSERT_EQ(p.blockH, 4);
}

void test_Normal_large_texture_5x5() {
    auto p = ASTCBlockSelector::Select(TextureUsage::Normal, 1025, 1024, 1024, false);
    ASSERT_EQ(p.blockW, 5);
    ASSERT_EQ(p.blockH, 5);
}

void test_Normal_4k_texture_5x5() {
    auto p = ASTCBlockSelector::Select(TextureUsage::Normal, 4096, 4096, 2048, false);
    ASSERT_EQ(p.blockW, 5);
    ASSERT_EQ(p.blockH, 5);
}

// ===== 3. LUTs ==============================================================

void test_LUT_always_4x4() {
    // LUTs are small by nature; compression overhead not worth it
    auto p = ASTCBlockSelector::Select(TextureUsage::LUT, 256, 16, 4096, false);
    ASSERT_EQ(p.blockW, 4);
    ASSERT_EQ(p.blockH, 4);
}

// ===== 4. General heuristic — resolution thresholds ========================
// rawMB > 8  →  6×6
// rawMB > 4  →  5×5
// else       →  4×4  (subject to VRAM override)

void test_General_large_texture_6x6() {
    // 4096×1024 = 16 MB raw, ample VRAM
    ASSERT(rawMB(4096, 1024) > 8.0f);
    auto p = ASTCBlockSelector::Select(TextureUsage::Diffuse, 4096, 1024, 2048, false);
    ASSERT_EQ(p.blockW, 6);
    ASSERT_EQ(p.blockH, 6);
}

void test_General_exactBoundary_8MB_is_NOT_large() {
    // Exactly 8 MB — condition is rawMB > 8, so this falls into the next bucket
    // 2048×1024 = 2,097,152 pixels × 4 bytes = 8.0 MB exactly — NOT > 8
    ASSERT(rawMB(2048, 1024) == 8.0f);
    auto p = ASTCBlockSelector::Select(TextureUsage::Diffuse, 2048, 1024, 2048, false);
    ASSERT_EQ(p.blockW, 5);  // falls into rawMB > 4 branch
    ASSERT_EQ(p.blockH, 5);
}

void test_General_medium_texture_5x5() {
    // 1920×1080 = ~7.91 MB — > 4, < 8 → 5×5
    ASSERT(rawMB(1920, 1080) > 4.0f && rawMB(1920, 1080) < 8.0f);
    auto p = ASTCBlockSelector::Select(TextureUsage::Diffuse, 1920, 1080, 2048, false);
    ASSERT_EQ(p.blockW, 5);
    ASSERT_EQ(p.blockH, 5);
}

void test_General_exactBoundary_4MB_is_NOT_medium() {
    // 1024×1024 = 4 MB exactly — NOT > 4, so falls to 4×4
    ASSERT(rawMB(1024, 1024) == 4.0f);
    auto p = ASTCBlockSelector::Select(TextureUsage::Diffuse, 1024, 1024, 2048, false);
    ASSERT_EQ(p.blockW, 4);
    ASSERT_EQ(p.blockH, 4);
}

void test_General_small_texture_4x4() {
    // 1280×720 = ~3.52 MB — small, generous VRAM
    auto p = ASTCBlockSelector::Select(TextureUsage::Diffuse, 1280, 720, 2048, false);
    ASSERT_EQ(p.blockW, 4);
    ASSERT_EQ(p.blockH, 4);
}

// ===== 5. VRAM pressure overrides ==========================================

void test_VRAM_tight_forces_6x6_regardless_of_size() {
    // Tiny texture but VRAM < 512 MB — should compress aggressively
    auto p = ASTCBlockSelector::Select(TextureUsage::Diffuse, 512, 512, 256, false);
    ASSERT_EQ(p.blockW, 6);
    ASSERT_EQ(p.blockH, 6);
}

void test_VRAM_exactBoundary_512MB_is_NOT_tight() {
    // vramBudgetMB == 512: condition is < 512, so this is NOT tight
    auto p = ASTCBlockSelector::Select(TextureUsage::Diffuse, 512, 512, 512, false);
    // 512×512 = 1 MB raw → 4×4
    ASSERT_EQ(p.blockW, 4);
    ASSERT_EQ(p.blockH, 4);
}

void test_VRAM_tight_boundary_511MB() {
    auto p = ASTCBlockSelector::Select(TextureUsage::Diffuse, 512, 512, 511, false);
    ASSERT_EQ(p.blockW, 6);
    ASSERT_EQ(p.blockH, 6);
}

void test_VRAM_tight_overrides_HDR() {
    // HDR is not explicitly handled — falls through to general heuristic
    // Small HDR + VRAM pressure → 6×6
    auto p = ASTCBlockSelector::Select(TextureUsage::HDR, 512, 512, 100, false);
    ASSERT_EQ(p.blockW, 6);
    ASSERT_EQ(p.blockH, 6);
}

void test_VRAM_tight_does_NOT_override_normal_map() {
    // Normal map takes priority over VRAM path (it returns early)
    auto p = ASTCBlockSelector::Select(TextureUsage::Normal, 512, 512, 100, false);
    ASSERT_EQ(p.blockW, 4);  // Normal, width <= 1024
    ASSERT_EQ(p.blockH, 4);
}

void test_VRAM_tight_does_NOT_override_LUT() {
    auto p = ASTCBlockSelector::Select(TextureUsage::LUT, 64, 64, 1, false);
    ASSERT_EQ(p.blockW, 4);
    ASSERT_EQ(p.blockH, 4);
}

// ===== 6. UI usage ==========================================================

void test_UI_small_texture_4x4() {
    // UI not explicitly handled — falls through to general heuristic
    auto p = ASTCBlockSelector::Select(TextureUsage::UI, 512, 512, 2048, false);
    // rawMB = 1 → 4×4
    ASSERT_EQ(p.blockW, 4);
    ASSERT_EQ(p.blockH, 4);
}

// ===== 7. Roughness =========================================================

void test_Roughness_large_enough_for_5x5() {
    auto p = ASTCBlockSelector::Select(TextureUsage::Roughness, 1920, 1080, 2048, false);
    ASSERT_EQ(p.blockW, 5);
    ASSERT_EQ(p.blockH, 5);
}

// ===== 8. bpp() correctness =================================================

void test_bpp_4x4() {
    ASTCParams p; p.blockW = 4; p.blockH = 4;
    ASSERT_NEAR(p.bpp(), 8.0f, 0.001f);
}

void test_bpp_5x5() {
    ASTCParams p; p.blockW = 5; p.blockH = 5;
    ASSERT_NEAR(p.bpp(), 128.0f / 25.0f, 0.001f);  // 5.12
}

void test_bpp_6x6() {
    ASTCParams p; p.blockW = 6; p.blockH = 6;
    ASSERT_NEAR(p.bpp(), 128.0f / 36.0f, 0.001f);  // ~3.556
}

void test_bpp_decreases_with_block_size() {
    ASTCParams p4; p4.blockW = p4.blockH = 4;
    ASTCParams p5; p5.blockW = p5.blockH = 5;
    ASTCParams p6; p6.blockW = p6.blockH = 6;
    ASSERT(p4.bpp() > p5.bpp());
    ASSERT(p5.bpp() > p6.bpp());
}

// ===== 9. compressedSize() — aligned dimensions =============================

void test_compressedSize_4x4_on_aligned_4x4_texture() {
    ASTCParams p; p.blockW = 4; p.blockH = 4;
    // 4×4 texture: 1×1 blocks = 1 × 16 bytes
    ASSERT_EQ(p.compressedSize(4, 4), 16u);
}

void test_compressedSize_4x4_on_1024x1024() {
    ASTCParams p; p.blockW = 4; p.blockH = 4;
    // 256×256 blocks × 16 bytes = 1,048,576 bytes = 1 MB
    ASSERT_EQ(p.compressedSize(1024, 1024), 1048576u);
}

void test_compressedSize_6x6_on_1920x1080() {
    ASTCParams p; p.blockW = 6; p.blockH = 6;
    // ceil(1920/6)=320, ceil(1080/6)=180, 320×180×16 = 921,600 bytes
    ASSERT_EQ(p.compressedSize(1920, 1080), 921600u);
}

// ===== 10. compressedSize() — non-aligned (ceil) dimensions =================

void test_compressedSize_4x4_non_aligned_width() {
    ASTCParams p; p.blockW = 4; p.blockH = 4;
    // 5×4 texture: ceil(5/4)=2 wide, ceil(4/4)=1 tall → 2 blocks × 16 = 32 bytes
    ASSERT_EQ(p.compressedSize(5, 4), 32u);
}

void test_compressedSize_4x4_non_aligned_both() {
    ASTCParams p; p.blockW = 4; p.blockH = 4;
    // 5×5: ceil(5/4)=2, ceil(5/4)=2 → 4 blocks × 16 = 64 bytes
    ASSERT_EQ(p.compressedSize(5, 5), 64u);
}

void test_compressedSize_6x6_non_aligned() {
    ASTCParams p; p.blockW = 6; p.blockH = 6;
    // 7×7: ceil(7/6)=2, ceil(7/6)=2 → 4 blocks × 16 = 64 bytes
    ASSERT_EQ(p.compressedSize(7, 7), 64u);
}

void test_compressedSize_5x5_non_aligned_width() {
    ASTCParams p; p.blockW = 5; p.blockH = 5;
    // 6×5: ceil(6/5)=2, ceil(5/5)=1 → 2 blocks × 16 = 32 bytes
    ASSERT_EQ(p.compressedSize(6, 5), 32u);
}

void test_compressedSize_1x1_texture() {
    ASTCParams p; p.blockW = 4; p.blockH = 4;
    // Minimum: 1 block regardless of dimensions
    ASSERT_EQ(p.compressedSize(1, 1), 16u);
}

// ===== 11. ASTC→ETC2 fallback (stub — expand once TextureManager exposes API)
// TODO: when DeviceCapabilities::supportsASTC == false,
//       TextureManager::Upload() must silently transcode to ETC2 (RGB8 or RGBA8).
//       Test contract:
//         - Output format is VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK
//         - Resulting compressedSize matches an ETC2 4×4 block layout (8 bytes/block RGB, 16 RGBA)
//         - No VK validation errors on upload
//         - ASTCBlockSelector is NOT called when ASTC unavailable
//
// void test_ETC2_fallback_on_no_ASTC_support() {
//     DeviceCapabilities fakeCaps;
//     fakeCaps.supportsASTC = false;
//     TextureManager mgr(fakeCaps);
//     auto handle = mgr.Upload(testRGBAPixels, 256, 256, TextureUsage::Diffuse);
//     ASSERT_EQ(handle.format, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK);
// }

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------
int main() {
    std::printf("=== GoCLTextureTest ===\n\n");

    // AnimatedUV
    RUN_TEST(AnimatedUV_overrides_large_texture);
    RUN_TEST(AnimatedUV_overrides_low_vram);
    RUN_TEST(AnimatedUV_overrides_normal_map);

    // Normal maps
    RUN_TEST(Normal_small_texture_4x4);
    RUN_TEST(Normal_large_texture_5x5);
    RUN_TEST(Normal_4k_texture_5x5);

    // LUT
    RUN_TEST(LUT_always_4x4);

    // General heuristic
    RUN_TEST(General_large_texture_6x6);
    RUN_TEST(General_exactBoundary_8MB_is_NOT_large);
    RUN_TEST(General_medium_texture_5x5);
    RUN_TEST(General_exactBoundary_4MB_is_NOT_medium);
    RUN_TEST(General_small_texture_4x4);

    // VRAM pressure
    RUN_TEST(VRAM_tight_forces_6x6_regardless_of_size);
    RUN_TEST(VRAM_exactBoundary_512MB_is_NOT_tight);
    RUN_TEST(VRAM_tight_boundary_511MB);
    RUN_TEST(VRAM_tight_overrides_HDR);
    RUN_TEST(VRAM_tight_does_NOT_override_normal_map);
    RUN_TEST(VRAM_tight_does_NOT_override_LUT);

    // Other usage types
    RUN_TEST(UI_small_texture_4x4);
    RUN_TEST(Roughness_large_enough_for_5x5);

    // bpp
    RUN_TEST(bpp_4x4);
    RUN_TEST(bpp_5x5);
    RUN_TEST(bpp_6x6);
    RUN_TEST(bpp_decreases_with_block_size);

    // compressedSize — aligned
    RUN_TEST(compressedSize_4x4_on_aligned_4x4_texture);
    RUN_TEST(compressedSize_4x4_on_1024x1024);
    RUN_TEST(compressedSize_6x6_on_1920x1080);

    // compressedSize — non-aligned / ceil
    RUN_TEST(compressedSize_4x4_non_aligned_width);
    RUN_TEST(compressedSize_4x4_non_aligned_both);
    RUN_TEST(compressedSize_6x6_non_aligned);
    RUN_TEST(compressedSize_5x5_non_aligned_width);
    RUN_TEST(compressedSize_1x1_texture);

    std::printf("\n--- Results: %d passed, %d failed ---\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}