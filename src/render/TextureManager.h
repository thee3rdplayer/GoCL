#pragma once

#include "../core/GoCLContext.h"
#include "ASTCBlockSelector.h"

#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <cstdint>


namespace GoCL {

class TextureManager {
public:
    static VkImage LoadTexture(
        const GoCLContext& ctx,
        const std::string& path,
        TextureUsage       usage);

    static VkFormat ASTCVkFormat(uint8_t bw, uint8_t bh, bool hdr = false);

private:
    static std::vector<uint8_t> CompressToASTCEnc(
        const uint8_t* raw, uint32_t w, uint32_t h,
        const ASTCParams& blk, bool hdr = false);

    static std::vector<uint8_t> TranscodeToETC2(
        const uint8_t* raw, uint32_t w, uint32_t h);
};

} // namespace GoCL