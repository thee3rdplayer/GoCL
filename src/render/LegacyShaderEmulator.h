#pragma once

#include "core/CapabilityOracle.h"

#include <vector>
#include <cstdint>

// =============================================================================
// Runtime SPIR‑V compatibility transformations for a limited subset of
// Vulkan feature‑gated capabilities.
//
// The emulator attempts to rewrite unsupported SPIR‑V capabilities when
// semantic preservation is possible. Certain capabilities still require
// native Vulkan feature support and cannot be safely emulated.
//
// Some transformations may increase register pressure, alter memory layouts,
// or fail when shader semantics cannot be preserved safely.
//
// All transformations operate on the caller's vector. If the caller needs
// to preserve the original, they should pass a copy (or use std::move).
// =============================================================================

namespace GoCL {

/**
 * @brief Runtime SPIR‑V compatibility transformations for Vulkan feature‑gated capabilities.
 */
class LegacyShaderEmulator {
public:
    // -----------------------------------------------------------------------
    // TransformOptions – controls which transformations are applied.
    // All flags default to false, meaning no patching is performed.
    // BuildOptions() fills these flags based on a DeviceCapabilities snapshot.
    // -----------------------------------------------------------------------
    struct TransformOptions {
        bool stripFP16               = false;   ///< Widen FP16 to FP32 (scalar only)
        bool stripSER                = false;   ///< Remove Shader Execution Reordering
        bool emulate8BitStorage      = false;   ///< (currently unsupported – reserved)
        bool strip16BitStorage       = false;   ///< (currently unsupported – reserved)

        // Descriptor indexing capability flags – populated from DeviceCapabilities.
        // Individual shaders may require additional indexing features beyond
        // those checked here; these flags are for forward‑looking validation.
        bool supportsRuntimeDescriptorArray      = false;
        bool supportsSampledImageNonUniform      = false;
        bool supportsStorageBufferNonUniform     = false;
        bool supportsStorageImageNonUniform      = false;
        bool supportsPartiallyBound              = false;

        // Descriptor set splitting – only enabled when descriptorLimitKnown is
        // true and maxDescriptorSetSize > 0.  Splitting requires a matching
        // pipeline layout and is not safe to apply in isolation.
        bool     descriptorLimitKnown      = false;
        uint32_t maxDescriptorSetSize      = 0;
    };

    // -----------------------------------------------------------------------
    // BuildOptions: produces a TransformOptions from a DeviceCapabilities
    // snapshot.  Each flag is set to true when the corresponding feature is
    // NOT natively supported – meaning the emulator must strip or emulate it.
    // -----------------------------------------------------------------------
    [[nodiscard]]
    static TransformOptions BuildOptions(const DeviceCapabilities& caps) {
        TransformOptions opt{};

        // FP16 and storage emulation flags – derived directly from caps.
        opt.stripFP16               = !caps.fp16Native;
        opt.emulate8BitStorage      = !caps.storage8BitAccess;
        opt.strip16BitStorage       = !caps.storage16BitAccess;

        // Record the queried descriptor indexing features (used only for
        // per‑shader validation or future splitting logic).
        opt.supportsRuntimeDescriptorArray      = caps.runtimeDescriptorArray;
        opt.supportsSampledImageNonUniform      = caps.shaderSampledImageArrayNonUniformIndexing;
        opt.supportsStorageBufferNonUniform     = caps.shaderStorageBufferArrayNonUniformIndexing;
        opt.supportsStorageImageNonUniform      = caps.shaderStorageImageArrayNonUniformIndexing;
        opt.supportsPartiallyBound              = caps.descriptorBindingPartiallyBound;

        // Descriptor indexing is insufficient if any of the five core
        // features is missing.  This heuristic enables descriptor set
        // splitting only when absolutely necessary.
        bool descriptorIndexingInsufficient =
            !(
                caps.runtimeDescriptorArray &&
                caps.descriptorBindingPartiallyBound &&
                caps.shaderSampledImageArrayNonUniformIndexing &&
                caps.shaderStorageBufferArrayNonUniformIndexing &&
                caps.shaderStorageImageArrayNonUniformIndexing
            );

        if (descriptorIndexingInsufficient) {
            if (caps.maxPerStageDescriptorSamplers > 0) {
                opt.descriptorLimitKnown  = true;
                opt.maxDescriptorSetSize  = caps.maxPerStageDescriptorSamplers;
                // WARNING: Vulkan descriptor capacity is governed by multiple
                // per‑type and per‑stage limits. A single aggregate count is
                // only a heuristic and must not be treated as a guaranteed
                // descriptor set capacity.
            } else {
                opt.descriptorLimitKnown  = false;
                opt.maxDescriptorSetSize  = 0;  // disable splitting
            }
        }

        return opt;
    }

    // -----------------------------------------------------------------------
    // Process – applies the requested transformations.
    //
    // Input SPIR‑V must already satisfy baseline structural validity.
    // This class performs feature‑compatibility transformations and is
    // not a full SPIR‑V validator.
    //
    // Returns transformed SPIR‑V (empty vector on fatal error).
    // Throws std::runtime_error on malformed or unsupported input.
    // -----------------------------------------------------------------------
    [[nodiscard]]
    static std::vector<uint32_t> Process(
        std::vector<uint32_t>       spirv,  // pass by value – caller can std::move
        const TransformOptions&     opts);

    // -----------------------------------------------------------------------
    // Validate – checks that no unsupported capability remains after patching.
    // Returns true if the shader is compatible with the given device caps.
    // This is not a full SPIR‑V validator; only the capabilities that the
    // emulator knows how to strip are checked.
    // -----------------------------------------------------------------------
    [[nodiscard]] static bool Validate(
        const std::vector<uint32_t>& spirv,
        const DeviceCapabilities&    caps);

private:
    enum class TransformResult {
        Unchanged,   ///< No modifications made
        Modified,    ///< Changes were applied successfully
        Unsupported, ///< Transformation cannot be performed (caller should fall back)
        Failed       ///< Internal error (e.g., malformed SPIR‑V)
    };

    // Individual transformation helpers. Each returns a TransformResult.
    [[nodiscard]] static TransformResult StripFP16Ops(std::vector<uint32_t>& spirv);
    [[nodiscard]] static TransformResult Strip16BitStorage(std::vector<uint32_t>& spirv);
    [[nodiscard]] static TransformResult Emulate8BitStorage(std::vector<uint32_t>& spirv);
    [[nodiscard]] static TransformResult RemoveSERDirectives(std::vector<uint32_t>& spirv);
    [[nodiscard]] static TransformResult SplitDescriptorSets(std::vector<uint32_t>& spirv, uint32_t maxSize);
};

}   // namespace GoCL