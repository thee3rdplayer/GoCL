#pragma once

// =============================================================================
// A lightweight, zero-dependency descriptor of GPU characteristics
// and engine-side performance heuristics.
//
// Populated once by CapabilityOracle::Query() and then read by ASTCBlockSelector,
// PipelineBuilder, and any future subsystem that needs to adjust its behaviour
// for specific hardware classes.
//
// Fields:
//   isMobileTier   — true for Mali, Adreno, and similar SoC GPUs.
//                    These benefit from aggressive compression and
//                    conservative resource limits.
//   isLowBandwidth — GPU class where memory bandwidth is commonly a
//                    performance bottleneck and compression/bandwidth-saving strategies
//                    should be preferred.
//
// Both fields default to false, which is safe for all desktop discrete GPUs.
// =============================================================================

namespace GoCL {

// Future expansion – not yet active
/*
enum class BandwidthClass : uint8_t {
    High,   // discrete GPU, plenty of VRAM
    Medium, // integrated or mobile with moderate bandwidth
    Low     // ultra-low bandwidth (e.g., SwiftShader, early Intel Gen9)
};
*/

/**
 * @brief Hardware empathy hints for performance tuning.
 * 
 * Provides coarse-grained classification of the target hardware
 * to guide compression, resource allocation, and rendering decisions.
 */
struct HardwareHint {
    bool isMobileTier   = false;   ///< Mali, Adreno, etc. Benefit from aggressive compression and conservative resource limits.
    bool isLowBandwidth = false;   ///< Intel integrated, some ARM cores. Memory bandwidth is commonly a bottleneck.

    // TODO: Future‑proof – replace booleans with BandwidthClass
    // BandwidthClass bandwidthClass = BandwidthClass::High;
};

}   // namespace GoCL