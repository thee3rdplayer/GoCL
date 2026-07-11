// =============================================================================
// Texture loading and compression.
//
// LoadTexture() decides at runtime which compression path to take:
//   - HDR + no ASTC HDR support → transcode to ETC2.
//   - ASTC LDR supported (hardware decode or software decode) → compress to ASTC.
//   - Otherwise → transcode to ETC2.
//
// ASTC block size is chosen by ASTCBlockSelector, which uses VRAM budget,
// configurable thresholds, and hardware hints.
//
// The astcenc encoder context is cached per thread (thread‑local) to avoid
// repeated initialisation for textures with the same block size and HDR mode.
// =============================================================================

#include "TextureManager.h"
#include "core/CapabilityOracle.h"
#include "ASTCBlockSelector.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <astcenc.h>
#include <basisu_transcoder.h>
#include <basisu_comp.h>
#include <vulkan/vulkan.h>

#include <stdexcept>
#include <string>
#include <mutex>
#include <cstring>
#include <cstdio>

// ---------------------------------------------------------------------------
// Thread‑local ASTC encoder context cache.
//
// Because astcenc_context_alloc is expensive, we keep a cached encoder per
// thread for the most recent block size, HDR mode, and quality preset.
// When the parameters change, the old context is freed and a new one is created.
// ---------------------------------------------------------------------------
struct CachedAstcContext {
    astcenc_config   cfg;
    astcenc_context* ctx = nullptr;
    bool             hdr   = false;
    uint8_t          bw    = 0;
    uint8_t          bh    = 0;
    unsigned int     preset = ASTCENC_PRE_FAST;

    ~CachedAstcContext() {
        if (ctx) {
            astcenc_context_free(ctx);
        }
    }
};

static thread_local CachedAstcContext tls_astcCtx;

/**
 * @brief Returns a cached astcenc_context, reinitialising if the parameters have changed.
 */
static astcenc_context* GetCachedAstcContext(
    bool hdr, uint32_t bw, uint32_t bh, unsigned int preset) {
    // If the existing context matches the requested parameters, reuse it.
    if (tls_astcCtx.ctx &&
        tls_astcCtx.hdr    == hdr &&
        tls_astcCtx.bw     == bw &&
        tls_astcCtx.bh     == bh &&
        tls_astcCtx.preset == preset) {
        return tls_astcCtx.ctx;
    }

    // Parameters changed – free the old context.
    if (tls_astcCtx.ctx) {
        astcenc_context_free(tls_astcCtx.ctx);
        tls_astcCtx.ctx = nullptr;
    }

    // Initialise configuration.
    if (astcenc_config_init(
            hdr ? ASTCENC_PRF_HDR : ASTCENC_PRF_LDR,
            bw, bh, 1,
            static_cast<unsigned int>(preset),
            0,
            &tls_astcCtx.cfg) != ASTCENC_SUCCESS) {
        return nullptr;
    }

    // Allocate encoder context.
    if (astcenc_context_alloc(&tls_astcCtx.cfg, 1, &tls_astcCtx.ctx, 0) != ASTCENC_SUCCESS) {
        tls_astcCtx.ctx = nullptr;
        return nullptr;
    }

    // Store the new parameters.
    tls_astcCtx.hdr    = hdr;
    tls_astcCtx.bw     = static_cast<uint8_t>(bw);
    tls_astcCtx.bh     = static_cast<uint8_t>(bh);
    tls_astcCtx.preset = preset;

    return tls_astcCtx.ctx;
}

