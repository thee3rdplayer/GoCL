// =============================================================================
// ETC2 texture compression for GoCL
//
// Two entry points:
//   1. TranscodeToETC2(raw, w, h)         – raw RGBA → UASTC → ETC2
//      (used by the proxy's ASTC‑to‑ETC2 fallback; heavy, off‑the‑render‑loop)
//   2. TranscodeBasisToETC2(data, size)   – pre‑encoded .basis → ETC2
//      (engine path; no compression, fast)
//
// Thread‑local transcoder instances guarantee safe concurrent use.
// C‑linkage wrappers return GPU‑friendly aligned memory; free with GoCL_Free().
// =============================================================================

#include "TextureManager.h"
#include <basisu_transcoder.h>
#include <basisu_comp.h>

#include <mutex>
#include <vector>
#include <cstdlib> // for std::aligned_alloc or free
#if defined(_WIN32) || defined(__MINGW32__)
#include <malloc.h> // for _aligned_malloc and _aligned_free
#endif 
#include <cstring>
#include <cstdio>

// ---------------------------------------------------------------------------
// Each thread owns a transcoder to avoid shared mutable state.
// ---------------------------------------------------------------------------
namespace {

// Safety cap – reject texture sizes that cannot be real (512 MB of ETC2 data)
// This prevents excessive memory allocation due to malformed input.
static constexpr size_t kMaxETC2Bytes = 512u * 1024u * 1024u;

}   // anonymous namespace

// ---------------------------------------------------------------------------
// Path 1: raw RGBA → UASTC → ETC2 (proxy ASTC fallback)
// ---------------------------------------------------------------------------
std::vector<uint8_t> GoCL::TextureManager::TranscodeToETC2(
    const uint8_t* raw, uint32_t w, uint32_t h) {
    // Validate input.
    if (!raw) {
        throw std::invalid_argument("raw == nullptr");
    }

    if (!w || !h) {
        throw std::invalid_argument("Invalid texture dimensions");
    }

    const size_t rgbaBytes = static_cast<size_t>(w) * h * 4;
    if (rgbaBytes > kMaxETC2Bytes) {
        throw std::runtime_error("Texture exceeds safety limit");
    }

    // Create a Basis image from the raw RGBA data (copies the memory).
    basisu::image srcImg(w, h);
    std::memcpy(srcImg.get_ptr(), raw, rgbaBytes);

    // Encode RGBA to a temporary .basis container using UASTC.
    // The result is immediately transcoded to ETC2.
    basisu::basis_compressor_params params{};
    params.m_source_images.emplace_back(std::move(srcImg));
    params.m_uastc = true;
    params.m_multithreading = false;    // single‑threaded, simpler

    basisu::basis_compressor comp;
    if (!comp.init(params) || comp.process() != basisu::basis_compressor::cECSuccess) {
        throw std::runtime_error("TextureManager: UASTC compression failed");
    }

    const auto& basisData = comp.get_output_basis_file();
    if (basisData.size() > UINT32_MAX) {
        throw std::overflow_error("Basis file exceeds uint32_t range");
    }

    // Transcode the in‑memory .basis file to ETC2.
    return TranscodeBasisToETC2(
        basisData.data(),
        static_cast<uint32_t>(basisData.size()));
}

