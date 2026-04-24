#include "CapabilityOracle.h"

#include <cstring>
#include <algorithm>
#include <vector>    


namespace GoCL {

// -----------------------------------------------------------------------------
// SPIR-V version ceiling derived from Vulkan API version
// Vulkan 1.0 → SPIR-V 1.0 (0x00010000)
// Vulkan 1.1 → SPIR-V 1.3 (0x00010300)
// Vulkan 1.2 → SPIR-V 1.5 (0x00010500)
// Vulkan 1.3 → SPIR-V 1.6 (0x00010600)
// -----------------------------------------------------------------------------
uint32_t CapabilityOracle::SpvVersionFromVk(uint32_t apiVersion) {
    const uint32_t major = VK_VERSION_MAJOR(apiVersion);
    const uint32_t minor = VK_VERSION_MINOR(apiVersion);

    if (major > 1 || (major == 1 && minor >= 3)) return 0x00010600;
    if (major == 1 && minor == 2)                return 0x00010500;
    if (major == 1 && minor == 1)                return 0x00010300;
    return 0x00010000;  // Vulkan 1.0
}

// -----------------------------------------------------------------------------
DeviceCapabilities CapabilityOracle::Query(VkPhysicalDevice physDev) {
    DeviceCapabilities caps{};

    // -------------------------------------------------------------------------
    // 1. Core properties (props2 for driver ID — requires Vulkan 1.2 or
    //    VK_KHR_driver_properties; falls back gracefully if unavailable)
    // -------------------------------------------------------------------------
    VkPhysicalDeviceDriverProperties driverProps{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };

    VkPhysicalDeviceProperties2 props2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    props2.pNext = &driverProps;

    vkGetPhysicalDeviceProperties2(physDev, &props2);
    const VkPhysicalDeviceProperties& props = props2.properties;

    caps.apiVersion           = props.apiVersion;
    caps.driverVersion        = props.driverVersion;
    caps.vendorID             = props.vendorID;
    caps.maxImageDimension2D  = props.limits.maxImageDimension2D;
    caps.maxDescriptors       = props.limits.maxPerStageDescriptorSamplers;
    caps.maxPushConstantsSize = props.limits.maxPushConstantsSize;
    caps.maxSpvVersion        = SpvVersionFromVk(props.apiVersion);
    caps.driverID             = driverProps.driverID;
    std::memcpy(caps.deviceName, props.deviceName, sizeof(caps.deviceName));

    // -------------------------------------------------------------------------
    // 2. Feature chain (Vulkan 1.1+)
    //    subgroupProperties requires vkGetPhysicalDeviceProperties2 already done above
    // -------------------------------------------------------------------------
    VkPhysicalDeviceShaderFloat16Int8Features fp16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES };

