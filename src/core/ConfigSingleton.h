#pragma once

#include "ConfigLoader.h"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace GoCL {

// =============================================================================
// Searches for GoCL.conf in the following order:
//   1. Environment variable GOCL_CONFIG (explicit path)
//   2. Current working directory (./GoCL.conf)
//   3. User config directory:
//        - Windows: %APPDATA%\GoCL\GoCL.conf
//        - Linux (XDG): $XDG_CONFIG_HOME/GoCL/GoCL.conf
//        - Linux (fallback): $HOME/.config/GoCL/GoCL.conf
//   4. System‑wide directory:
//        - Windows: %PROGRAMDATA%\GoCL\GoCL.conf
//        - Linux: /etc/GoCL/GoCL.conf
// Returns default‑initialized RuntimeConfig if no file found.
// =============================================================================

/**
 * @brief Loads configuration with fallback search path.
 * 
 * @return RuntimeConfig loaded from the first found config file, or default if none found.
 */
static RuntimeConfig LoadConfigWithFallback() {
    std::error_code ec;   // non‑throwing overload for filesystem operations

    // 1. Environment override – highest priority for developer/debugging.
    if (const char *env = std::getenv("GOCL_CONFIG")) {
        if (std::filesystem::exists(env, ec)) {
            return ConfigLoader::Load(env);
        }
    }

    // 2. Current working directory – convenient for in‑project configs.
    if (std::filesystem::exists("GoCL.conf", ec)) {
        return ConfigLoader::Load("GoCL.conf");
    }

    // 3. User config directory (platform‑specific)
#ifdef _WIN32
    if (const char *appdata = std::getenv("APPDATA")) {
        std::string path = std::string(appdata) + "\\GoCL\\GoCL.conf";
        if (std::filesystem::exists(path, ec)) {
            return ConfigLoader::Load(path);
        }
    }
#else
    // Linux: respect XDG_CONFIG_HOME if set (modern convention).
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME")) {
        std::string path = std::string(xdg) + "/GoCL/GoCL.conf";
        if (std::filesystem::exists(path, ec)) {
            return ConfigLoader::Load(path);
        }
    }

    // Fallback to traditional ~/.config/GoCL/GoCL.conf
    if (const char *home = std::getenv("HOME")) {
        std::string path = std::string(home) + "/.config/GoCL/GoCL.conf";
        if (std::filesystem::exists(path, ec)) {
            return ConfigLoader::Load(path);
        }
    }
#endif

    // 4. System‑wide configuration (lowest priority)
#ifdef _WIN32
    if (const char *progdata = std::getenv("PROGRAMDATA")) {
        std::string path = std::string(progdata) + "\\GoCL\\GoCL.conf";
        if (std::filesystem::exists(path, ec)) {
            return ConfigLoader::Load(path);
        }
    }
#else
    if (std::filesystem::exists("/etc/GoCL/GoCL.conf", ec)) {
        return ConfigLoader::Load("/etc/GoCL/GoCL.conf");
    }
#endif

    // No config file found – return default configuration (safe for all hardware).
    return RuntimeConfig{};
}

/**
 * @brief Thread‑safe cached config – loaded once at first call, then reused.
 * 
 * This function is safe to call from any thread after engine init.
 * 
 * @return const RuntimeConfig& Reference to the cached configuration.
 */
inline const RuntimeConfig& GetConfig() {
    static const RuntimeConfig cfg = LoadConfigWithFallback();
    return cfg;
}

}   // namespace GoCL