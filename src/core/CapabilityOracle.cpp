#include "CapabilityOracle.h"

#include <cstring>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace GoCL {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/**
 * @brief Returns true if the named instance extension is available.
 * 
 * Caches nothing; called only a few times during startup.
 */
static bool HasInstanceExtension(const char* name) {
    uint32_t count = 0;
    VkResult r = vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    if (r != VK_SUCCESS) return false;

    std::vector<VkExtensionProperties> props(count);
    r = vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());
    if (r != VK_SUCCESS) return false;

    for (auto& p : props) {
        if (std::strcmp(p.extensionName, name) == 0) return true;
    }
    return false;
}

/**
 * @brief Safe wrapper for vkGetPhysicalDeviceFeatures2.
 * 
 * vkGetPhysicalDeviceFeatures2 is core in Vulkan 1.1, or available via 
 * VK_KHR_get_physical_device_properties2 on 1.0. Returns false if neither is 
 * available (Vulkan 1.0 without the extension), in which case the caller must 
 * use vkGetPhysicalDeviceFeatures.
 */
static bool SafeGetPhysicalDeviceFeatures2(VkPhysicalDevice physDev,
                                           VkPhysicalDeviceFeatures2* feat2) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physDev, &props);

    if (props.apiVersion >= VK_API_VERSION_1_1) {
        vkGetPhysicalDeviceFeatures2(physDev, feat2);
        return true;
    }

    if (HasInstanceExtension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
        auto pfn = (PFN_vkGetPhysicalDeviceFeatures2KHR)
            vkGetInstanceProcAddr(nullptr, "vkGetPhysicalDeviceFeatures2KHR");
        if (pfn) {
            pfn(physDev, feat2);
            return true;
        }
    }
    return false;
}

// Extension list cache per physical device.
// g_lastExtensionDevice avoids re‑enumerating for the same device multiple times.
static std::unordered_map<VkPhysicalDevice,
                          std::vector<VkExtensionProperties>> g_extensionCache;
static VkPhysicalDevice g_lastExtensionDevice = VK_NULL_HANDLE;
static std::mutex g_extensionCacheMutex;

