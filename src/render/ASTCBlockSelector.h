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
        // UI/HDR: quality-sensitive, block artifacts unacceptable — always 4×4.
        // HDR note: caller must still verify VK_EXT_texture_compression_astc_hdr;
        // this selector only governs block size, not format class.
        if (usage == TextureUsage::UI  ||
            usage == TextureUsage::HDR ||
            usage == TextureUsage::LUT) [[unlikely]]
            return {};

        // Animated UV: cache coherence dominates compression ratio — always 4×4.
        if (isAnimatedUV) [[unlikely]]
            return {};

        const uint32_t pixels = texWidth * texHeight;

        // Normal maps: preserve high-frequency detail; block size by pixel count,
        // not width alone (handles non-square atlases correctly).
        if (usage == TextureUsage::Normal) [[unlikely]] {
            const uint8_t blk = (pixels > kThreshLarge) ? 5u : 4u;
            return { blk, blk };
        }

        // Roughness: single-channel perceptual data tolerates more compression.
        // Skips one tier vs Diffuse at medium sizes; stays within the 6×6 cap
        // validated safe by the 2k/8×8 sampler-cache research.
        if (usage == TextureUsage::Roughness) [[unlikely]] {
            const uint8_t blk = (pixels > kThreshMedium || vramBudgetMB < kVRAMTight) ? 6u : 5u;
            return { blk, blk };
        }

        // Hot path: Diffuse
        const uint8_t blk = (pixels > kThreshLarge || vramBudgetMB < kVRAMTight) ? 6u
                          : (pixels > kThreshMedium)                              ? 5u
                          :                                                         4u;
        return { blk, blk };
    }
};

} // namespace GoCL