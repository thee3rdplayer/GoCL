#pragma once

#include "ASTCThresholds.h"

#include <cstdint>
#include <string>

// =============================================================================
// Reads GoCL.conf at engine start and populates RuntimeConfig.
//
// This is the only place that touches the configuration file. Every subsystem that needs
// a configurable value gets it from RuntimeConfig, not by parsing the file
// themselves.
//
// All values have safe defaults that work across covered cross‑generational
// hardware.
//
// NOTE: This parser accepts a restricted plain text-like format.
//        Supported: flat key/value pairs with integer, float, bool,
//        and bare-word string values.
//        Unsupported: quoted configuration strings, arrays, inline tables,
//        and nested tables.
// =============================================================================

namespace GoCL {

/**
 * @brief Engine utilities – controls the quality vs performance trade‑off for
 *        dynamic resolution upscaling when using the engine's `DynamicResolutionHelper`.
 * 
 * @note Proxy‑based DRS ignores this enum and only uses `scale`.
 */
enum class DynamicResolutionQuality : uint8_t {
    Performance,    ///< Fastest, lowest quality
    Balanced,       ///< Default, good trade‑off
    Quality         ///< Slower, best image quality
};

/**
 * @brief Proxy‑layer overrides for Dynamic Resolution Scaling.
 * 
 * The proxy only uses `enabled` and `scale`; `quality` is ignored in the proxy.
 */
struct DynamicResolutionConfig {
    bool enabled = false;   ///< Enable DRS in the proxy (swapchain extent scaling).
    float scale = 0.7f;     ///< Render resolution multiplier (clamped 0.25‑1.0).
    DynamicResolutionQuality quality = DynamicResolutionQuality::Balanced; ///< Not used in proxy.
};

/**
 * @brief Engine utilities – meshlet‑based GPU culling (only available in the static library).
 */
struct MeshletCullerConfig {
    bool enabled = true;    ///< Enable meshlet culling engine‑side.
};

/**
 * @brief Runtime configuration – loaded once from GoCL.conf.
 * 
 * Fields are grouped by usage: engine‑side (when linked as static library) and
 * proxy‑layer (when injected into pre‑built games). Both share the same config file.
 */
struct RuntimeConfig {
    // ─── Engine‑side settings ──────────────────────────────────────────────
    /// Affects applications linking GoCL statically.
    ASTCThresholds astcThresholds{};    ///< Controls ASTC block size aggressiveness.

    // ─── Proxy‑layer overrides ─────────────────────────────────────────────
    /// Affects injected Vulkan games.
    uint32_t       stagingSizeMB         = 64;   ///< Staging buffer size for texture uploads (MB).
    uint32_t       maxFramesInFlight     = 2;    ///< Swapchain image count; expected range 1‑4.
    std::string    pipelineCachePath     = "gocl_cache.bin";    ///< File path for pipeline cache persistence.
    bool           disableDGC            = false;   ///< If true, proxy won't use VK_EXT_device_generated_commands even if supported; falls back to CPU‑driven indirect draws.
    bool           skipAstcTranscode     = false;   ///< If true, proxy won't transcode ASTC textures to ETC2 (use when another layer already handles transcoding).

    // ─── Engine‑side settings (also respected by proxy when appropriate) ──
    uint32_t       msaaSamples        = 1;   ///< 1 = no MSAA; other valid values: 2,4,8,16,32,64.
    bool           enableTessellation = false;   ///< Enable tessellation support (requires pipeline state).

    // ─── Engine utilities ──────────────────────────────────────────────────
    /// Only effective in static library builds.
    MeshletCullerConfig     meshletCuller{};

    DynamicResolutionConfig dynamicResolutionScaling{}; ///< DRS configuration (proxy ignores `quality`).

    // ─── Proxy‑layer overrides ─────────────────────────────────────────────
    std::string    preferredPresentMode;    ///< "fifo", "fifo_relaxed", "mailbox", "immediate", or empty.
};

/**
 * @brief Configuration loader for GoCL.conf
 */
class ConfigLoader {
public:
    /**
     * @brief Parses the configuration file at the given path.
     * 
     * @param path Path to configuration file (default "GoCL.conf").
     * @return RuntimeConfig with defaults if the file is missing or malformed.
     */
    static RuntimeConfig Load(const std::string& path = "GoCL.conf");

    /**
     * @brief Returns a default‑initialized RuntimeConfig (no file I/O).
     * 
     * @return Default RuntimeConfig instance.
     */
    static RuntimeConfig Default() { return {}; }
};

}   // namespace GoCL