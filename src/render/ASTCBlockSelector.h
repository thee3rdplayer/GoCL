#pragma once

#include "core/ASTCThresholds.h"
#include "core/HardwareHint.h"

#include <cstdint>

// =============================================================================
// Selects an ASTC block size for a texture.
//
// Balances image quality against memory footprint and bandwidth usage using
// heuristics based on:
//
//   - Texture class (UI/LUT, normal map, albedo, material masks, etc.)
//   - Texture dimensions
//   - Configurable size thresholds
//   - Available VRAM budget
//   - Hardware characteristics supplied via HardwareHint
//
// UI and LUT textures are biased toward ASTC 4×4 because they are highly
// sensitive to compression artifacts. Less artifact-sensitive textures may
// use larger block sizes to reduce memory usage and bandwidth.
//
// This class performs no Vulkan operations and has no external dependencies.
// All decisions are derived solely from function parameters.
// =============================================================================

namespace GoCL {

/**
 * @brief TextureUsage tags help the selector bias toward quality or compression.
 * 
 * UI and LUT textures are forced to 4×4 blocks to minimise compression artifacts.
 * Normal maps and roughness maps have dedicated quality thresholds.
 * HDR textures are treated like diffuses but require HDR‑compatible formats.
 * Additional tags (Metallic, AmbientOcclusion, Height, Emissive, Environment)
 * are currently mapped to the same logic as Diffuse, but kept separate for
 * future fine‑tuning.
 */
enum class TextureUsage : uint8_t {
    Diffuse,
    Normal,
    Roughness,
    UI,
    LUT,
    Metallic,
    AmbientOcclusion,
    Height,
    Emissive,
    Environment
};

enum class TextureRange : uint8_t {
    LDR,
    HDR
};

/**
 * @brief All ASTC block dimensions supported by the Vulkan standard.
 * 
 * The block size determines the compression ratio and quality.
 * Larger blocks = higher compression, lower quality, lower memory usage.
 */
enum class ASTCBlock : uint8_t {
    B4x4,
    B5x4,
    B5x5,
    B6x5,
    B6x6,
    B8x5,
    B8x6,
    B8x8,
    B10x5,
    B10x6,
    B10x8,
    B10x10,
    B12x10,
    B12x12
};

/**
 * @brief ASTCParams holds the chosen block dimensions.
 * 
 * bpp() and compressedSize() are constexpr helpers useful for
 * buffer allocation and memory‑budget calculations at compile time.
 */
struct ASTCParams {
    ASTCBlock block = ASTCBlock::B4x4;

    /**
     * @brief Returns the block width in texels.
     */
    constexpr uint8_t blockWidth() const noexcept {
        switch (block) {
        case ASTCBlock::B4x4:   return 4;
        case ASTCBlock::B5x4:   return 5;
        case ASTCBlock::B5x5:   return 5;
        case ASTCBlock::B6x5:   return 6;
        case ASTCBlock::B6x6:   return 6;
        case ASTCBlock::B8x5:   return 8;
        case ASTCBlock::B8x6:   return 8;
        case ASTCBlock::B8x8:   return 8;
        case ASTCBlock::B10x5:  return 10;
        case ASTCBlock::B10x6:  return 10;
        case ASTCBlock::B10x8:  return 10;
        case ASTCBlock::B10x10: return 10;
        case ASTCBlock::B12x10: return 12;
        case ASTCBlock::B12x12: return 12;
        }
        return 4;
    }

    /**
     * @brief Returns the block height in texels.
     */
    constexpr uint8_t blockHeight() const noexcept {
        switch (block) {
        case ASTCBlock::B4x4:   return 4;
        case ASTCBlock::B5x4:   return 4;
        case ASTCBlock::B5x5:   return 5;
        case ASTCBlock::B6x5:   return 5;
        case ASTCBlock::B6x6:   return 6;
        case ASTCBlock::B8x5:   return 5;
        case ASTCBlock::B8x6:   return 6;
        case ASTCBlock::B8x8:   return 8;
        case ASTCBlock::B10x5:  return 5;
        case ASTCBlock::B10x6:  return 6;
        case ASTCBlock::B10x8:  return 8;
        case ASTCBlock::B10x10: return 10;
        case ASTCBlock::B12x10: return 10;
        case ASTCBlock::B12x12: return 12;
        }
        return 4;
    }

    /**
     * @brief Bits per pixel – a standard metric for texture compression efficiency.
     */
    constexpr float bpp() const noexcept {
        return 128.0f / (blockWidth() * blockHeight());
    }

