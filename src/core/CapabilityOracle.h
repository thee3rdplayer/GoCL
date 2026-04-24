#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>


namespace GoCL {

struct DeviceCapabilities {
    // -------------------------------------------------------------------------
    // Texture compression
    // -------------------------------------------------------------------------
    bool astcLdrHW        = false;  // hardware ASTC LDR (Mali, Adreno, Apple)
    bool astcHdrHW        = false;  // hardware ASTC HDR (rare — Mali-G710+)
    bool etc2Support      = false;  // Vulkan spec mandates for all conformant drivers.
                                    // HW on Mali/Adreno; driver-emulated on NVIDIA/AMD/Intel. 

    // -------------------------------------------------------------------------
    // Shader features
    // -------------------------------------------------------------------------
    bool fp16Native       = false;  // VK_KHR_shader_float16_int8: shaderFloat16
    bool int8Support      = false;  // VK_KHR_shader_float16_int8: shaderInt8
    bool shaderInt16      = false;
    bool subgroupOps      = false;  // wave/subgroup size >= 16, basic ops supported

    // -------------------------------------------------------------------------
    // API tier
    // -------------------------------------------------------------------------
    bool descriptorTier2  = false;  // VK_EXT_descriptor_indexing (bindless)
    uint32_t maxDescriptors = 0;    // VkPhysicalDeviceLimits::maxPerStageDescriptorSamplers

    // -------------------------------------------------------------------------
    // Device limits (used by ASTCBlockSelector and tests)
    // -------------------------------------------------------------------------
    uint32_t maxImageDimension2D  = 0;
    uint32_t vramBudgetMB         = 0;   // approximate; from VkPhysicalDeviceMemoryProperties
    uint32_t maxPushConstantsSize = 0;   // VkPhysicalDeviceLimits — min 128 bytes per spec

    // -------------------------------------------------------------------------
    // Versioning
    // -------------------------------------------------------------------------
    uint32_t apiVersion    = 0;     // VK_MAKE_VERSION(major, minor, patch)
    uint32_t driverVersion = 0;     // vendor-encoded
    uint32_t maxSpvVersion = 0;     // SPIR-V version ceiling: 0x00MMNN00
                                    // Derived from apiVersion — see SpvVersionFromVk()

    // -------------------------------------------------------------------------
    // Hardware identity
    // -------------------------------------------------------------------------
    uint32_t  vendorID   = 0;       // 0x10DE NVIDIA | 0x1002 AMD | 0x8086 Intel
                                    // 0x13B5 ARM    | 0x5143 Qualcomm
    VkDriverId driverID  = VK_DRIVER_ID_MAX_ENUM;  // stable across driver updates
    char deviceName[256] = {};

    // -------------------------------------------------------------------------
    // Derived architectural flags
    // Set by CapabilityOracle::Query() — do not write elsewhere.
    // -------------------------------------------------------------------------

    // NVIDIA
    bool isKepler           = false;  // GK1xx: older than Maxwell
    bool isMaxwell          = false;  // GM1xx/GM2xx: no HW ASTC, no FP16 native
    bool isPascal           = false;  // GP1xx: FP16 native, no HW ASTC
    bool isTuringOrNewer    = false;  // TU1xx+: tensor cores, HW mesh shaders

    bool hasSER             = false; // Shader Execution Reordering — NVIDIA Ada (RTX 40xx) only
    
    bool nvidiaNoASTCDriver = false; // driver >= 545.84 dropped SW ASTC on desktop
   
    // AMD
    bool isGCN            = false;  // pre-RDNA (GCN1–GCN5): no ASTC, limited FP16
    bool isRDNA           = false;  // RDNA1/2/3: FP16 wave32, still no ASTC desktop

    // Intel
    bool isIntelGen9ToGen12 = false;  // Skylake–Tiger Lake: HW ASTC LDR present
    bool isIntelXe          = false;  // Gen12 Xe (RKL/ADL): HW ASTC LDR present  
    bool isIntelArc         = false;  // Arc Alchemist (DG2, 0x5600+): ASTC removed

    // Mobile / console-like
    bool isMali           = false;  // ARM Mali (Bifrost/Valhall): HW ASTC
    bool isAdreno         = false;  // Qualcomm Adreno: HW ASTC

    // CI / software
    bool isSwiftShader    = false;  // Google SwiftShader / Mesa LLVMpipe
};

class CapabilityOracle {
public:
    static DeviceCapabilities Query(VkPhysicalDevice physDev);

    // Exposed for unit testing
    static uint32_t SpvVersionFromVk(uint32_t apiVersion);
    static bool     IsNvidiaDriverAbove(uint32_t driverVersion,
                                        uint32_t major, uint32_t minor);
    static bool CheckExtension(VkPhysicalDevice physDev, const char* name);                                    
};

} // namespace GoCL