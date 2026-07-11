#include "ConfigLoader.h"
#include <vulkan/vulkan.h>

#include <fstream>
#include <string>
#include <cstdlib>
#include <cstring>
#include <cctype>

// =============================================================================
// The parser supports:
//   - sections ([section.name])
//   - key = value pairs
//   - (integer, float, bool, and string values)
//   - quoted and unquoted strings supported
//   - # comments (full‑line and inline)
//   - blank lines and whitespace‑insensitive indentation
//
// Unknown keys or malformed lines are silently ignored so new options
// can be added without breaking existing files.
// =============================================================================

namespace GoCL {

/**
 * @brief Helper: remove leading/trailing whitespace (spaces, tabs, CR, LF).
 */
static std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

RuntimeConfig ConfigLoader::Load(const std::string& path) {
    RuntimeConfig cfg;

    std::ifstream file(path);
    if (!file) return cfg;  // missing file → all defaults

    std::string line;
    std::string currentSection;
    
    while (std::getline(file, line)) {
        std::string t = Trim(line);
        
        // Skip empty lines and full‑line comments
        if (t.empty() || t[0] == '#') continue;

        // Detect section headers: [section.name]
        if (t.front() == '[' && t.back() == ']') {
            currentSection = Trim(t.substr(1, t.size() - 2));
            continue;
        }

        // Split on first '=' sign (key = value)
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;

        std::string key = Trim(t.substr(0, eq));
        std::string val = Trim(t.substr(eq + 1));

        // Strip inline comments (anything after a '#', except when inside quotes).
        // The parser does not support quoted strings containing '#' – acceptable for GoCL.conf.
        auto comment = val.find('#');
        if (comment != std::string::npos) {
            val = Trim(val.substr(0, comment));
        }

        // Remove surrounding quotes (both single and double) so that values like "512" become 512.
        // This makes numbers, booleans, and enums work seamlessly with quoted plain text syntax.
        if (val.size() >= 2) {
            char first = val.front(), last = val.back();
            if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
                val = val.substr(1, val.size() - 2);
            }
        }

        // ── Helper lambdas for type‑safe parsing ─────────────────────────────
        // These lambdas silently ignore conversion errors; the config stays at its default value.
        auto parseUint = [&val](uint32_t& target) {
            try { target = static_cast<uint32_t>(std::stoull(val)); } catch (...) {}
        };
        
        auto parseBool = [&val](bool& target) {
            if (val == "true" || val == "1" || val == "yes") {
                target = true;
            } else if (val == "false" || val == "0" || val == "no") {
                target = false;
            }
            // Any other string leaves the target unchanged (default).
        };
        
        auto parseFloat = [&val](float& target) {
            try { target = std::stof(val); } catch (...) {}
        };
        
        auto parseString = [&val](std::string& target) {
            target = val;
            // Strip quotes again (in case they were not removed earlier – but already done).
            // This ensures the string is stored without surrounding quotes.
            if (target.size() >= 2 &&
                ((target.front() == '"' && target.back() == '"') ||
                 (target.front() == '\'' && target.back() == '\''))) {
                target = target.substr(1, target.size() - 2);
            }
        };

        // ── Dispatch by section ─────────────────────────────────────────────
        if (currentSection == "astc.thresholds") {
            if (key == "largePixels") {
                parseUint(cfg.astcThresholds.largePixels);
            } else if (key == "mediumPixels") {
                parseUint(cfg.astcThresholds.mediumPixels);
            } else if (key == "tightVRAM") {
                parseUint(cfg.astcThresholds.tightVRAM);
            }
        } else if (currentSection == "render") {
            if (key == "max_frames_in_flight") {
                uint32_t value = cfg.maxFramesInFlight;
                parseUint(value);
                // Clamp to sane range [1,4] to avoid invalid swapchain configurations.
                if (value >= 1 && value <= 4) {
                    cfg.maxFramesInFlight = value;
                }
            } else if (key == "msaa_samples") {
                uint32_t samples = 0;
                parseUint(samples);
                
                // Convert integer sample count to Vulkan enum; unsupported values are ignored.
                switch (samples) {
                    case 1:  cfg.msaaSamples = VK_SAMPLE_COUNT_1_BIT;  break;
                    case 2:  cfg.msaaSamples = VK_SAMPLE_COUNT_2_BIT;  break;
                    case 4:  cfg.msaaSamples = VK_SAMPLE_COUNT_4_BIT;  break;
                    case 8:  cfg.msaaSamples = VK_SAMPLE_COUNT_8_BIT;  break;
                    case 16: cfg.msaaSamples = VK_SAMPLE_COUNT_16_BIT; break;
                    case 32: cfg.msaaSamples = VK_SAMPLE_COUNT_32_BIT; break;
                    case 64: cfg.msaaSamples = VK_SAMPLE_COUNT_64_BIT; break;
                    default: /* ignore invalid, keep default */ break;
                }
            } else if (key == "enable_tessellation") {
                parseBool(cfg.enableTessellation);
            } else if (key == "preferred_present_mode") {
                parseString(cfg.preferredPresentMode);
            } else if (key == "disable_dgc") {
                parseBool(cfg.disableDGC);
            } else if (key == "skip_astc_transcode") {
                parseBool(cfg.skipAstcTranscode);
            }
        } else if (currentSection == "go_context") {
            if (key == "staging_size_mb") {
                uint32_t value = cfg.stagingSizeMB;
                parseUint(value);
                if (value > 0) {
                    cfg.stagingSizeMB = value;  // Only override if valid positive integer.
                }
            } else if (key == "pipeline_cache_path") {
                parseString(cfg.pipelineCachePath); // Relative or absolute file path.
            }
        } else if (currentSection == "dynamic_resolution_scaling") {
            if (key == "enabled") {
                parseBool(cfg.dynamicResolutionScaling.enabled);
            } else if (key == "scale") {
                parseFloat(cfg.dynamicResolutionScaling.scale);
            } else if (key == "quality") {
                std::string q = val;
                // Convert to lowercase for case‑insensitivity (supports "Balanced", "balanced", "BALANCED").
                for (auto& c : q) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (q == "performance") {
                    cfg.dynamicResolutionScaling.quality = DynamicResolutionQuality::Performance;
                } else if (q == "balanced") {
                    cfg.dynamicResolutionScaling.quality = DynamicResolutionQuality::Balanced;
                } else if (q == "quality") {
                    cfg.dynamicResolutionScaling.quality = DynamicResolutionQuality::Quality;
                }
                // else keep default (Balanced)
            }
        } else if (currentSection == "meshlet_culler") {
            if (key == "enabled") {
                parseBool(cfg.meshletCuller.enabled);
            }
        }
    }
    return cfg;
}

}   // namespace GoCL