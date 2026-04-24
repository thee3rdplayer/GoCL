#pragma once
#include <cstdint>


namespace GoCL {

enum class TextureUsage : uint8_t { Diffuse, Normal, Roughness, UI, HDR, LUT };

struct ASTCParams {
    uint8_t blockW = 4, blockH = 4;

    constexpr float bpp() const noexcept {
        return 128.0f / (blockW * blockH);
    }
    constexpr uint32_t compressedSize(uint32_t w, uint32_t h) const noexcept {
        return ((w + blockW - 1) / blockW) *
               ((h + blockH - 1) / blockH) * 16u;
    }
};

class ASTCBlockSelector {
    // rawMB = (w * h * 4) / (1024 * 1024)
    // > 8 MB  →  w*h > 2,097,152
    // > 4 MB  →  w*h > 1,048,576
    static constexpr uint32_t kThreshLarge  = (8u * 1024u * 1024u) / 4u; // 2 097 152
    static constexpr uint32_t kThreshMedium = (4u * 1024u * 1024u) / 4u; // 1 048 576
    static constexpr uint32_t kVRAMTight    = 512u;

public:
    [[nodiscard]] static ASTCParams Select(
        TextureUsage usage,
        uint32_t     texWidth,
        uint32_t     texHeight,
        uint32_t     vramBudgetMB,
        bool         isAnimatedUV = false
    ) noexcept {
        // UI + LUT: always 4×4, no exceptions
        if (usage == TextureUsage::UI ||
            usage == TextureUsage::LUT) [[unlikely]]
            return {};

        if (isAnimatedUV) [[unlikely]]
            return {};

        const uint32_t pixels = texWidth * texHeight;

        if (usage == TextureUsage::Normal) [[unlikely]] {
            // was kThreshLarge — too conservative, misses medium-large normals
            const uint8_t blk = (pixels > kThreshMedium) ? 5u : 4u;
            return { blk, blk };
        }

        if (usage == TextureUsage::Roughness) [[unlikely]] {
            // was kThreshMedium — too aggressive, 1920×1080 hit 6×6 unexpectedly
            const uint8_t blk = (pixels > kThreshLarge || vramBudgetMB < kVRAMTight) ? 6u : 5u;
            return { blk, blk };
        }

        // HDR removed from early-out: VRAM pressure must still apply
        // Hot path: Diffuse + HDR
        const uint8_t blk = (pixels > kThreshLarge || vramBudgetMB < kVRAMTight) ? 6u
                        : (pixels > kThreshMedium)                              ? 5u
                        :                                                         4u;
        return { blk, blk };
    }
};

} // namespace GoCL