namespace GoCL {

// ---------------------------------------------------------------------------
// LoadTexture – loads an image from disk, compresses it, and uploads to VRAM.
// The decision tree uses device capabilities and VRAM pressure.
// ---------------------------------------------------------------------------
VkImage TextureManager::LoadTexture(
    const GoCLContext& ctx,
    const std::string& path,
    TextureUsage       usage,
    VkDeviceMemory*    outMemory) {
    int w, h, ch;
    using StbiPtr = std::unique_ptr<uint8_t, decltype(&stbi_image_free)>;
    StbiPtr raw(stbi_load(path.c_str(), &w, &h, &ch, 4), stbi_image_free);

    if (!raw) {
        const char* reason = stbi_failure_reason();
        throw std::runtime_error(
            std::string("Failed to load: ") + path + " — " +
            (reason ? reason : "unknown stb error"));
    }

    if (w <= 0 || h <= 0) {
        throw std::runtime_error("Invalid image dimensions");
    }

    const uint32_t vramRemaining = ctx.VRAMRemainingMB();   // free VRAM (or total if no budget ext)
    const uint32_t totalVRAM     = ctx.caps.vramBudgetMB;   // total VRAM (static)
    const bool     isHDR         = (usage == TextureUsage::Environment);

    // -----------------------------------------------------------------------
    // Path 1: HDR texture on a device that lacks VK_EXT_texture_compression_astc_hdr.
    // Fall back to ETC2 (HDR data is truncated to LDR; this is the only portable fallback).
    // -----------------------------------------------------------------------
    if (isHDR && !ctx.caps.astcHdrHW) {
        auto etc2 = TranscodeToETC2(raw.get(), w, h);
        return ctx.UploadTexture(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,
                                 w, h, etc2.data(), etc2.size(), outMemory);
    }

    // -----------------------------------------------------------------------
    // Path 2: ASTC supported (hardware decode or NVIDIA software decode).
    // Choose block size, compress to ASTC, and upload.
    // -----------------------------------------------------------------------
    if (ctx.caps.astcLdrHW ||
        (ctx.caps.vendorID == 0x10DE && !ctx.caps.nvidiaNoASTCDriver)) {
        ASTCParams blk = ASTCBlockSelector::Select(
            usage, w, h, vramRemaining,
            false,  // isAnimatedUV
            ctx.astcThresholds,
            ctx.caps.hardwareHint,
            totalVRAM);

        auto compressed = CompressToASTCEnc(raw.get(), w, h, blk, isHDR);
        VkFormat fmt = ASTCVkFormat(blk.blockWidth(), blk.blockHeight(), isHDR);

        TextureManager::LoadedTexture result{};
        result.image = ctx.UploadTexture(fmt, w, h,
                                         compressed.data(), compressed.size(),
                                         &result.memory);
        result.format = fmt;
        if (outMemory) {
            *outMemory = result.memory;
        }
        return result.image;
    }

    // -----------------------------------------------------------------------
    // Path 3: No ASTC support – fall back to universal ETC2.
    // -----------------------------------------------------------------------
    auto etc2 = TranscodeToETC2(raw.get(), w, h);
    VkFormat fmt = VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;

    TextureManager::LoadedTexture result{};
    result.image = ctx.UploadTexture(fmt, w, h,
                                     etc2.data(), etc2.size(),
                                     &result.memory);
    result.format = fmt;
    if (outMemory) {
        *outMemory = result.memory;
    }
    return result.image;
}

// ---------------------------------------------------------------------------
// ASTCVkFormat – converts block dimensions to the corresponding Vulkan format.
// The hdr flag selects SFLOAT (HDR) vs UNORM variants.
// ---------------------------------------------------------------------------
VkFormat TextureManager::ASTCVkFormat(uint8_t bw, uint8_t bh, bool hdr) {
    switch ((uint32_t(bw) << 8) | bh) {
        case (4  << 8) | 4:  return hdr ? VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK   : VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
        case (5  << 8) | 4:  return hdr ? VK_FORMAT_ASTC_5x4_SFLOAT_BLOCK   : VK_FORMAT_ASTC_5x4_UNORM_BLOCK;
        case (5  << 8) | 5:  return hdr ? VK_FORMAT_ASTC_5x5_SFLOAT_BLOCK   : VK_FORMAT_ASTC_5x5_UNORM_BLOCK;
        case (6  << 8) | 5:  return hdr ? VK_FORMAT_ASTC_6x5_SFLOAT_BLOCK   : VK_FORMAT_ASTC_6x5_UNORM_BLOCK;
        case (6  << 8) | 6:  return hdr ? VK_FORMAT_ASTC_6x6_SFLOAT_BLOCK   : VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
        case (8  << 8) | 5:  return hdr ? VK_FORMAT_ASTC_8x5_SFLOAT_BLOCK   : VK_FORMAT_ASTC_8x5_UNORM_BLOCK;
        case (8  << 8) | 6:  return hdr ? VK_FORMAT_ASTC_8x6_SFLOAT_BLOCK   : VK_FORMAT_ASTC_8x6_UNORM_BLOCK;
        case (8  << 8) | 8:  return hdr ? VK_FORMAT_ASTC_8x8_SFLOAT_BLOCK   : VK_FORMAT_ASTC_8x8_UNORM_BLOCK;
        case (10 << 8) | 5:  return hdr ? VK_FORMAT_ASTC_10x5_SFLOAT_BLOCK  : VK_FORMAT_ASTC_10x5_UNORM_BLOCK;
        case (10 << 8) | 6:  return hdr ? VK_FORMAT_ASTC_10x6_SFLOAT_BLOCK  : VK_FORMAT_ASTC_10x6_UNORM_BLOCK;
        case (10 << 8) | 8:  return hdr ? VK_FORMAT_ASTC_10x8_SFLOAT_BLOCK  : VK_FORMAT_ASTC_10x8_UNORM_BLOCK;
        case (10 << 8) | 10: return hdr ? VK_FORMAT_ASTC_10x10_SFLOAT_BLOCK : VK_FORMAT_ASTC_10x10_UNORM_BLOCK;
        case (12 << 8) | 10: return hdr ? VK_FORMAT_ASTC_12x10_SFLOAT_BLOCK : VK_FORMAT_ASTC_12x10_UNORM_BLOCK;
        case (12 << 8) | 12: return hdr ? VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK : VK_FORMAT_ASTC_12x12_UNORM_BLOCK;
        default:
            using namespace std::string_literals;
            throw std::runtime_error(
                "TextureManager: unsupported ASTC block size "s + std::to_string(bw) + "x" + std::to_string(bh));
    }
}

// ---------------------------------------------------------------------------
// CompressToASTCEnc – compresses raw RGBA data to ASTC using the astcenc library.
// Uses the thread‑local cached encoder context to minimise allocation overhead.
// ---------------------------------------------------------------------------
std::vector<uint8_t> TextureManager::CompressToASTCEnc(
    const uint8_t* raw, uint32_t w, uint32_t h,
    const ASTCParams& blk, bool hdr) {
    astcenc_context* ctx = GetCachedAstcContext(
        hdr, blk.blockWidth(), blk.blockHeight(), ASTCENC_PRE_FAST);

    if (!ctx) {
        throw std::runtime_error("TextureManager: astcenc_context_alloc failed");
    }

    astcenc_image img{};
    img.dim_x     = w;
    img.dim_y     = h;
    img.dim_z     = 1;
    img.data_type = ASTCENC_TYPE_U8;
    void* slices  = const_cast<uint8_t*>(raw);
    img.data      = &slices;

    const astcenc_swizzle swizzle{
        ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };

    const uint32_t outSize = static_cast<uint32_t>(blk.compressedSize(w, h));
    std::vector<uint8_t> out(outSize);

    if (astcenc_compress_image(ctx, &img, &swizzle, out.data(), outSize, 0)
            != ASTCENC_SUCCESS) {
        throw std::runtime_error("TextureManager: astcenc_compress_image failed");
    }

    return out;
}

// ---------------------------------------------------------------------------
// TranscodeToETC2 – converts raw RGBA to ETC2 compressed data.
// This is the heavy path (UASTC → ETC2) used by the proxy and by the engine
// fallback when ASTC is unavailable.
// ---------------------------------------------------------------------------
std::vector<uint8_t> TextureManager::TranscodeToETC2(
    const uint8_t* raw, uint32_t w, uint32_t h) {
    static std::once_flag sInit;
    std::call_once(sInit, []{ basist::basisu_transcoder_init(); });

    basisu::image srcImg(w, h);
    const size_t rowBytes = w * 4;

    for (uint32_t y = 0; y < h; ++y) {
        std::memcpy(srcImg.get_ptr() + y * rowBytes, raw + y * rowBytes, rowBytes);
    }

    static thread_local basisu::job_pool jpool(1);

    basisu::basis_compressor_params params{};
    params.m_source_images.push_back(srcImg);
    params.m_uastc          = true;
    params.m_multithreading = false;
    params.m_pJob_pool      = &jpool;

    basisu::basis_compressor comp;
    if (!comp.init(params)) {
        throw std::runtime_error("TextureManager: basis_compressor init failed");
    }

    if (comp.process() != basisu::basis_compressor::cECSuccess) {
        throw std::runtime_error("TextureManager: basis_compressor process failed");
    }

    const auto& basisData = comp.get_output_basis_file();

    basist::basisu_transcoder transcoder;
    if (!transcoder.start_transcoding(basisData.data(), static_cast<uint32_t>(basisData.size()))) {
        throw std::runtime_error("TextureManager: transcoder start failed");
    }

    basist::basisu_image_level_info levelInfo{};
    if (!transcoder.get_image_level_info(
            basisData.data(),
            static_cast<uint32_t>(basisData.size()),
            levelInfo, 0, 0)) {
        throw std::runtime_error("TextureManager: get_image_level_info failed");
    }

    const uint32_t totalBlocks = levelInfo.m_total_blocks;

    if (totalBlocks > UINT32_MAX / 16) {
        throw std::overflow_error("TextureManager: ETC2 output overflow");
    }

    const uint32_t outSize = totalBlocks * 16;
    std::vector<uint8_t> out(outSize);

    if (!transcoder.transcode_image_level(
            basisData.data(), static_cast<uint32_t>(basisData.size()),
            0, 0, out.data(), totalBlocks,
            basist::transcoder_texture_format::cTFETC2_RGBA)) {
        throw std::runtime_error("TextureManager: ETC2 transcode failed");
    }

    return out;
}

}   // namespace GoCL