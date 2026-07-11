#pragma once

#include "HardwareHint.h"
#include <vulkan/vulkan.h>

#include <cstdint>

namespace GoCL {

/**
 * @brief Snapshot of the physical device's capabilities, gathered once at startup.
 * 
 * Used by all subsystems (shader patching, texture compression, etc.) to adapt
 * to the actual hardware.
 */
struct DeviceCapabilities {
    // --- Texture compression -------------------------------------------------
    bool astcLdrHW         = false;  ///< Hardware ASTC LDR decode available.
    bool astcHdrHW         = false;  ///< Hardware ASTC HDR decode (rare; requires VK_EXT_texture_compression_astc_hdr).
    bool etc2Support       = false;  ///< ETC2, mandated by Vulkan spec for mobile devices, but queried for completeness.

    // --- Shader arithmetic features -----------------------------------------
    bool fp16Native        = false;  ///< VK_KHR_shader_float16_int8: shaderFloat16 support.
    bool int8Support       = false;  ///< VK_KHR_shader_float16_int8: shaderInt8 support.
    bool shaderInt16       = false;  ///< 16‑bit integer types in shaders (VK_KHR_16bit_storage / Vulkan 1.1+).
    bool subgroupOps       = false;  ///< Subgroup operations (ballot, shuffle, reductions) in compute shaders.

    // --- Storage access (buffer reads/writes with reduced bit widths) --------
    bool storage8BitAccess  = false; ///< 8‑bit integer access to storage buffers.
    bool storage16BitAccess = false; ///< 16‑bit integer access to storage buffers.

    // --- Descriptor indexing (bindless) features ----------------------------
    bool runtimeDescriptorArray                      = false;
    bool descriptorBindingPartiallyBound             = false;
    bool descriptorBindingVariableDescriptorCount    = false;
    bool shaderSampledImageArrayNonUniformIndexing   = false;
    bool shaderStorageBufferArrayNonUniformIndexing  = false;
    bool shaderStorageImageArrayNonUniformIndexing   = false;

    /// Combined bindless flag - true only when all essential descriptor‑indexing features are present.
    bool bindlessDescriptors = false;

    // --- Device limits ------------------------------------------------------
    uint32_t maxPerStageDescriptorSamplers = 0; ///< From VkPhysicalDeviceLimits.
    uint32_t maxImageDimension2D           = 0; ///< Maximum 2D image width/height.
    uint32_t vramBudgetMB                  = 0; ///< Total device‑local VRAM (static, not remaining).
    uint32_t maxPushConstantsSize          = 0; ///< Max push constant bytes.

    // --- Versioning ---------------------------------------------------------
    uint32_t apiVersion    = 0; ///< Vulkan API version supported by the device.
    uint32_t driverVersion = 0; ///< Vendor‑specific driver version.
    uint32_t maxSpvVersion = 0; ///< Highest SPIR‑V version the device can consume.

    // --- Hardware identity --------------------------------------------------
    uint32_t  vendorID = 0;   ///< PCI vendor ID (0x10DE = NVIDIA, 0x1002 = AMD, 0x8086 = Intel, …).
    uint32_t  deviceID = 0;   ///< Device ID (used for pipeline cache invalidation).
    VkDriverId driverID = VK_DRIVER_ID_MAX_ENUM;
    bool nvidiaNoASTCDriver = false;                    ///< True on NVIDIA drivers that lack ASTC decode support.
    char deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};
    uint8_t pipelineCacheUUID[VK_UUID_SIZE];            ///< From VkPhysicalDeviceProperties::pipelineCacheUUID.

    // --- Software renderer detection ----------------------------------------
    bool isSwiftShader = false; ///< True for SwiftShader, llvmpipe, etc.

    // --- Optional extension capabilities ------------------------------------
    bool hasRayTracingPipeline  = false; ///< VK_KHR_ray_tracing_pipeline.
    bool hasMeshShaders         = false; ///< VK_EXT_mesh_shader.
    bool hasFragmentShadingRate = false; ///< VK_KHR_fragment_shading_rate.
    bool hasRayQuery            = false; ///< VK_KHR_ray_query.
    bool hasShaderModuleIdentifier = false; ///< VK_EXT_shader_module_identifier.
    bool dgcSupported           = false; ///< VK_EXT_device_generated_commands.
    bool timelineSemaphoreEnabled = false; ///< VK_KHR_timeline_semaphore.

    // --- Hardware empathy hints (coarse) -----------------------------------
    HardwareHint hardwareHint = {};
};

/**
 * @brief Capability oracle for querying and caching Vulkan device capabilities.
 */
class CapabilityOracle {
public:
    /**
     * @brief Queries the device and returns a fully populated DeviceCapabilities.
     * 
     * @param physDev Physical device to query.
     * @return Fully populated DeviceCapabilities structure.
     */
    static DeviceCapabilities Query(VkPhysicalDevice physDev);

    /**
     * @brief Converts Vulkan API version to the highest SPIR‑V version the device can likely handle.
     * 
     * @param apiVersion Vulkan API version.
     * @return SPIR‑V version as a 32-bit integer.
     */
    static uint32_t SpvVersionFromVk(uint32_t apiVersion);

    /**
     * @brief Checks whether the device supports a specific extension.
     * 
     * @param physDev Physical device to query.
     * @param name Extension name to check.
     * @return true if the extension is supported, false otherwise.
     */
    static bool CheckExtension(VkPhysicalDevice physDev, const char* name);
};

}   // namespace GoCL