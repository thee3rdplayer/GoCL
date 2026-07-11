#pragma once

#include <cstdint>

namespace GoCL {

/**
 * @brief Thresholds that control the aggressiveness of ASTC block size selection.
 * 
 * Larger blocks → higher compression → lower quality but also lower VRAM usage.
 * These thresholds determine when to trade quality for memory savings.
 */
struct ASTCThresholds {
    /**
     * @brief Raw pixel count above which a texture is considered "large".
     * 
     * Equivalent to 8 MiB of raw RGBA8 data (8 * 1024 * 1024 / 4 bytes per pixel).
     * Textures >= this size will receive the largest block size (6x6) when VRAM is tight.
     */
    uint32_t largePixels = (8u * 1024u * 1024u) / 4u;  // ≈ 2,097,152 pixels

    /**
     * @brief Raw pixel count above which a texture is considered "medium".
     * 
     * Equivalent to 4 MiB of raw RGBA8 data.
     * Textures >= this size (but smaller than largePixels) will use medium block size (5x5).
     */
    uint32_t mediumPixels = (4u * 1024u * 1024u) / 4u;  // ≈ 1,048,576 pixels

    /**
     * @brief Remaining VRAM budget (in MB) below which the selector becomes aggressive.
     * 
     * When free VRAM drops below this threshold, larger ASTC blocks are preferred
     * even for textures that would normally use smaller blocks.
     */
    uint32_t tightVRAM = 512u;  // 512 MiB threshold
};

}  // namespace GoCL