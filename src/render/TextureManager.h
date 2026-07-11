#pragma once

#include "core/GoCLContext.h"
#include "ASTCBlockSelector.h"
#include <vulkan/vulkan.h>

#include <string>
#include <vector>
#include <cstdint>

// =============================================================================
// Texture loading and compression pipeline.
//
// Each LoadTexture() call runs a decision tree:
//   1. HDR + no ASTC HDR support -> fallback path.
//      HDR data may be reduced to LDR when transcoded to ETC2.
//   2. ASTC LDR supported by device/runtime policy.
//   3. Otherwise → transcode to ETC2 (universal Vulkan fallback)
//
// ASTC block size is chosen by ASTCBlockSelector, which uses VRAM budget,
// configurable thresholds, and hardware hints to balance quality vs memory.
// =============================================================================

namespace GoCL {

/**
 * @brief Texture loading and compression pipeline.
 * 
 * Handles loading images from disk, compressing them to GPU-friendly formats
 * (ASTC or ETC2), and uploading to device memory.
 */
class TextureManager {
public:
    /**
     * @brief Holds the result of a successful texture load.
     * 
     * The caller owns both the VkImage and the VkDeviceMemory and must destroy
     * them together using GoCLContext::DestroyTexture().
     */
    struct LoadedTexture {
        VkImage image = VK_NULL_HANDLE;         ///< GPU‑side texture handle
        VkDeviceMemory memory = VK_NULL_HANDLE; ///< Device memory bound to the image
        VkFormat format = VK_FORMAT_UNDEFINED;  ///< Actual Vulkan format used (ASTC or ETC2)
    };

    /**
     * @brief Loads an image from disk, compresses it to a GPU‑friendly format,
     *        and uploads it to device memory.
     *
     * @param ctx Engine context (provides device, capabilities, staging buffer).
     * @param path File system path to the image (supports PNG, JPEG, etc.).
     * @param usage Texture usage hint (affects block size selection).
     * @param outMemory If not nullptr, receives the allocated VkDeviceMemory.
     * @return VkImage handle. The caller must later call ctx.DestroyTexture()
     *         with the returned image and the memory from outMemory (if provided).
     * @throws std::runtime_error on I/O or compression failure.
     */
    static VkImage LoadTexture(
        const GoCLContext& ctx,
        const std::string& path,
        TextureUsage       usage,
        VkDeviceMemory*    outMemory);

    /**
     * @brief Maps ASTC block dimensions (width, height) to the corresponding Vulkan format enum.
     * 
     * @param bw Block width in texels.
     * @param bh Block height in texels.
     * @param hdr If true, selects SFLOAT (HDR) variants (requires VK_EXT_texture_compression_astc_hdr).
     *            If false, selects UNORM variants (core ASTC LDR).
     * @return VkFormat corresponding to the block dimensions.
     * @throws std::runtime_error for unsupported block sizes.
     */
    static VkFormat ASTCVkFormat(uint8_t bw, uint8_t bh, bool hdr = false);

    /**
     * @brief Converts raw RGBA pixels (8‑bits per channel) to ETC2 compressed data.
     * 
     * Used by the Vulkan proxy for ASTC→ETC2 fallback at runtime, and also
     * internally by LoadTexture when ASTC hardware decode is unavailable.
     * 
     * @param raw Pointer to raw RGBA pixel data.
     * @param w Image width in pixels.
     * @param h Image height in pixels.
     * @return Byte vector containing ETC2‑compressed data (16 bytes per 4x4 block).
     */
    static std::vector<uint8_t> TranscodeToETC2(
        const uint8_t* raw,
        uint32_t w,
        uint32_t h);

    /**
     * @brief Converts a pre‑compressed .basis file (UASTC) directly to ETC2.
     * 
     * This is the fast path for assets that were offline‑compressed to Basis
     * format. Used by the engine (static library) but not by the proxy.
     * 
     * @param basisData Pointer to the .basis file data.
     * @param basisSize Size of the .basis file data in bytes.
     * @return Byte vector containing ETC2‑compressed data.
     */
    static std::vector<uint8_t> TranscodeBasisToETC2(
        const uint8_t* basisData,
        uint32_t basisSize);

private:
    /**
     * @brief Internal helper: compresses raw RGBA to ASTC using the astcenc library.
     * 
     * Uses a thread‑local cached encoder context to avoid repeated allocations.
     * 
     * @param raw Pointer to raw RGBA pixel data.
     * @param w Image width in pixels.
     * @param h Image height in pixels.
     * @param blk ASTC block parameters (block size).
     * @param hdr If true, uses HDR encoding (requires ASTC HDR support).
     * @return Byte vector containing ASTC‑compressed data.
     */
    static std::vector<uint8_t> CompressToASTCEnc(
        const uint8_t* raw, uint32_t w, uint32_t h,
        const ASTCParams& blk, bool hdr = false);
};

}   // namespace GoCL