// ---------------------------------------------------------------------------
// Path 2: pre‑encoded .basis → ETC2 (fast)
// ---------------------------------------------------------------------------
std::vector<uint8_t> GoCL::TextureManager::TranscodeBasisToETC2(
    const uint8_t* basisData, uint32_t basisSize) {
    // One‑time initialisation
    static std::once_flag initFlag;
    std::call_once(initFlag, []{ basist::basisu_transcoder_init(); });

    basist::basisu_transcoder transcoder;   // local, destroyed on return

    // Basic validation.
    if (!basisData || !basisSize) {
        throw std::invalid_argument("Invalid basis input");
    }

    if (!transcoder.validate_header(basisData, basisSize)) {
        throw std::runtime_error("Invalid basis header");
    }

    if (!transcoder.start_transcoding(basisData, basisSize)) {
        throw std::runtime_error("TranscodeBasis: start_transcoding failed");
    }

    // Query layer 0, level 0 to get the number of blocks.
    basist::basisu_image_level_info levelInfo{};
    if (!transcoder.get_image_level_info(basisData, basisSize, levelInfo, 0, 0)) {
        throw std::runtime_error("TranscodeBasis: get_image_level_info failed");
    }

    const uint32_t totalBlocks = levelInfo.m_total_blocks;
    if (totalBlocks == 0) {
        throw std::runtime_error("TranscodeBasis: zero blocks");
    }

    // Bytes per block for ETC2 RGBA (16 bytes per 4x4 block).
    const uint32_t bytesPerBlock = basist::basis_get_bytes_per_block_or_pixel(
        basist::transcoder_texture_format::cTFETC2_RGBA);

    if (totalBlocks > SIZE_MAX / bytesPerBlock) {
        throw std::overflow_error("Output size overflow");
    }

    const size_t outSize = static_cast<size_t>(totalBlocks) * bytesPerBlock;
    if (outSize > kMaxETC2Bytes) {
        throw std::runtime_error("TranscodeBasis: output size exceeds safety limit");
    }

    std::vector<uint8_t> out(outSize);

    // Perform the actual transcoding to ETC2.
    if (!transcoder.transcode_image_level(
            basisData, basisSize,
            0, 0,
            out.data(), totalBlocks,
            basist::transcoder_texture_format::cTFETC2_RGBA)) {
        throw std::runtime_error(
            "ETC2 transcode failed "
            "(image=0 level=0 size=" +
            std::to_string(basisSize) +
            " blocks=" +
            std::to_string(totalBlocks) + ")");
    }

    return out;
}

// ---------------------------------------------------------------------------
// C‑linkage wrappers (proxy dlopen / LoadLibrary)
// Returned memory is at least 16-byte aligned.
// Must be released with GoCL_Free().
// ---------------------------------------------------------------------------

// Copies the content of a vector into newly allocated 16‑byte aligned memory.
// Returns 1 on success, 0 on failure. Used by both C exports.
static int CopyResult(
    const std::vector<uint8_t>& result,
    uint8_t** outData,
    size_t* outSize) {
    if (result.empty()) {
        return 0;
    }

    const size_t allocSize =
        (result.size() + 15u) & ~size_t(15u);   // round up to multiple of 16

#if defined(_WIN32)
    *outData = static_cast<uint8_t*>(_aligned_malloc(allocSize, 16));
#else
    *outData = static_cast<uint8_t*>(std::aligned_alloc(16, allocSize));
#endif 

    if (!*outData) {
        return 0;
    }

    *outSize = result.size();
    std::memcpy(*outData, result.data(), result.size());
    return 1;
}

/**
 * @brief Transcodes raw RGBA to ETC2 and returns a 16‑byte aligned pointer.
 * 
 * Intended for the proxy’s lazy‑loaded transcoder library.
 */
extern "C" int GoCL_TranscodeToETC2(
    const uint8_t* raw, uint32_t w, uint32_t h,
    uint8_t** outData, size_t* outSize) {
    if (outData) *outData = nullptr;
    if (outSize) *outSize = 0;

    if (!raw || !outData || !outSize) {
        return 0;
    }

    try {
        auto result = GoCL::TextureManager::TranscodeToETC2(raw, w, h);
        return CopyResult(result, outData, outSize);
    } catch (...) {
        if (outData) *outData = nullptr;
        if (outSize) *outSize = 0;
        return 0;
    }
}

/**
 * @brief Transcodes a pre‑compressed .basis file to ETC2.
 * 
 * imageIndex and levelIndex are ignored in this version (always 0).
 */
extern "C" int GoCL_TranscodeBasisToETC2(
    const uint8_t* basisData, uint32_t basisSize,
    uint8_t** outData, size_t* outSize,
    uint32_t imageIndex, uint32_t levelIndex) {
    (void)imageIndex;   // unused
    (void)levelIndex;   // unused

    if (!basisData || !outData || !outSize) {
        return 0;
    }

    try {
        auto result = GoCL::TextureManager::TranscodeBasisToETC2(basisData, basisSize);
        if (result.empty()) {
            return 0;
        }

        *outSize = result.size();
        #if defined(_WIN32)
            *outData = static_cast<uint8_t*>(_aligned_malloc(result.size(), 16));
        #else
            *outData = static_cast<uint8_t*>(std::aligned_alloc(16, result.size()));
        #endif
            if (!*outData) {
                return 0;
            } 

        std::memcpy(*outData, result.data(), result.size());
        return 1;
    } catch (...) {
        return 0;
    }
}

/**
 * @brief Frees memory allocated by the C transcoder functions.
 */

extern "C" void GoCL_Free(void* ptr) {
#if defined(_WIN32) || defined(__MINGW32__)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
} 