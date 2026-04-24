#include "LegacyShaderEmulator.h"

#include <unordered_map>
#include <unordered_set>
#include <cstring>


namespace GoCL {

// ---------------------------------------------------------------------------
// SPIR-V helpers
// ---------------------------------------------------------------------------
static constexpr uint32_t kMagic  = 0x07230203;
static constexpr uint32_t kHeader = 5; // words before first instruction

static inline uint16_t Op(uint32_t w)  { return static_cast<uint16_t>(w & 0xFFFF); }
static inline uint16_t Wc(uint32_t w)  { return static_cast<uint16_t>(w >> 16); }
static inline uint32_t Hdr(uint16_t wc, uint16_t op) {
    return (static_cast<uint32_t>(wc) << 16) | op;
}
// Fill wc words starting at off with individual OpNop words
static void NopRange(std::vector<uint32_t>& spv, uint32_t off, uint16_t wc) {
    for (uint16_t k = 0; k < wc; ++k)
        spv[off + k] = Hdr(1, 0); // OpNop = opcode 0, wordCount 1
}

// Walk instructions (const)
template<typename F>
static void Walk(const std::vector<uint32_t>& spv, F&& fn) {
    if (spv.size() < kHeader || spv[0] != kMagic) return;
    for (uint32_t i = kHeader; i < spv.size(); ) {
        uint16_t wc = Wc(spv[i]);
        if (!wc) break;
        fn(i, Op(spv[i]), wc);
        i += wc;
    }
}

// Walk instructions (mutable)
template<typename F>
static void WalkMut(std::vector<uint32_t>& spv, F&& fn) {
    if (spv.size() < kHeader || spv[0] != kMagic) return;
    for (uint32_t i = kHeader; i < spv.size(); ) {
        uint16_t wc = Wc(spv[i]);
        if (!wc) break;
        fn(i, Op(spv[i]), wc);
        i += wc;
    }
}

// Compare null-terminated string against word-packed UTF-8 in SPIR-V
static bool SpvStrEq(const uint32_t* words, uint32_t wordCount, const char* target) {
    const char* p   = reinterpret_cast<const char*>(words);
    const size_t n  = wordCount * 4;
    const size_t tl = std::strlen(target);
    return tl < n && std::strncmp(p, target, tl) == 0 && p[tl] == '\0';
}

// SPIR-V opcodes
enum : uint16_t {
    Op_Nop        = 0,
    Op_Extension  = 10,
    Op_Capability = 17,
    Op_TypeInt    = 21,
    Op_TypeFloat  = 22,
    Op_FConvert   = 115,
    Op_Decorate   = 71,
};

// SPIR-V capabilities
enum : uint32_t {
    Cap_Float16                   = 9,
    Cap_Int8                      = 39,
    Cap_StorageBuffer8Bit         = 4448,
    Cap_ShaderInvocationReorderNV = 5383,
    Cap_16BitStorage_SB           = 4433,
    Cap_16BitStorage_UNI          = 4434,
    Cap_16BitStorage_PA           = 4435,
    Cap_16BitStorage_IO           = 4436,
};

// SPIR-V decorations
enum : uint32_t {
    Dec_DescriptorSet = 34,
    Dec_Binding       = 33,
};

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------
std::vector<uint32_t> LegacyShaderEmulator::Process(
    const std::vector<uint32_t>& spirv,
    const TransformOptions&      opts)
{
    if (spirv.empty() || spirv[0] != kMagic) return {};

    std::vector<uint32_t> out = spirv;
    if (opts.stripFP16)               StripFP16Ops(out);
    if (opts.strip16BitStorage)       Strip16BitStorage(out);
    if (opts.emulate8BitStorage)      Emulate8BitStorage(out);
    if (opts.stripSER)                RemoveSERDirectives(out);
    if (opts.stripDescriptorIndexing) SplitDescriptorSets(out, opts.maxDescriptorSetSize);
    return out;
}

// ---------------------------------------------------------------------------
// StripFP16Ops
// Patch OpTypeFloat width 16 → 32 in-place (same result ID).
// Replace OpFConvert → OpCopyObject (identity; type already widened).
// NOP out OpCapability Float16.
// ---------------------------------------------------------------------------
void LegacyShaderEmulator::StripFP16Ops(std::vector<uint32_t>& spv) {
    std::unordered_set<uint32_t> fp16ids;
    Walk(spv, [&](uint32_t off, uint16_t op, uint16_t wc) {
        if (op == Op_TypeFloat && wc >= 3 && spv[off + 2] == 16)
            fp16ids.insert(spv[off + 1]);
    });
    if (fp16ids.empty()) return;

    static constexpr uint16_t Op_CopyObject = 83; // OpCopyObject result_type result_id operand
    WalkMut(spv, [&](uint32_t off, uint16_t op, uint16_t wc) {
        if (op == Op_TypeFloat && wc >= 3 && spv[off + 2] == 16) {
            spv[off + 2] = 32; // widen: fp16 → fp32, same ID
        }
        else if (op == Op_Capability && wc >= 2 && spv[off + 1] == Cap_Float16) {
            NopRange(spv, off, wc);
        }
        else if (op == Op_FConvert && wc >= 4) {
            // Both sides are now fp32 — make it an identity copy
            spv[off] = Hdr(4, Op_CopyObject);
            // operands: result_type[off+1] result_id[off+2] operand[off+3] — unchanged
        }
        else if (op == Op_Extension && wc >= 2) {
            if (SpvStrEq(&spv[off + 1], wc - 1, "SPV_KHR_float_controls"))
                NopRange(spv, off, wc);
        }
    });
}

// ---------------------------------------------------------------------------
// Strip16BitStorage
// ---------------------------------------------------------------------------
void LegacyShaderEmulator::Strip16BitStorage(std::vector<uint32_t>& spv) {
    WalkMut(spv, [&](uint32_t off, uint16_t op, uint16_t wc) {
        if (op == Op_Capability && wc >= 2) {
            uint32_t cap = spv[off + 1];
            if (cap == Cap_16BitStorage_SB  || cap == Cap_16BitStorage_UNI ||
                cap == Cap_16BitStorage_PA  || cap == Cap_16BitStorage_IO)
                NopRange(spv, off, wc);
        }
        else if (op == Op_Extension && wc >= 2) {
            if (SpvStrEq(&spv[off + 1], wc - 1, "SPV_KHR_16bit_storage"))
                NopRange(spv, off, wc);
        }
    });
}

// ---------------------------------------------------------------------------
// Emulate8BitStorage
// Widen OpTypeInt width 8 → 32 (same ID).
// NOP out Int8 / StorageBuffer8BitAccess capability + extension.
// ---------------------------------------------------------------------------
void LegacyShaderEmulator::Emulate8BitStorage(std::vector<uint32_t>& spv) {
    std::unordered_set<uint32_t> int8ids;
    Walk(spv, [&](uint32_t off, uint16_t op, uint16_t wc) {
        if (op == Op_TypeInt && wc >= 4 && spv[off + 2] == 8)
            int8ids.insert(spv[off + 1]);
    });
    if (int8ids.empty()) return;

    WalkMut(spv, [&](uint32_t off, uint16_t op, uint16_t wc) {
        if (op == Op_TypeInt && wc >= 4 && spv[off + 2] == 8) {
            spv[off + 2] = 32; // widen: i8 → i32, same ID
        }
        else if (op == Op_Capability && wc >= 2) {
            uint32_t cap = spv[off + 1];
            if (cap == Cap_Int8 || cap == Cap_StorageBuffer8Bit)
                NopRange(spv, off, wc);
        }
        else if (op == Op_Extension && wc >= 2) {
            if (SpvStrEq(&spv[off + 1], wc - 1, "SPV_KHR_8bit_storage"))
                NopRange(spv, off, wc);
        }
    });
}

// ---------------------------------------------------------------------------
// RemoveSERDirectives
// NOP out NVIDIA SER capability + extension declaration.
// ---------------------------------------------------------------------------
void LegacyShaderEmulator::RemoveSERDirectives(std::vector<uint32_t>& spv) {
    WalkMut(spv, [&](uint32_t off, uint16_t op, uint16_t wc) {
        if (op == Op_Capability && wc >= 2 &&
            spv[off + 1] == Cap_ShaderInvocationReorderNV) {
            NopRange(spv, off, wc);
        }
        else if (op == Op_Extension && wc >= 2) {
            if (SpvStrEq(&spv[off + 1], wc - 1, "SPV_NV_shader_invocation_reorder"))
                NopRange(spv, off, wc);
        }
    });
}

// ---------------------------------------------------------------------------
// SplitDescriptorSets
// Best-effort binding remap: if binding >= maxSize, spill to next set index.
// Full IR-level set splitting requires rewriting OpVariable and access chains
// which changes the SPIR-V bound — deferred to a future dedicated pass.
// ---------------------------------------------------------------------------
void LegacyShaderEmulator::SplitDescriptorSets(
    std::vector<uint32_t>& spv, uint32_t maxSize)
{
    if (maxSize == 0) return;

    // Collect current set assignments
    std::unordered_map<uint32_t, uint32_t> idSet, idBinding;
    Walk(spv, [&](uint32_t off, uint16_t op, uint16_t wc) {
        if (op == Op_Decorate && wc >= 4) {
            uint32_t id  = spv[off + 1];
            uint32_t dec = spv[off + 2];
            uint32_t val = spv[off + 3];
            if (dec == Dec_DescriptorSet) idSet[id]     = val;
            if (dec == Dec_Binding)       idBinding[id] = val;
        }
    });

    WalkMut(spv, [&](uint32_t off, uint16_t op, uint16_t wc) {
        if (op == Op_Decorate && wc >= 4) {
            uint32_t id  = spv[off + 1];
            uint32_t dec = spv[off + 2];
            auto bIt = idBinding.find(id);
            if (bIt == idBinding.end() || bIt->second < maxSize) return;

            uint32_t overflow = bIt->second / maxSize;
            if (dec == Dec_DescriptorSet)
                spv[off + 3] = idSet[id] + overflow;
            else if (dec == Dec_Binding)
                spv[off + 3] = bIt->second % maxSize;
        }
    });
}

} // namespace GoCL