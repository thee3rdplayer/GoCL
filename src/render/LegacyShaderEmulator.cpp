#include "LegacyShaderEmulator.h"

#include <unordered_set>
#include <cstring>
#include <cstdio>

namespace GoCL {

// ---------------------------------------------------------------------------
// SPIR‑V helpers
// ---------------------------------------------------------------------------
static constexpr uint32_t kMagic  = 0x07230203;   // SPIR‑V magic number
static constexpr uint32_t kHeader = 5;            // words in SPIR‑V header

static inline uint16_t Op(uint32_t w) { return static_cast<uint16_t>(w & 0xFFFF); }
static inline uint16_t Wc(uint32_t w) { return static_cast<uint16_t>(w >> 16); }
static inline uint32_t Hdr(uint16_t wc, uint16_t op) {
    return (static_cast<uint32_t>(wc) << 16) | op;
}

/**
 * @brief Replace a range of words with OpNop instructions (each Nop is one word).
 */
static void NopRange(std::vector<uint32_t>& spv, uint32_t off, uint16_t wc) {
    for (uint16_t k = 0; k < wc; ++k) {
        spv[off + k] = Hdr(1, 0);
    }
}

/**
 * @brief Read‑only traversal of SPIR‑V instructions.
 */
template<typename F>
static void Walk(const std::vector<uint32_t>& spv, F&& fn) {
    if (spv.size() < kHeader || spv[0] != kMagic) return;
    for (uint32_t i = kHeader; i < spv.size(); ) {
        uint16_t wc = Wc(spv[i]);
        if (!wc || i + wc > spv.size()) return;
        fn(i, Op(spv[i]), wc);
        i += wc;
    }
}

/**
 * @brief Read‑write traversal (for patching).
 */
template<typename F>
static void WalkMut(std::vector<uint32_t>& spv, F&& fn) {
    if (spv.size() < kHeader || spv[0] != kMagic) return;
    for (uint32_t i = kHeader; i < spv.size(); ) {
        uint16_t wc = Wc(spv[i]);
        if (!wc || i + wc > spv.size()) return;
        fn(i, Op(spv[i]), wc);
        i += wc;
    }
}

/**
 * @brief Compare a SPIR‑V literal string with a C string.
 * 
 * Assumes the SPIR‑V string is null‑terminated.
 */
static bool SpvStrEq(const uint32_t* words, uint32_t wordCount, const char* target) {
    const char* p   = reinterpret_cast<const char*>(words);
    const size_t n  = wordCount * 4;
    const size_t tl = std::strlen(target);
    return tl < n && std::strncmp(p, target, tl) == 0 && p[tl] == '\0';
}

/**
 * @brief Convert IEEE 754‑2008 half‑precision (binary16) to single‑precision (binary32).
 * 
 * Handles denormals, NaN, and ±Inf correctly.
 */
static uint32_t F16ToF32(uint16_t h) {
    uint32_t sign = (h >> 15) & 1;
    int32_t  exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;

    if (exp == 0) {
        if (mant == 0) return sign << 31;          // ±0
        while ((mant & 0x400) == 0) { mant <<= 1; exp--; }
        exp++;
        mant &= ~0x400;
    } else if (exp == 31) {
        return (sign << 31) | (0xFF << 23) | (mant << 13); // ±Inf/NaN
    }
    return (sign << 31) | ((exp + 112) << 23) | (mant << 13);
}

// SPIR‑V instruction opcodes relevant to the emulator.
enum : uint16_t {
    Op_Nop        = 0,
    Op_Extension  = 10,
    Op_Capability = 17,
    Op_TypeInt    = 21,
    Op_TypeFloat  = 22,
    Op_Constant   = 43,
    Op_FConvert   = 115,
    Op_Decorate   = 71,
};

// SPIR‑V capability IDs.
enum : uint32_t {
    Cap_Float16                    = 9,
    Cap_Int8                       = 39,
    Cap_StorageBuffer8Bit          = 4448,
    Cap_16BitStorage_SB            = 4433,
    Cap_16BitStorage_UNI           = 4434,
    Cap_16BitStorage_PA            = 4435,
    Cap_16BitStorage_IO            = 4436,
};

// SPIR‑V decoration IDs (used in descriptor set splitting, currently disabled).
enum : uint32_t {
    Dec_DescriptorSet = 34,
    Dec_Binding       = 33,
};

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------
/**
 * @brief Applies the requested transformations in order.
 * 
 * Returns the patched SPIR‑V. If any transformation fails or is unsupported,
 * returns an empty vector (the caller should then fall back to the original shader).
 */
std::vector<uint32_t> LegacyShaderEmulator::Process(
    std::vector<uint32_t>       spirv,
    const TransformOptions&     opts) {
    if (spirv.empty() || spirv[0] != kMagic) {
        return {};
    }

    if (opts.stripFP16) {
        auto r = StripFP16Ops(spirv);
        if (r == TransformResult::Failed || r == TransformResult::Unsupported) {
            return {};
        }
    }

    if (opts.strip16BitStorage) {
        auto r = Strip16BitStorage(spirv);
        if (r == TransformResult::Failed || r == TransformResult::Unsupported) {
            return {};
        }
    }

    if (opts.emulate8BitStorage) {
        auto r = Emulate8BitStorage(spirv);
        if (r == TransformResult::Failed || r == TransformResult::Unsupported) {
            return {};
        }
    }

    // Descriptor set splitting is disabled by default because it requires
    // a matching pipeline layout. It can be enabled by the caller when
    // the max descriptor set size is known (e.g., in PipelineBuilder).
    if (opts.descriptorLimitKnown && opts.maxDescriptorSetSize > 0) {
        (void)SplitDescriptorSets(spirv, opts.maxDescriptorSetSize);
    }

    return spirv;
}

// ---------------------------------------------------------------------------
// StripFP16Ops
// ---------------------------------------------------------------------------
/**
 * @brief Widen FP16 types and constants to FP32. Also removes the Capability.
 * 
 * **Limitation:** Only scalar FP16 constants are widened. If the shader uses
 * FP16 in any other way (arithmetic, vectors, OpFConvert), the transformation
 * is considered unsupported and the function returns Unsupported. A full
 * def‑use rewrite would be needed for complex FP16 usage, which is not yet
 * implemented.
 */
LegacyShaderEmulator::TransformResult
LegacyShaderEmulator::StripFP16Ops(std::vector<uint32_t>& spv) {
    std::unordered_set<uint32_t> fp16Types;

    // 1. Collect all OpTypeFloat with width 16.
    Walk(spv, [&](uint32_t off, uint16_t op, uint16_t wc) {
        if (op == Op_TypeFloat && wc >= 3 && spv[off + 2] == 16) {
            fp16Types.insert(spv[off + 1]);
        }
    });

    if (fp16Types.empty()) {
        return TransformResult::Unchanged;
    }

    // 2. Detect any instruction that would make safe widening impossible.
    bool canWiden = true;
    Walk(spv, [&](uint32_t off, uint16_t op, uint16_t wc) {
        if (!canWiden) return;

        if (op == Op_TypeFloat) {
            // Already collected – ignore.
        } else if (op == Op_Constant) {
            // Only scalar constants (wc==3) are safe to widen.
            if (wc != 3 && fp16Types.count(spv[off + 1])) {
                canWiden = false;
            }
        } else if (op == Op_Capability || op == Op_Extension) {
            // These will be NOPed later – harmless.
        } else if (op == Op_FConvert) {
            // OpFConvert semantics would be broken by type widening.
            canWiden = false;
        } else {
            // Any other instruction that uses a FP16 type as its result type
            // (second word) cannot be handled without full def‑use rewrite.
            if (wc >= 2 && fp16Types.count(spv[off + 1])) {
                canWiden = false;
            }
        }
    });

    if (!canWiden) {
        fprintf(stderr, "GoCL: FP16 shader contains complex operations – "
                        "cannot safely widen to FP32.\n");
        return TransformResult::Unsupported;
    }

    // 3. Perform widening: change type width, widen scalar constants,
    //    and remove the FP16 capability.
    WalkMut(spv, [&](uint32_t off, uint16_t op, uint16_t wc) {
        if (op == Op_TypeFloat && wc >= 3 && spv[off + 2] == 16) {
            spv[off + 2] = 32;  // widen type
        } else if (op == Op_Constant && wc == 3 && fp16Types.count(spv[off + 1])) {
            uint16_t h = static_cast<uint16_t>(spv[off + 2] & 0xFFFF);
            spv[off + 2] = F16ToF32(h); // widen constant value
        } else if (op == Op_Capability && wc >= 2 && spv[off + 1] == Cap_Float16) {
            NopRange(spv, off, wc); // remove FP16 capability
        }
    });

    return TransformResult::Modified;
}

// ---------------------------------------------------------------------------
// Strip16BitStorage
// ---------------------------------------------------------------------------
/**
 * @brief Intentionally disabled because safely widening 16‑bit integer types to 32‑bit
 * requires a full def‑use rewrite that updates struct layouts, offsets,
 * array strides, access chains, and all load/store operations.
 * Without that, the transformation would produce invalid SPIR‑V.
 */
LegacyShaderEmulator::TransformResult
LegacyShaderEmulator::Strip16BitStorage(std::vector<uint32_t>& spv) {
    (void)spv;
    fprintf(stderr, "GoCL: 16‑bit storage widening requested but not yet implemented.\n");
    return TransformResult::Unsupported;
}

// ---------------------------------------------------------------------------
// Emulate8BitStorage
// ---------------------------------------------------------------------------
/**
 * @brief Similarly disabled for the same reasons as 16‑bit storage.
 */
LegacyShaderEmulator::TransformResult
LegacyShaderEmulator::Emulate8BitStorage(std::vector<uint32_t>& spv) {
    (void)spv;
    fprintf(stderr, "GoCL: 8‑bit storage emulation requested but not yet implemented.\n");
    return TransformResult::Unsupported;
}

// ---------------------------------------------------------------------------
// Validate
// ---------------------------------------------------------------------------
/**
 * @brief Verifies that no unsupported capabilities remain after patching.
 * 
 * @return true if the shader is compatible with the device's capabilities.
 */
bool LegacyShaderEmulator::Validate(
    const std::vector<uint32_t>& spv,
    const DeviceCapabilities&    caps) {
    if (spv.size() < kHeader || spv[0] != kMagic) {
        return false;
    }

    bool ok = true;
    Walk(spv, [&](uint32_t off, uint16_t op, uint16_t wc) {
        if (op != Op_Capability || wc < 2) return;

        switch (spv[off + 1]) {
            case Cap_Float16:
                if (!caps.fp16Native) {
                    ok = false;
                    fprintf(stderr, "GoCL: FP16 capability still present after patching.\n");
                }
                break;

            case Cap_Int8:
            case Cap_StorageBuffer8Bit:
                if (!caps.int8Support) {
                    ok = false;
                    fprintf(stderr, "GoCL: 8‑bit capability still present after patching.\n");
                }
                break;

            case Cap_16BitStorage_SB:
            case Cap_16BitStorage_UNI:
            case Cap_16BitStorage_PA:
            case Cap_16BitStorage_IO:
                if (!caps.storage16BitAccess) {
                    ok = false;
                    fprintf(stderr, "GoCL: 16‑bit storage capability still present after patching.\n");
                }
                break;

            default:
                break;
        }
    });

    return ok;
}

// ---------------------------------------------------------------------------
// SplitDescriptorSets
// ---------------------------------------------------------------------------
/**
 * @brief Placeholder for future implementation.
 * 
 * Splitting descriptor sets across multiple bindings would require both SPIR‑V
 * patching and a matching pipeline layout. It is not safe to do in isolation,
 * so this function currently does nothing.
 */
LegacyShaderEmulator::TransformResult
LegacyShaderEmulator::SplitDescriptorSets(std::vector<uint32_t>& spv,
                                          uint32_t /*maxSize*/) {
    (void)spv;
    return TransformResult::Unchanged;
}

}   // namespace GoCL