    VkPhysicalDeviceDescriptorIndexingFeatures descIdx{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
    descIdx.pNext = &fp16;

    VkPhysicalDeviceFeatures2 feat2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    feat2.pNext = &descIdx;

    vkGetPhysicalDeviceFeatures2(physDev, &feat2);

    caps.astcHdrHW = CheckExtension(physDev, "VK_EXT_texture_compression_astc_hdr");
    if (caps.vendorID == 0x1002 || caps.vendorID == 0x8086 || caps.isSwiftShader)
        caps.astcHdrHW = false;

    caps.astcHdrHW   = CheckExtension(physDev, "VK_EXT_texture_compression_astc_hdr"); 
    caps.astcLdrHW   = feat2.features.textureCompressionASTC_LDR;
    caps.etc2Support = feat2.features.textureCompressionETC2; 
    caps.fp16Native  = fp16.shaderFloat16;
    caps.int8Support = fp16.shaderInt8;
    caps.shaderInt16 = feat2.features.shaderInt16;

    // Descriptor indexing: only mark tier2 if the key bindless features are present
    caps.descriptorTier2 = descIdx.shaderSampledImageArrayNonUniformIndexing &&
                           descIdx.runtimeDescriptorArray &&
                           descIdx.descriptorBindingVariableDescriptorCount;

    // -------------------------------------------------------------------------
    // 3. Subgroup support (core in Vulkan 1.1)
    // -------------------------------------------------------------------------
    VkPhysicalDeviceSubgroupProperties subgroupProps{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
    VkPhysicalDeviceProperties2 subgroupQuery{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    subgroupQuery.pNext = &subgroupProps;
    vkGetPhysicalDeviceProperties2(physDev, &subgroupQuery);

    const VkSubgroupFeatureFlags requiredOps =
        VK_SUBGROUP_FEATURE_BASIC_BIT |
        VK_SUBGROUP_FEATURE_VOTE_BIT  |
        VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
    caps.subgroupOps = (subgroupProps.subgroupSize >= 16) &&
                       ((subgroupProps.supportedOperations & requiredOps) == requiredOps);

    // -------------------------------------------------------------------------
    // 4. VRAM budget (largest DEVICE_LOCAL heap)
    // -------------------------------------------------------------------------
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);
    VkDeviceSize maxDeviceLocal = 0;
    for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
        if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            maxDeviceLocal = std::max(maxDeviceLocal, memProps.memoryHeaps[i].size);
    }
    caps.vramBudgetMB = static_cast<uint32_t>(maxDeviceLocal / (1024 * 1024));

    // -------------------------------------------------------------------------
    // 5. Per-vendor architectural derivation
    //    Branch on driverID where available (stable), fall back to vendorID.
    // -------------------------------------------------------------------------
    const uint32_t devID = props.deviceID;

    switch (caps.vendorID) {

    // --- NVIDIA (0x10DE) ----------------------------------------------------- 
    case 0x10DE: {
        // Kepler: roughly GK104/GK110 era
        // Fermi and older sit below this range; Maxwell starts at 0x1380
        caps.isKepler       = (devID >= 0x0FC0 && devID < 0x1380);
        caps.isMaxwell      = (devID >= 0x1380 && devID < 0x15C0) ||
                            (devID >= 0x17C0 && devID <= 0x17FF);
        caps.isPascal       = (devID >= 0x15C0 && devID < 0x1B80) ||
                            (devID >= 0x1B00 && devID < 0x1E00);
        caps.isTuringOrNewer = (devID >= 0x1E00); 
        // SER: NVIDIA Ada Lovelace (RTX 40xx) = device IDs 0x2600+
        // Confirmed via VK_NV_ray_tracing_invocation_reorder extension presence
        
        caps.hasSER = (devID >= 0x2600) && CheckExtension(physDev, "VK_NV_ray_tracing_invocation_reorder");

        caps.nvidiaNoASTCDriver = IsNvidiaDriverAbove(caps.driverVersion, 545, 84); 
        break;
    }

    // --- AMD (0x1002) --------------------------------------------------------
    case 0x1002: {
        // RDNA1 (Navi 10/12/14): 0x7310–0x7340
        // RDNA2 (Navi 21/22/23/24): 0x73A0–0x73FF
        // RDNA3 (Navi 31/32/33): 0x744C–0x747F
        // GCN everything below
        caps.isRDNA = (devID >= 0x7310);
        caps.isGCN  = !caps.isRDNA;

        // AMD desktop: no hardware ASTC or ETC2 
        caps.astcLdrHW   = false;
        caps.astcHdrHW   = false;
        // etc2Support left as reported by driver (RADV emulates it; AMDVLK may not)
        break;
    }

    // --- Intel (0x8086) ------------------------------------------------------
    case 0x8086: {
        // Gen9–Gen12 (Skylake 0x1900 → Alder Lake ~0x55FF): HW ASTC LDR supported
        // Arc Alchemist / Xe-HPG (DG2, 0x5600+): ASTC removed
        // Gen12 Xe LP (RKL 0x4C80, DG1 0x4905, ADL 0x4680): still has ASTC
        caps.isIntelGen9ToGen12 = (devID >= 0x1900 && devID < 0x5600);
        caps.isIntelXe          = (devID >= 0x4C80 && devID < 0x5600);
        caps.isIntelArc         = (devID >= 0x5600);

        // Arc and newer: ASTC dropped — override feature query
        // Gen9–Gen12: trust the driver report (ANV correctly exposes it)
        if (caps.isIntelArc)
            caps.astcLdrHW = false;

        // ETC2: ANV emulates on Gen9+ — leave as driver-reported
        break;
    }

    // --- ARM Mali (0x13B5) ---------------------------------------------------
    case 0x13B5: {
        caps.isMali = true;
        // Mali Bifrost (G31–G77) and Valhall (G57, G78, G710+) all support HW ASTC
        // astcLdrHW already set from feature query; sanity guard only
        // astcHdrHW: Valhall G710 and later — driverID or devID sniffing is brittle;
        // rely on the feature query result (VK_EXT_texture_compression_astc_hdr)
        break;
    }

    // --- Qualcomm Adreno (0x5143) --------------------------------------------
    case 0x5143: {
        caps.isAdreno = true;
        // Adreno 5xx+: HW ASTC LDR confirmed; HDR via extension on newer parts
        // astcLdrHW already set from feature query
        break;
    }

    default:
        break;
    }

    // -------------------------------------------------------------------------
    // 6. Software / CI renderer override
    // -------------------------------------------------------------------------
    if (caps.driverID == VK_DRIVER_ID_GOOGLE_SWIFTSHADER ||
        caps.driverID == VK_DRIVER_ID_MESA_LLVMPIPE) {
        caps.isSwiftShader = true;
        // Both report ETC2 support; neither has real ASTC decode
        caps.astcLdrHW = false;
    }

    return caps;
}

// -----------------------------------------------------------------------------
bool CapabilityOracle::IsNvidiaDriverAbove(uint32_t dv,
                                            uint32_t major, uint32_t minor) {
    // NVIDIA encodes: major<<22 | minor<<14 | patch
    const uint32_t dvMajor = dv >> 22;
    const uint32_t dvMinor = (dv >> 14) & 0xFF;
    return (dvMajor > major) || (dvMajor == major && dvMinor >= minor);
}

bool CapabilityOracle::CheckExtension(VkPhysicalDevice physDev, const char* name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physDev, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(physDev, nullptr, &count, exts.data());
    for (const auto& e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

} // namespace GoCL