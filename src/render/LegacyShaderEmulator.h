#pragma once

#include "../core/CapabilityOracle.h"

#include <vector>
#include <cstdint> 


namespace GoCL {

class LegacyShaderEmulator {
public:
    struct TransformOptions {
        bool     stripFP16;
        bool     stripSER;
        bool     emulate8BitStorage;
        bool     strip16BitStorage;
        bool     stripDescriptorIndexing;
        uint32_t maxDescriptorSetSize;
    };

    static TransformOptions BuildOptions(const DeviceCapabilities& caps) {
        TransformOptions opt{};
        opt.stripFP16               = !caps.fp16Native;
        opt.emulate8BitStorage      = !caps.int8Support;
        opt.strip16BitStorage       = !caps.shaderInt16;
        opt.stripDescriptorIndexing = !caps.descriptorTier2;
        opt.maxDescriptorSetSize    = caps.maxDescriptors > 0 ? caps.maxDescriptors : 16;
        opt.stripSER = !(caps.vendorID == 0x10DE && caps.hasSER);
        return opt;
    }

    static std::vector<uint32_t> Process(
        const std::vector<uint32_t>& spirv,
        const TransformOptions&      opts);

private:
    static void StripFP16Ops(std::vector<uint32_t>& spirv);
    static void Strip16BitStorage(std::vector<uint32_t>& spirv);
    static void Emulate8BitStorage(std::vector<uint32_t>& spirv);
    static void RemoveSERDirectives(std::vector<uint32_t>& spirv);
    static void SplitDescriptorSets(std::vector<uint32_t>& spirv, uint32_t maxSize);
};

} // namespace GoCL