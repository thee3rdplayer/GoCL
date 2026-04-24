#include "../render/TextureManager.h"
#include "../core/CapabilityOracle.h"
#include "../render/ASTCBlockSelector.h"

#define STB_IMAGE_IMPLEMENTATION

#include <stb_image.h>
#include <astcenc.h>
#include <basisu_transcoder.h>
#include <basisu_comp.h>          // basis_compressor_params / basis_compressor
#include <vulkan/vulkan.h>
#include <stdexcept>
#include <string> 


namespace GoCL {

VkImage TextureManager::LoadTexture(
    const GoCLContext& ctx,
    const std::string& path,
    TextureUsage       usage)
{
    int w, h, ch;
    uint8_t* raw = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!raw) throw std::runtime_error("Failed to load: " + path);

    VkImage result = VK_NULL_HANDLE;
    const uint32_t vramMB = ctx.VRAMRemainingMB();

    if (ctx.caps.astcLdrHW) {
        // HW path: Mali, Adreno, Intel Gen9–Gen12
        ASTCParams blk  = ASTCBlockSelector::Select(usage, w, h, vramMB);
        auto compressed = CompressToASTCEnc(raw, w, h, blk);
        VkFormat fmt    = ASTCVkFormat(blk.blockW, blk.blockH);
        result = ctx.UploadTexture(fmt, w, h, compressed.data(), compressed.size());

    } else if (ctx.caps.vendorID == 0x10DE && !ctx.caps.nvidiaNoASTCDriver) {
        // NVIDIA pre-545.84: driver SW-decodes ASTC — skip Basis transcode,
        // upload ASTC directly and let the driver handle it
        ASTCParams blk  = ASTCBlockSelector::Select(usage, w, h, vramMB);
        auto compressed = CompressToASTCEnc(raw, w, h, blk);
        VkFormat fmt    = ASTCVkFormat(blk.blockW, blk.blockH);
        result = ctx.UploadTexture(fmt, w, h, compressed.data(), compressed.size());

    } else {
        // ETC2 fallback: NVIDIA ≥545.84, AMD, Intel Arc, software renderers
        auto etc2    = TranscodeToETC2(raw, w, h);
        VkFormat fmt = (ch == 4)
            ? VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK
            : VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
        result = ctx.UploadTexture(fmt, w, h, etc2.data(), etc2.size());
    }

    stbi_image_free(raw);
    return result;
}

VkFormat TextureManager::ASTCVkFormat(uint8_t bw, uint8_t bh)
{
    // Encode block dims as a single key: (bw << 8) | bh
    switch ((uint32_t(bw) << 8) | bh) {
        case (4  << 8) | 4:  return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
        case (5  << 8) | 4:  return VK_FORMAT_ASTC_5x4_UNORM_BLOCK;
        case (5  << 8) | 5:  return VK_FORMAT_ASTC_5x5_UNORM_BLOCK;
        case (6  << 8) | 5:  return VK_FORMAT_ASTC_6x5_UNORM_BLOCK;
        case (6  << 8) | 6:  return VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
        case (8  << 8) | 5:  return VK_FORMAT_ASTC_8x5_UNORM_BLOCK;
        case (8  << 8) | 6:  return VK_FORMAT_ASTC_8x6_UNORM_BLOCK;
        case (8  << 8) | 8:  return VK_FORMAT_ASTC_8x8_UNORM_BLOCK;
        case (10 << 8) | 5:  return VK_FORMAT_ASTC_10x5_UNORM_BLOCK;
        case (10 << 8) | 6:  return VK_FORMAT_ASTC_10x6_UNORM_BLOCK;
        case (10 << 8) | 8:  return VK_FORMAT_ASTC_10x8_UNORM_BLOCK;
        case (10 << 8) | 10: return VK_FORMAT_ASTC_10x10_UNORM_BLOCK;
        case (12 << 8) | 10: return VK_FORMAT_ASTC_12x10_UNORM_BLOCK;
        case (12 << 8) | 12: return VK_FORMAT_ASTC_12x12_UNORM_BLOCK;
        default:
            throw std::runtime_error(
                "TextureManager: unsupported ASTC block size " +
                std::to_string(bw) + "x" + std::to_string(bh));
    }
}

// ---------------------------------------------------------------------------
// CompressToASTCEnc
// ---------------------------------------------------------------------------
std::vector<uint8_t> TextureManager::CompressToASTCEnc(
    const uint8_t* raw, uint32_t w, uint32_t h, const ASTCParams& blk)
{
    astcenc_config cfg{};
    astcenc_config_init(
        ASTCENC_PRF_LDR,
        blk.blockW, blk.blockH, 1,
        ASTCENC_PRE_FAST, 0, &cfg);

    astcenc_context* ctx = nullptr;
    if (astcenc_context_alloc(&cfg, 1, &ctx, 0) != ASTCENC_SUCCESS)
        throw std::runtime_error("TextureManager: astcenc_context_alloc failed");

    astcenc_image img{};
    img.dim_x     = w;
    img.dim_y     = h;
    img.dim_z     = 1;
    img.data_type = ASTCENC_TYPE_U8;
    void* slices  = const_cast<uint8_t*>(raw);
    img.data      = &slices;

    const astcenc_swizzle swizzle{
        ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };

    const uint32_t outSize = blk.compressedSize(w, h);
    std::vector<uint8_t> out(outSize);

    if (astcenc_compress_image(ctx, &img, &swizzle, out.data(), outSize, 0)
            != ASTCENC_SUCCESS) {
        astcenc_context_free(ctx);
        throw std::runtime_error("TextureManager: astcenc_compress_image failed");
    }

    astcenc_context_free(ctx);
    return out;
}

// ---------------------------------------------------------------------------
// TranscodeToETC2
// ---------------------------------------------------------------------------
std::vector<uint8_t> TextureManager::TranscodeToETC2(
    const uint8_t* raw, uint32_t w, uint32_t h)
{
    using namespace basist;

    basisu_transcoder_init();

    basisu::image srcImg(w, h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = raw + (y * w + x) * 4;
            srcImg(x, y) = basisu::color_rgba(p[0], p[1], p[2], p[3]);
        }

    basisu::basis_compressor_params params{};
    params.m_source_images.push_back(srcImg);
    params.m_uastc          = true;
    params.m_multithreading = false;

    basisu::job_pool jpool(1);
    params.m_pJob_pool = &jpool;

    basisu::basis_compressor comp;
    if (!comp.init(params))
        throw std::runtime_error("TextureManager: basis_compressor init failed");
    if (comp.process() != basisu::basis_compressor::cECSuccess)
        throw std::runtime_error("TextureManager: basis_compressor process failed");

    const auto& basisData = comp.get_output_basis_file();

    basisu_transcoder transcoder;
    if (!transcoder.start_transcoding(basisData.data(), (uint32_t)basisData.size()))
        throw std::runtime_error("TextureManager: transcoder start failed");

    uint32_t origW, origH, totalBlocks;
    transcoder.get_image_level_desc(
        basisData.data(), (uint32_t)basisData.size(),
        0, 0, origW, origH, totalBlocks);

    const uint32_t outSize = totalBlocks * 16;
    std::vector<uint8_t> out(outSize);

    if (!transcoder.transcode_image_level(
            basisData.data(), (uint32_t)basisData.size(),
            0, 0, out.data(), totalBlocks,
            transcoder_texture_format::cTFETC2_RGBA))
        throw std::runtime_error("TextureManager: ETC2 transcode failed");

    return out;
}

} // namespace GoCL