bool CapabilityOracle::CheckExtension(VkPhysicalDevice physDev, const char* name) {
    std::scoped_lock lock(g_extensionCacheMutex);

    if (g_lastExtensionDevice != physDev) {
        uint32_t count = 0;
        VkResult r = vkEnumerateDeviceExtensionProperties(physDev, nullptr, &count, nullptr);
        if (r != VK_SUCCESS) return false;

        auto& exts = g_extensionCache[physDev];
        exts.resize(count);
        r = vkEnumerateDeviceExtensionProperties(physDev, nullptr, &count, exts.data());
        if (r != VK_SUCCESS) {
            // Invalidate the cache entry so a future call can retry
            g_extensionCache.erase(physDev);
            return false;
        }
        g_lastExtensionDevice = physDev;
    }

    for (auto& e : g_extensionCache[physDev]) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

/**
 * @brief Map Vulkan API version to the maximum SPIR‑V version the device can consume.
 * 
 * This is a conservative upper bound; the actual SPIR‑V used may be lower.
 */
uint32_t CapabilityOracle::SpvVersionFromVk(uint32_t apiVersion) {
    const uint32_t major = VK_VERSION_MAJOR(apiVersion);
    const uint32_t minor = VK_VERSION_MINOR(apiVersion);
    
    if (major > 1 || (major == 1 && minor >= 3)) return 0x00010600; // SPIR‑V 1.6
    if (major == 1 && minor == 2)                return 0x00010500; // SPIR‑V 1.5
    if (major == 1 && minor == 1)                return 0x00010300; // SPIR‑V 1.3
    return 0x00010000;                                              // SPIR‑V 1.0
}

DeviceCapabilities CapabilityOracle::Query(VkPhysicalDevice physDev) {
    DeviceCapabilities caps{};

    // ── 1. Core properties ──────────────────────────────────────────────
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physDev, &props);

    caps.apiVersion                      = props.apiVersion;
    caps.driverVersion                   = props.driverVersion;
    caps.vendorID                        = props.vendorID;
    caps.maxImageDimension2D             = props.limits.maxImageDimension2D;
    caps.maxPerStageDescriptorSamplers   = props.limits.maxPerStageDescriptorSamplers;
    caps.maxPushConstantsSize            = props.limits.maxPushConstantsSize;
    caps.maxSpvVersion                   = SpvVersionFromVk(props.apiVersion);
    std::memcpy(caps.deviceName, props.deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE);

    // ── 2. Driver properties (only if available) ────────────────────────
    if (caps.apiVersion >= VK_API_VERSION_1_2 ||
        HasInstanceExtension(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME)) {
        VkPhysicalDeviceDriverProperties driverProps{};
        driverProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &driverProps;
        vkGetPhysicalDeviceProperties2(physDev, &props2);
        
        caps.driverID = driverProps.driverID;
        std::memcpy(caps.pipelineCacheUUID, props2.properties.pipelineCacheUUID, VK_UUID_SIZE);
        caps.isSwiftShader = (caps.driverID == VK_DRIVER_ID_GOOGLE_SWIFTSHADER);
    }

    // ── 3. Features ─────────────────────────────────────────────────────
    VkPhysicalDeviceFeatures2 feat2{};
    feat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

    VkPhysicalDeviceShaderFloat16Int8Features  fp16{};
    VkPhysicalDeviceDescriptorIndexingFeatures descIdx{};
    VkPhysicalDevice8BitStorageFeatures        storage8{};
    VkPhysicalDevice16BitStorageFeatures       storage16{};

    // Build the pNext chain: feat2 → fp16 → descIdx → storage8 → storage16
    fp16.sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
    descIdx.sType   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    storage8.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES;
    storage16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;

    feat2.pNext    = &fp16;
    fp16.pNext     = &descIdx;
    descIdx.pNext  = &storage8;
    storage8.pNext = &storage16;

    // Query all features in one go.
    const bool useExtended = SafeGetPhysicalDeviceFeatures2(physDev, &feat2);
    if (!useExtended) {
        // Fallback for Vulkan 1.0 devices – only base features are available.
        vkGetPhysicalDeviceFeatures(physDev, &feat2.features);
    }

    // ── Timeline semaphore support ──────────────────────────────────────
    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{};
    timelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;

    VkPhysicalDeviceFeatures2 timelineQuery{};
    timelineQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    timelineQuery.pNext = &timelineFeatures;

    SafeGetPhysicalDeviceFeatures2(physDev, &timelineQuery);

    caps.timelineSemaphoreEnabled = (timelineFeatures.timelineSemaphore == VK_TRUE);

    // Core feature flags – always valid even on Vulkan 1.0.
    caps.astcLdrHW   = feat2.features.textureCompressionASTC_LDR;
    caps.etc2Support = feat2.features.textureCompressionETC2;
    caps.shaderInt16 = feat2.features.shaderInt16;

    if (useExtended) {
        caps.fp16Native  = fp16.shaderFloat16;
        caps.int8Support = fp16.shaderInt8;

        // Combined bindless flag – true only when all three core descriptor indexing
        // features are present. Individual flags are also stored for finer decisions.
        caps.bindlessDescriptors = descIdx.shaderSampledImageArrayNonUniformIndexing &&
                                   descIdx.runtimeDescriptorArray &&
                                   descIdx.descriptorBindingVariableDescriptorCount;

        // Individual descriptor indexing flags.
        caps.runtimeDescriptorArray                         = descIdx.runtimeDescriptorArray;
        caps.descriptorBindingPartiallyBound                = descIdx.descriptorBindingPartiallyBound;
        caps.descriptorBindingVariableDescriptorCount       = descIdx.descriptorBindingVariableDescriptorCount;
        caps.shaderSampledImageArrayNonUniformIndexing      = descIdx.shaderSampledImageArrayNonUniformIndexing;
        caps.shaderStorageBufferArrayNonUniformIndexing     = descIdx.shaderStorageBufferArrayNonUniformIndexing;
        caps.shaderStorageImageArrayNonUniformIndexing      = descIdx.shaderStorageImageArrayNonUniformIndexing;

        // 8‑bit and 16‑bit storage access – used for shader‑side buffer reads/writes.
        caps.storage8BitAccess  = storage8.storageBuffer8BitAccess;
        caps.storage16BitAccess = storage16.storageBuffer16BitAccess;
    }

    // ── 4. ASTC HDR ─────────────────────────────────────────────────────
    caps.astcHdrHW = CheckExtension(physDev, VK_EXT_TEXTURE_COMPRESSION_ASTC_HDR_EXTENSION_NAME);

    // ── 5. Subgroup support ─────────────────────────────────────────────
    VkPhysicalDeviceSubgroupProperties subgroupProps{};
    subgroupProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    
    VkPhysicalDeviceProperties2 subgroupQuery{};
    subgroupQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    subgroupQuery.pNext = &subgroupProps;
    vkGetPhysicalDeviceProperties2(physDev, &subgroupQuery);

    const VkSubgroupFeatureFlags requiredOps =
        VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT |
        VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;

    // Subgroups are considered "supported" only if they can be used in compute shaders
    // and provide the fundamental operations required by the engine.
    caps.subgroupOps = ((subgroupProps.supportedOperations & requiredOps) == requiredOps) &&
                       (subgroupProps.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT);

    // ── 6. VRAM budget ──────────────────────────────────────────────────
    // This gives the total device‑local heap size as a static value.
    // The proxy will later use VK_EXT_memory_budget to obtain dynamic usage.
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);
    
    VkDeviceSize maxDeviceLocal = 0;
    for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
        if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            maxDeviceLocal = std::max(maxDeviceLocal, memProps.memoryHeaps[i].size);
        }
    }
    caps.vramBudgetMB = static_cast<uint32_t>(maxDeviceLocal / (1024 * 1024));

    // ── 7. Extension‑based capability detection ─────────────────────────
    caps.hasRayTracingPipeline   = CheckExtension(physDev, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    caps.hasMeshShaders          = CheckExtension(physDev, VK_EXT_MESH_SHADER_EXTENSION_NAME);
    caps.hasFragmentShadingRate  = CheckExtension(physDev, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
    caps.hasRayQuery             = CheckExtension(physDev, VK_KHR_RAY_QUERY_EXTENSION_NAME);
    caps.hasShaderModuleIdentifier = CheckExtension(physDev, VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME);
    
    bool hasDGC_EXT = CheckExtension(physDev, VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME);
    if (hasDGC_EXT) {
        // Query the feature struct to confirm DGC is actually supported
        VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT dgcFeatures{};
        dgcFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT;

        VkPhysicalDeviceFeatures2 dgcFeat2{};
        dgcFeat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        dgcFeat2.pNext = &dgcFeatures;

        vkGetPhysicalDeviceFeatures2(physDev, &dgcFeat2);

        caps.dgcSupported = (dgcFeatures.deviceGeneratedCommands == VK_TRUE);
    }

    // NVIDIA ASTC driver‑decode support is determined by the presence of VK_EXT_astc_decode_mode.
    // If the extension is missing, the driver does not support hardware ASTC decoding.
    if (caps.vendorID == 0x10DE) {
        caps.nvidiaNoASTCDriver = !CheckExtension(physDev, VK_EXT_ASTC_DECODE_MODE_EXTENSION_NAME);
    }

    // ── 8. Hardware empathy hint (vendor‑ID‑based) ──────────────────────
    // These heuristics are coarse but sufficient for the engine's adaptation.
    caps.hardwareHint.isMobileTier   = (caps.vendorID == 0x13B5 ||  // Mali
                                        caps.vendorID == 0x5143);   // Adreno
    caps.hardwareHint.isLowBandwidth = (caps.vendorID == 0x8086);   // Intel integrated

    // TODO: When BandwidthClass is uncommented in HardwareHint.h, enable this:
    /*
    if (caps.isLowBandwidth)
        caps.hardwareHint.bandwidthClass = BandwidthClass::Low;
    else if (caps.isMobileTier)
        caps.hardwareHint.bandwidthClass = BandwidthClass::Medium;
    else
        caps.hardwareHint.bandwidthClass = BandwidthClass::High;
    */

    // ── 9. Software renderer override ───────────────────────────────────
    // Software renderers often report fake capabilities; forcibly disable ASTC LDR
    // because they typically cannot decode it efficiently (or at all).
    if (caps.driverID == VK_DRIVER_ID_MESA_LLVMPIPE ||
        std::strstr(caps.deviceName, "SwiftShader") ||
        std::strstr(caps.deviceName, "llvmpipe")) {
        caps.isSwiftShader = true;
        caps.astcLdrHW = false;
    }

    return caps;
}

}   // namespace GoCL