    /**
     * @brief Returns the exact compressed size in bytes for a given texture dimension.
     * 
     * Rounds up to whole blocks.
     */
    constexpr uint64_t compressedSize(uint32_t w, uint32_t h) const noexcept {
        const auto bw = blockWidth();
        const auto bh = blockHeight();
        return static_cast<uint64_t>((w + bw - 1) / bw) *
               static_cast<uint64_t>((h + bh - 1) / bh) *
               16ull;
    }
};

/**
 * @brief Stateless, header‑only ASTC block size selector.
 * 
 * Call only when the device supports ASTC LDR (or software decode).
 * Capability checks are the caller’s responsibility (see TextureManager
 * and the proxy layer).
 */
class ASTCBlockSelector {
public:
    /**
     * @brief Select an ASTC block size based on texture usage, dimensions, VRAM budget,
     *        animation flag, configurable thresholds, hardware hints, and total VRAM.
     * 
     * The decision is deterministic.
     * 
     * @param usage Texture usage category.
     * @param texWidth Texture width in pixels.
     * @param texHeight Texture height in pixels.
     * @param vramBudgetMB Available VRAM budget in MiB.
     * @param isAnimatedUV True if texture has animated UV coordinates.
     * @param thresholds ASTC threshold configuration.
     * @param hint Hardware hints for performance tuning.
     * @param totalVRAM_MB Total device VRAM in MiB.
     * @return ASTCParams Selected block parameters.
     */
    [[nodiscard]] static ASTCParams Select(
        TextureUsage            usage,
        uint32_t                texWidth,
        uint32_t                texHeight,
        uint32_t                vramBudgetMB,
        bool                    isAnimatedUV  = false,
        const ASTCThresholds&   thresholds    = ASTCThresholds{},
        const HardwareHint&     hint          = HardwareHint{},
        uint32_t                totalVRAM_MB  = 0
    ) noexcept {
        // Zero‑dimension textures are invalid – return the smallest block.
        if (texWidth == 0 || texHeight == 0) {
            return { ASTCBlock::B4x4 };
        }

        // UI and LUT textures: artefact‑sensitive, always use highest quality (4×4).
        if (usage == TextureUsage::UI || usage == TextureUsage::LUT) [[unlikely]] {
            return { ASTCBlock::B4x4 };
        }

        // Animated UVs (e.g., scrolling textures) show visible temporal flickering
        // when compressed aggressively – stay at 4×4.
        if (isAnimatedUV) [[unlikely]] {
            return { ASTCBlock::B4x4 };
        }

        // ---- VRAM budget scaling: when remaining VRAM is very low,
        //      increase the effective "tightVRAM" threshold to force
        //      stronger compression. ----
        uint32_t effectiveTight  = thresholds.tightVRAM;
        uint32_t effectiveLarge  = thresholds.largePixels;
        uint32_t effectiveMedium = thresholds.mediumPixels;
        if (totalVRAM_MB > 0) {
            const float budgetRatio = static_cast<float>(vramBudgetMB) /
                                    static_cast<float>(totalVRAM_MB);
            constexpr float kLowBudgetRatio = 0.25f;  // only 25% of VRAM left
            if (budgetRatio < kLowBudgetRatio) {
                effectiveTight = thresholds.tightVRAM * 2u;  // double the pressure
            }
        }

        // ---- Hardware‑specific adjustments: low‑bandwidth and mobile GPUs
        //      benefit from more aggressive compression. ----
        if (hint.isLowBandwidth) {
            // Increase VRAM sensitivity and lower size thresholds.
            effectiveTight  = (effectiveTight  * 3u) / 2u; // +50%
            effectiveLarge  = (effectiveLarge  * 2u) / 3u; // –33%
            effectiveMedium = (effectiveMedium * 2u) / 3u;
        } else if (hint.isMobileTier) {
            effectiveTight  = (effectiveTight  * 5u) / 4u; // +25%
            effectiveLarge  = (effectiveLarge  * 3u) / 4u; // –25%
            // medium threshold unchanged for mobile tier
        }

        const uint64_t pixels =
            static_cast<uint64_t>(texWidth) *
            static_cast<uint64_t>(texHeight);

        // Normal maps: use 5×5 above the medium threshold.
        // Too aggressive compression causes visible banding on smooth surfaces.
        if (usage == TextureUsage::Normal) [[unlikely]] {
            return {
                (pixels > thresholds.mediumPixels)
                    ? ASTCBlock::B5x5
                    : ASTCBlock::B4x4
            };
        }

        // Roughness maps: more forgiving of compression artefacts.
        // Use 6×6 above the large threshold or when VRAM is tight.
        if (usage == TextureUsage::Roughness) [[unlikely]] {
            return {
                (pixels > effectiveLarge || vramBudgetMB < effectiveTight)
                    ? ASTCBlock::B6x6
                    : ASTCBlock::B5x5
            };
        }

        // Hot path: Diffuse, Metallic, AmbientOcclusion, Height, Emissive,
        // Environment, and HDR textures (all treated similarly).
        // Three‑tier decision: large → 6×6, medium → 5×5, small → 4×4.
        // isLowBandwidth forces 6×6 only above a floor size (64×64) because
        // tiny textures provide negligible bandwidth savings.
        static constexpr uint32_t kLowBandwidthFloorPixels = 64u * 64u;
        return {
            ((hint.isLowBandwidth && pixels >= kLowBandwidthFloorPixels) ||
             pixels > effectiveLarge ||
             vramBudgetMB < effectiveTight)
                ? ASTCBlock::B6x6
            : (pixels > effectiveMedium)
                ? ASTCBlock::B5x5
                : ASTCBlock::B4x4
        };
    }
};

}   // namespace GoCL