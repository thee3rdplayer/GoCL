// GoCL Vulkan Proxy – injects cross‑generational optimisations into
// pre‑built Vulkan applications.
//
// This shared library (vulkan_proxy.so / vulkan_proxy.dll) is loaded by
// the Vulkan loader as an implicit layer (see VkLayer_GoCL_Proxy.json).
// It intercepts critical entry points and automatically:
//   - Patches SPIR‑V shaders to remove unsupported features
//   - Transcodes ASTC textures to ETC2 when hardware decode is absent
//   - Adjusts swap‑chain image count and present mode from GoCL.conf
//   - Splits large indirect draw calls into GPU‑friendly batches
//
// The layer negotiates with the loader via VkLayerInstanceCreateInfo /
// VkLayerDeviceCreateInfo to obtain the next GPA / GDPA in the chain.
// All non‑intercepted functions are forwarded through the next layer's
// dispatch table, preserving full loader compliance and layer stacking.

#include "core/CapabilityOracle.h"
#include "core/ConfigLoader.h"
#include "core/ConfigSingleton.h"
#include "render/LegacyShaderEmulator.h"
#include "render/VRAMBudgetTracker.h"
#include <vulkan/vulkan.h>
#include <astcenc.h>
#if defined(_WIN32) || defined(__MINGW32__)
#include <windows.h>
#ifdef DeviceCapabilities
#undef DeviceCapabilities
#endif
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef VK_INDIRECT_COMMANDS_TOKEN_TYPE_PIPELINE_EXT
#define VK_INDIRECT_COMMANDS_TOKEN_TYPE_PIPELINE_EXT ((VkIndirectCommandsTokenTypeEXT)1000428003)
#endif

using GoCL::GetConfig;

// ---------------------------------------------------------------------------
// Forward declarations of loader link structures (minimal set required).
// ---------------------------------------------------------------------------
struct VkLayerInstanceLink;
struct VkLayerDeviceLink;

// ---------------------------------------------------------------------------
// Per‑device function pointer caching structures.
// ---------------------------------------------------------------------------
struct InstanceDispatch {
    PFN_vkGetInstanceProcAddr gpa;
};
struct DeviceDispatch {
    PFN_vkGetDeviceProcAddr gdpa;
};

// ---------------------------------------------------------------------------
// Global state – all shared across the layer.
// ---------------------------------------------------------------------------
static std::unordered_map<VkInstance, InstanceDispatch> next_gpa;
static std::unordered_map<VkDevice,   DeviceDispatch>   next_gdpa;
static std::mutex g_nextGpaMutex;
static std::mutex g_nextGdpaMutex;

static PFN_vkCreateDevice g_realCreateDevice = nullptr;
static VkInstance g_capturedInstance = VK_NULL_HANDLE;
static std::mutex g_instanceMutex;

// Command‑buffer → current bound pipeline (for DGC)
static std::unordered_map<VkCommandBuffer, VkPipeline> g_cmdBufferPipelineMap;
static std::mutex g_cmdBufferPipelineMutex;

// Per‑command‑buffer pipeline index for DGC (maps cmdBuf -> uint32_t index)
static std::unordered_map<VkCommandBuffer, uint32_t> g_cmdBufferPipelineIndexMap;
static std::mutex g_cmdBufferPipelineIndexMutex;

// ---------------------------------------------------------------------------
// ProxyDGCManager – manages DGC objects for a single device.
// ---------------------------------------------------------------------------
class ProxyDGCManager {
public:
    ProxyDGCManager() = default;
    ~ProxyDGCManager() { destroy(); }

    ProxyDGCManager(const ProxyDGCManager&) = delete;
    ProxyDGCManager& operator=(const ProxyDGCManager&) = delete;
    ProxyDGCManager(ProxyDGCManager&&) = default;
    ProxyDGCManager& operator=(ProxyDGCManager&&) = default;

    bool init(VkDevice device, VkPhysicalDevice physDev, uint32_t maxSequences, bool indexed);
    void destroy();

    bool recordDrawRange(VkCommandBuffer cmdBuf, VkBuffer buffer, VkDeviceSize offset,
                         VkDeviceSize stride, uint32_t maxDrawCount, uint32_t pipelineIndex);
    uint32_t getPipelineIndex(VkPipeline pipeline);
    void execute(VkCommandBuffer cmdBuf, VkDeviceAddress countAddress = 0, uint32_t maxCount = 0);

private:
    uint32_t m_stride;  // 88 for indexed, 84 for non-indexed
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkBuffer& buffer, VkDeviceMemory& memory, bool hostVisible);

    // Function pointers for DGC extension – loaded dynamically.
    static PFN_vkCreateIndirectCommandsLayoutEXT  pfnCreateIndirectCommandsLayout;
    static PFN_vkCreateIndirectExecutionSetEXT   pfnCreateIndirectExecutionSet;
    static PFN_vkGetGeneratedCommandsMemoryRequirementsEXT pfnGetGeneratedCommandsMemReqs;
    static PFN_vkUpdateIndirectExecutionSetPipelineEXT pfnUpdateExecutionSetPipeline;
    static PFN_vkDestroyIndirectExecutionSetEXT pfnDestroyExecutionSet;
    static PFN_vkDestroyIndirectCommandsLayoutEXT pfnDestroyCommandsLayout;
    static PFN_vkCmdExecuteGeneratedCommandsEXT pfnCmdExecuteGeneratedCommands;

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physDev = VK_NULL_HANDLE;
    VkIndirectCommandsLayoutEXT m_layout = VK_NULL_HANDLE;
    VkIndirectExecutionSetEXT m_executionSet = VK_NULL_HANDLE;
    VkBuffer m_inputBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_inputMemory = VK_NULL_HANDLE;
    void* m_inputMapped = nullptr;
    VkDeviceSize m_inputSize = 0;
    VkDeviceAddress m_inputAddress = 0;
    VkBuffer m_preprocessBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_preprocessMemory = VK_NULL_HANDLE;
    VkDeviceSize m_preprocessSize = 0;
    VkDeviceAddress m_preprocessAddress = 0;
    uint32_t m_maxSequences = 0;
    uint32_t m_sequenceCount = 0;
    uint32_t m_maxPipelines = 32;
    std::unordered_map<VkPipeline, uint32_t> m_pipelineIndexMap;
    std::vector<VkPipeline> m_pipelines;
};

// Static member definitions
PFN_vkCreateIndirectCommandsLayoutEXT  ProxyDGCManager::pfnCreateIndirectCommandsLayout = nullptr;
PFN_vkCreateIndirectExecutionSetEXT   ProxyDGCManager::pfnCreateIndirectExecutionSet = nullptr;
PFN_vkGetGeneratedCommandsMemoryRequirementsEXT ProxyDGCManager::pfnGetGeneratedCommandsMemReqs = nullptr;
PFN_vkUpdateIndirectExecutionSetPipelineEXT ProxyDGCManager::pfnUpdateExecutionSetPipeline = nullptr;
PFN_vkDestroyIndirectExecutionSetEXT ProxyDGCManager::pfnDestroyExecutionSet = nullptr;
PFN_vkDestroyIndirectCommandsLayoutEXT ProxyDGCManager::pfnDestroyCommandsLayout = nullptr;
PFN_vkCmdExecuteGeneratedCommandsEXT ProxyDGCManager::pfnCmdExecuteGeneratedCommands = nullptr;

// ---------------------------------------------------------------------------
// ProxyDGCManager implementation
// ---------------------------------------------------------------------------
bool ProxyDGCManager::init(VkDevice device, VkPhysicalDevice physDev, uint32_t maxSequences, bool indexed) {
    m_device = device;
    m_physDev = physDev;
    m_maxSequences = maxSequences;
    m_stride = indexed ? 88 : 84;   // 4 + 64 + 20/16

    // Dynamically load DGC extension function pointers
    pfnCreateIndirectCommandsLayout = (PFN_vkCreateIndirectCommandsLayoutEXT)vkGetDeviceProcAddr(m_device, "vkCreateIndirectCommandsLayoutEXT");
    pfnCreateIndirectExecutionSet  = (PFN_vkCreateIndirectExecutionSetEXT) vkGetDeviceProcAddr(m_device, "vkCreateIndirectExecutionSetEXT");
    pfnGetGeneratedCommandsMemReqs = (PFN_vkGetGeneratedCommandsMemoryRequirementsEXT)vkGetDeviceProcAddr(m_device, "vkGetGeneratedCommandsMemoryRequirementsEXT");
    pfnUpdateExecutionSetPipeline  = (PFN_vkUpdateIndirectExecutionSetPipelineEXT)vkGetDeviceProcAddr(m_device, "vkUpdateIndirectExecutionSetPipelineEXT");
    pfnDestroyExecutionSet         = (PFN_vkDestroyIndirectExecutionSetEXT)vkGetDeviceProcAddr(m_device, "vkDestroyIndirectExecutionSetEXT");
    pfnDestroyCommandsLayout       = (PFN_vkDestroyIndirectCommandsLayoutEXT)vkGetDeviceProcAddr(m_device, "vkDestroyIndirectCommandsLayoutEXT");
    pfnCmdExecuteGeneratedCommands = (PFN_vkCmdExecuteGeneratedCommandsEXT)vkGetDeviceProcAddr(m_device, "vkCmdExecuteGeneratedCommandsEXT");

    // Abort if any DGC entry point failed to load
    if (!pfnCreateIndirectCommandsLayout || !pfnCreateIndirectExecutionSet ||
        !pfnGetGeneratedCommandsMemReqs    || !pfnUpdateExecutionSetPipeline ||
        !pfnDestroyExecutionSet            || !pfnDestroyCommandsLayout ||
        !pfnCmdExecuteGeneratedCommands) {
        fprintf(stderr, "ProxyDGCManager: DGC extension not available\n");
        return false;
    }

    // 1. Build tokens: pipeline, push constant, and the correct draw token
    VkIndirectCommandsLayoutTokenEXT pipelineToken{};
    pipelineToken.sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
    pipelineToken.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_PIPELINE_EXT;
    pipelineToken.offset = 0;

    VkIndirectCommandsPushConstantTokenEXT pushConstantInfo{};
    pushConstantInfo.updateRange = { VK_SHADER_STAGE_ALL_GRAPHICS, 4, 64 };

    VkIndirectCommandsLayoutTokenEXT pushToken{};
    pushToken.sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
    pushToken.pNext = &pushConstantInfo;
    pushToken.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT;
    pushToken.offset = 4;

    VkIndirectCommandsLayoutTokenEXT drawToken{};
    drawToken.sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
    drawToken.type = indexed ? VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_EXT
                             : VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT;
    drawToken.offset = 68;   // same offset; payload size differs

    std::vector<VkIndirectCommandsLayoutTokenEXT> tokens = { pipelineToken, pushToken, drawToken };

    VkResult r;
    VkIndirectCommandsLayoutCreateInfoEXT layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT;
    layoutCI.shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS;
    layoutCI.indirectStride = m_stride;
    layoutCI.tokenCount = static_cast<uint32_t>(tokens.size());
    layoutCI.pTokens = tokens.data();
    r = pfnCreateIndirectCommandsLayout(m_device, &layoutCI, nullptr, &m_layout);
    if (r != VK_SUCCESS) return false;

    VkIndirectExecutionSetPipelineInfoEXT pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_PIPELINE_INFO_EXT;
    pipelineInfo.maxPipelineCount = m_maxPipelines;
    VkIndirectExecutionSetCreateInfoEXT execCI{};
    execCI.sType = VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_CREATE_INFO_EXT;
    execCI.type = VK_INDIRECT_EXECUTION_SET_INFO_TYPE_PIPELINES_EXT;
    execCI.info.pPipelineInfo = &pipelineInfo;
    r = pfnCreateIndirectExecutionSet(m_device, &execCI, nullptr, &m_executionSet);
    if (r != VK_SUCCESS) return false;

    // ---- 2. Allocate input buffer with correct size
    VkDeviceSize inputSize = m_stride * maxSequences;
    if (!createBuffer(inputSize,
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                      VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      m_inputBuffer, m_inputMemory, true))
        return false;
    m_inputSize = inputSize;

    r = vkMapMemory(m_device, m_inputMemory, 0, VK_WHOLE_SIZE, 0, &m_inputMapped);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "ProxyDGCManager: vkMapMemory failed (%d)\n", r);
        return false;
    }

    // ---- 3. Query and allocate preprocess buffer ----
    VkGeneratedCommandsMemoryRequirementsInfoEXT memReqInfo{};
    memReqInfo.sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_EXT;
    memReqInfo.indirectCommandsLayout = m_layout;
    memReqInfo.indirectExecutionSet = m_executionSet;
    memReqInfo.maxSequenceCount = maxSequences;

    VkMemoryRequirements2 memReq2{};
    memReq2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    pfnGetGeneratedCommandsMemReqs(m_device, &memReqInfo, &memReq2);

    VkDeviceSize preprocessSize = memReq2.memoryRequirements.size;
    if (preprocessSize > 0) {
        // ---- Preprocess buffer – must use DGC-queried memory type bits ----
        VkBufferCreateInfo preBufCI{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        preBufCI.size  = preprocessSize;
        preBufCI.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                        VK_BUFFER_USAGE_2_PREPROCESS_BUFFER_BIT_EXT;
        preBufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkResult r = vkCreateBuffer(m_device, &preBufCI, nullptr, &m_preprocessBuffer);
        if (r != VK_SUCCESS) return false;

        VkMemoryRequirements bufMemReq;
        vkGetBufferMemoryRequirements(m_device, m_preprocessBuffer, &bufMemReq);

        // Intersect the buffer's type bits with those from the DGC query
        uint32_t allowedTypes = bufMemReq.memoryTypeBits & memReq2.memoryRequirements.memoryTypeBits;
        if (allowedTypes == 0) {
            // No compatible memory type; fallback handled by returning false
            vkDestroyBuffer(m_device, m_preprocessBuffer, nullptr);
            m_preprocessBuffer = VK_NULL_HANDLE;
            return false;
        }

        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(m_physDev, &memProps);

        uint32_t memType = UINT32_MAX;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((allowedTypes & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                memType = i;
                break;
            }
        }

        // Fallback: any allowed type if no DEVICE_LOCAL found
        if (memType == UINT32_MAX) {
            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
                if (allowedTypes & (1u << i)) {
                    memType = i;
                    break;
                }
            }
        }

        if (memType == UINT32_MAX) {
            vkDestroyBuffer(m_device, m_preprocessBuffer, nullptr);
            m_preprocessBuffer = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryAllocateInfo allocCI{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocCI.allocationSize  = bufMemReq.size;
        allocCI.memoryTypeIndex = memType;
        r = vkAllocateMemory(m_device, &allocCI, nullptr, &m_preprocessMemory);
        if (r != VK_SUCCESS) {
            vkDestroyBuffer(m_device, m_preprocessBuffer, nullptr);
            m_preprocessBuffer = VK_NULL_HANDLE;
            return false;
        }

        r = vkBindBufferMemory(m_device, m_preprocessBuffer, m_preprocessMemory, 0);
        if (r != VK_SUCCESS) {
            vkFreeMemory(m_device, m_preprocessMemory, nullptr);
            vkDestroyBuffer(m_device, m_preprocessBuffer, nullptr);
            m_preprocessBuffer = VK_NULL_HANDLE;
            m_preprocessMemory = VK_NULL_HANDLE;
            return false;
        }

        m_preprocessSize = preprocessSize;
    }

    // ---- 4. Retrieve device addresses ----
    VkBufferDeviceAddressInfo addrInfo{};
    addrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addrInfo.buffer = m_inputBuffer;
    m_inputAddress = vkGetBufferDeviceAddress(m_device, &addrInfo);

    if (m_preprocessBuffer) {
        addrInfo.buffer = m_preprocessBuffer;
        m_preprocessAddress = vkGetBufferDeviceAddress(m_device, &addrInfo);
    }

    return true;
}

void ProxyDGCManager::destroy() {
    if (m_inputMapped) {
        vkUnmapMemory(m_device, m_inputMemory);
        m_inputMapped = nullptr;
    }
    if (m_inputMemory) {
        vkFreeMemory(m_device, m_inputMemory, nullptr);
        m_inputMemory = VK_NULL_HANDLE;
    }
    if (m_inputBuffer) {
        vkDestroyBuffer(m_device, m_inputBuffer, nullptr);
        m_inputBuffer = VK_NULL_HANDLE;
    }
    if (m_preprocessMemory) {
        vkFreeMemory(m_device, m_preprocessMemory, nullptr);
        m_preprocessMemory = VK_NULL_HANDLE;
    }
    if (m_preprocessBuffer) {
        vkDestroyBuffer(m_device, m_preprocessBuffer, nullptr);
        m_preprocessBuffer = VK_NULL_HANDLE;
    }
    if (m_executionSet) {
        pfnDestroyExecutionSet(m_device, m_executionSet, nullptr);
        m_executionSet = VK_NULL_HANDLE;
    }
    if (m_layout) {
        pfnDestroyCommandsLayout(m_device, m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
    }
    m_device = VK_NULL_HANDLE;
    m_physDev = VK_NULL_HANDLE;
}

bool ProxyDGCManager::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                   VkBuffer& buffer, VkDeviceMemory& memory, bool hostVisible) {
    VkBufferCreateInfo bufCI{};
    bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCI.size = size;
    bufCI.usage = usage;
    bufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult r = vkCreateBuffer(m_device, &bufCI, nullptr, &buffer);
    if (r != VK_SUCCESS) return false;

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(m_device, buffer, &memReq);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_physDev, &memProps);
    uint32_t memType = UINT32_MAX;
    VkMemoryPropertyFlags required = hostVisible
        ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & required) == required) {
            memType = i;
            break;
        }
    }
    if (memType == UINT32_MAX) {
        vkDestroyBuffer(m_device, buffer, nullptr);
        return false;
    }

    VkMemoryAllocateInfo allocCI{};
    allocCI.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocCI.allocationSize = memReq.size;
    allocCI.memoryTypeIndex = memType;

    r = vkAllocateMemory(m_device, &allocCI, nullptr, &memory);
    if (r != VK_SUCCESS) {
        vkDestroyBuffer(m_device, buffer, nullptr);
        return false;
    }

    r = vkBindBufferMemory(m_device, buffer, memory, 0);
    if (r != VK_SUCCESS) {
        vkFreeMemory(m_device, memory, nullptr);
        vkDestroyBuffer(m_device, buffer, nullptr);
        return false;
    }
    return true;
}

bool ProxyDGCManager::recordDrawRange(VkCommandBuffer cmdBuf, VkBuffer buffer,
                                      VkDeviceSize offset, VkDeviceSize stride,
                                      uint32_t maxDrawCount, uint32_t pipelineIndex) {
    if (m_sequenceCount + maxDrawCount > m_maxSequences) {
        fprintf(stderr, "ProxyDGCManager: sequence limit reached\n");
        return false;
    }

    uint8_t* ptr = static_cast<uint8_t*>(m_inputMapped) + m_sequenceCount * m_stride;
    std::memcpy(ptr, &pipelineIndex, sizeof(uint32_t)); // offset 0

    VkDeviceSize dstBase = m_sequenceCount * m_stride;
    VkBufferCopy copyRegion{};
    copyRegion.srcOffset = offset;
    copyRegion.dstOffset = dstBase + 68;    // draw command offset
    copyRegion.size = maxDrawCount * stride;
    vkCmdCopyBuffer(cmdBuf, buffer, m_inputBuffer, 1, &copyRegion);

    m_sequenceCount += maxDrawCount;
    return true;
}

uint32_t ProxyDGCManager::getPipelineIndex(VkPipeline pipeline) {
    auto it = m_pipelineIndexMap.find(pipeline);
    if (it != m_pipelineIndexMap.end()) return it->second;
    if (m_pipelines.size() >= m_maxPipelines) {
        fprintf(stderr, "ProxyDGCManager: pipeline limit reached\n");
        return UINT32_MAX;
    }
    uint32_t idx = static_cast<uint32_t>(m_pipelines.size());
    m_pipelines.push_back(pipeline);
    m_pipelineIndexMap[pipeline] = idx;

    VkWriteIndirectExecutionSetPipelineEXT write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_INDIRECT_EXECUTION_SET_PIPELINE_EXT;
    write.index = idx;
    write.pipeline = pipeline;
    pfnUpdateExecutionSetPipeline(m_device, m_executionSet, 1, &write);
    return idx;
}

void ProxyDGCManager::execute(VkCommandBuffer cmdBuf, VkDeviceAddress countAddress, uint32_t maxCount) {
    if (m_sequenceCount == 0) return;

    // Ensure all previous copies to the input buffer are visible to the DGC engine
    VkBufferMemoryBarrier inputBarrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    inputBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    inputBarrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    inputBarrier.buffer = m_inputBuffer;
    inputBarrier.offset = 0;
    inputBarrier.size = VK_WHOLE_SIZE;

    vkCmdPipelineBarrier(cmdBuf,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                        0,
                        0, nullptr,
                        1, &inputBarrier,
                        0, nullptr);

    VkGeneratedCommandsInfoEXT genInfo{};
    genInfo.sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT;
    genInfo.indirectExecutionSet = m_executionSet;
    genInfo.indirectCommandsLayout = m_layout;
    genInfo.indirectAddress = m_inputAddress;
    genInfo.indirectAddressSize = m_sequenceCount * m_stride;
    genInfo.preprocessAddress = m_preprocessAddress;
    genInfo.preprocessSize = m_preprocessSize;
    if (countAddress != 0) {
        genInfo.sequenceCountAddress = countAddress;
        genInfo.maxSequenceCount = maxCount;
    } else {
        genInfo.maxSequenceCount = m_sequenceCount;
    }
    genInfo.shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS;

    pfnCmdExecuteGeneratedCommands(cmdBuf, VK_FALSE, &genInfo);
    m_sequenceCount = 0;
}

// ---------------------------------------------------------------------------
// Pipeline cache header
// ---------------------------------------------------------------------------
struct PipelineCacheHeader {
    uint32_t vendorID;
    uint32_t deviceID;
    uint32_t driverVersion;
    uint8_t  pipelineCacheUUID[VK_UUID_SIZE];
};

typedef uint32_t VkLayerFunction;

// ---------------------------------------------------------------------------
// Vulkan loader layer-negotiation structures
// ---------------------------------------------------------------------------
#ifndef VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO
#define VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO \
    static_cast<VkStructureType>(1000312000)
#endif
#ifndef VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
#define VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO \
    static_cast<VkStructureType>(1000312001)
#endif

#ifndef VK_LAYER_LINK_INFO
#define VK_LAYER_LINK_INFO (1u)
#endif

struct VkLayerInstanceLink {
    struct VkLayerInstanceLink* pNext;
    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
};

struct VkLayerInstanceCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkLayerFunction function;
    union {
        VkLayerInstanceLink* pLayerInfo;
        VkBool32 loaderInstance;
    } u;
};

struct VkLayerDeviceLink {
    struct VkLayerDeviceLink* pNext;
    PFN_vkGetDeviceProcAddr pfnNextGetDeviceProcAddr;
};

struct VkLayerDeviceCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkLayerFunction function;
    union {
        VkLayerDeviceLink* pLayerInfo;
        VkBool32 loaderDevice;
    } u;
};

// ---------------------------------------------------------------------------
// Device context – per‑device state and cached function pointers.
// ---------------------------------------------------------------------------
struct DeviceContext {
    GoCL::DeviceCapabilities  caps;
    GoCL::HardwareHint        hardwareHint;
    GoCL::RuntimeConfig       config;
    GoCL::VRAMBudgetTracker   vramTracker;
    DeviceContext() : vramTracker(VK_NULL_HANDLE, false) {}

    VkPhysicalDevice          physDev = VK_NULL_HANDLE;
    VkDevice                  device  = VK_NULL_HANDLE;
    PFN_vkDestroyDevice       fpDestroyDevice = nullptr;
    bool hasMemoryBudgetExt = false;
    bool dgcSupported       = false;
    std::unique_ptr<ProxyDGCManager> dgcManagerIndexed;
    std::unique_ptr<ProxyDGCManager> dgcManagerNonIndexed;
    std::unique_ptr<ProxyDGCManager> dgcManager;
    VkDeviceAddress pendingCountAddress = 0;
    uint32_t pendingMaxCount = 0;

    PFN_vkCreateBuffer                      fpCreateBuffer                      = nullptr;
    PFN_vkGetBufferMemoryRequirements       fpGetBufferMemoryRequirements       = nullptr;
    PFN_vkAllocateMemory                    fpAllocateMemory                    = nullptr;
    PFN_vkFreeMemory                        fpFreeMemory                        = nullptr;
    PFN_vkCreateShaderModule                fpCreateShaderModule                = nullptr;
    PFN_vkCreateGraphicsPipelines           fpCreateGraphicsPipelines           = nullptr;
    PFN_vkCreateComputePipelines            fpCreateComputePipelines            = nullptr;
    PFN_vkCreateSwapchainKHR                fpCreateSwapchainKHR                = nullptr;
    PFN_vkCreateImage                       fpCreateImage                       = nullptr;
    PFN_vkDestroyImage                      fpDestroyImage                      = nullptr;
    PFN_vkAllocateCommandBuffers            fpAllocateCommandBuffers            = nullptr;
    PFN_vkFreeCommandBuffers                fpFreeCommandBuffers                = nullptr;
    PFN_vkBindBufferMemory                  fpBindBufferMemory                  = nullptr;
    PFN_vkDestroyBuffer                     fpDestroyBuffer                     = nullptr;
    PFN_vkCmdCopyBufferToImage              fpCmdCopyBufferToImage              = nullptr;
    PFN_vkMapMemory                         fpMapMemory                         = nullptr;
    PFN_vkUnmapMemory                       fpUnmapMemory                       = nullptr;
    PFN_vkCmdDrawIndirect                   fpCmdDrawIndirect                   = nullptr;
    PFN_vkCmdDrawIndexedIndirect            fpCmdDrawIndexedIndirect            = nullptr;
    PFN_vkCmdDrawIndirectCount              fpCmdDrawIndirectCount              = nullptr;
    PFN_vkCmdDrawIndexedIndirectCount       fpCmdDrawIndexedIndirectCount       = nullptr;
    PFN_vkCmdBindPipeline                   fpCmdBindPipeline                   = nullptr;
    PFN_vkCmdExecuteGeneratedCommandsEXT    fpCmdExecuteGeneratedCommandsEXT    = nullptr;
    PFN_vkCmdPreprocessGeneratedCommandsEXT fpCmdPreprocessGeneratedCommandsEXT = nullptr;
    PFN_vkEndCommandBuffer                  fpEndCommandBuffer                  = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR fpGetSurfaceCaps              = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR fpGetPresentModes             = nullptr;
    PFN_vkFlushMappedMemoryRanges           fpFlushMappedMemoryRanges           = nullptr;
    PFN_vkCreatePipelineCache               fpCreatePipelineCache               = nullptr;
    PFN_vkDestroyPipelineCache              fpDestroyPipelineCache              = nullptr;
};

static std::unordered_map<VkDevice, DeviceContext> g_deviceContexts;
static std::mutex                                  g_deviceContextsMutex;

static DeviceContext* GetDeviceContext(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_deviceContextsMutex);
    auto it = g_deviceContexts.find(device);
    return (it != g_deviceContexts.end()) ? &it->second : nullptr;
}

// ---------------------------------------------------------------------------
// Additional shared globals.
// ---------------------------------------------------------------------------
struct ImageOverride {
    VkFormat originalFormat;
    VkFormat actualFormat;
    uint32_t blockW, blockH;
};
static std::unordered_map<VkImage, ImageOverride> g_imageOverrideMap;
static std::mutex                                 g_imageMutex;

static std::vector<uint8_t> g_decodeScratch;
static std::mutex           g_decodeScratchMutex;

static VkBuffer       g_stagingBuffer = VK_NULL_HANDLE;
static VkDeviceMemory g_stagingMemory = VK_NULL_HANDLE;
static VkDeviceSize   g_stagingSize   = 64ull * 1024 * 1024;
static bool g_skipAstcTranscode       = false;

static std::unordered_map<VkBuffer, VkDeviceMemory> g_bufferMemoryMap;
static std::mutex                                   g_bufferMemMutex;

static std::mutex g_stagingMutex;

static std::unordered_map<VkCommandBuffer, VkDevice> g_cmdBufferDeviceMap;
static std::mutex                                    g_cmdBufferMutex;

static VkDevice getDeviceFromCommandBuffer(VkCommandBuffer cmdBuf) {
    std::lock_guard<std::mutex> lock(g_cmdBufferMutex);
    auto it = g_cmdBufferDeviceMap.find(cmdBuf);
    return (it != g_cmdBufferDeviceMap.end()) ? it->second : VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Utility functions
// ---------------------------------------------------------------------------
static bool IsASTCFormat(VkFormat fmt) {
    switch (fmt) {
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:   case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:   case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:   case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:   case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:   case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:   case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:   case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:   case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:  case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:  case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:  case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK: case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK: case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK: case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
            return true;
        default: return false;
    }
}

static void ParseASTCBlockSize(VkFormat fmt, uint32_t& bw, uint32_t& bh) {
    switch (fmt) {
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:  case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:  bw=4; bh=4; break;
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:  case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:  bw=5; bh=4; break;
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:  case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:  bw=5; bh=5; break;
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:  case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:  bw=6; bh=5; break;
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:  case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:  bw=6; bh=6; break;
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:  case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:  bw=8; bh=5; break;
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:  case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:  bw=8; bh=6; break;
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:  case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:  bw=8; bh=8; break;
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK: case VK_FORMAT_ASTC_10x5_SRGB_BLOCK: bw=10; bh=5; break;
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK: case VK_FORMAT_ASTC_10x6_SRGB_BLOCK: bw=10; bh=6; break;
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK: case VK_FORMAT_ASTC_10x8_SRGB_BLOCK: bw=10; bh=8; break;
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:bw=10; bh=10;break;
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:bw=12; bh=10;break;
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:bw=12; bh=12;break;
        default: throw std::runtime_error("Unsupported ASTC format");
    }
}

static VkDeviceSize ASTCCompressedSize(uint32_t w, uint32_t h, uint32_t bw, uint32_t bh) {
    return ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16ull;
}
static VkDeviceSize ETC2CompressedSize(uint32_t w, uint32_t h) {
    return ((w + 3) / 4) * ((h + 3) / 4) * 16ull;
}

// ---------------------------------------------------------------------------
// Staging buffer management
// ---------------------------------------------------------------------------
static bool EnsureStagingBuffer(DeviceContext* ctx, VkDevice device, VkDeviceSize size) {
    std::lock_guard<std::mutex> lockStaging(g_stagingMutex);
    if (!ctx) return false;
    if (g_stagingBuffer != VK_NULL_HANDLE && g_stagingSize >= size) return true;

    if (g_stagingBuffer) {
        if (ctx->fpDestroyBuffer) ctx->fpDestroyBuffer(device, g_stagingBuffer, nullptr);
        if (ctx->fpFreeMemory)    ctx->fpFreeMemory(device, g_stagingMemory, nullptr);
        g_stagingBuffer = VK_NULL_HANDLE;
        g_stagingMemory = VK_NULL_HANDLE;
    }

    if (!ctx->fpCreateBuffer || !ctx->fpGetBufferMemoryRequirements ||
        !ctx->fpAllocateMemory || !ctx->fpBindBufferMemory)
        return false;

    VkBufferCreateInfo bufCI{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufCI.size = size;
    bufCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (ctx->fpCreateBuffer(device, &bufCI, nullptr, &g_stagingBuffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memReq{};
    ctx->fpGetBufferMemoryRequirements(device, g_stagingBuffer, &memReq);

    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(ctx->physDev, &memProps);

    uint32_t memIdx = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memIdx = i; break;
        }
    }
    if (memIdx == UINT32_MAX) {
        if (ctx->fpDestroyBuffer) ctx->fpDestroyBuffer(device, g_stagingBuffer, nullptr);
        g_stagingBuffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo allocCI{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocCI.allocationSize = memReq.size;
    allocCI.memoryTypeIndex = memIdx;
    if (ctx->fpAllocateMemory(device, &allocCI, nullptr, &g_stagingMemory) != VK_SUCCESS) {
        if (ctx->fpDestroyBuffer) ctx->fpDestroyBuffer(device, g_stagingBuffer, nullptr);
        g_stagingBuffer = VK_NULL_HANDLE;
        return false;
    }

    if (ctx->fpBindBufferMemory(device, g_stagingBuffer, g_stagingMemory, 0) != VK_SUCCESS) {
        if (ctx->fpDestroyBuffer) ctx->fpDestroyBuffer(device, g_stagingBuffer, nullptr);
        if (ctx->fpFreeMemory)    ctx->fpFreeMemory(device, g_stagingMemory, nullptr);
        g_stagingBuffer = VK_NULL_HANDLE;
        g_stagingMemory = VK_NULL_HANDLE;
        return false;
    }
    g_stagingSize = size;
    return true;
}

// ---------------------------------------------------------------------------
// Present mode helper
// ---------------------------------------------------------------------------
static VkPresentModeKHR ChoosePresentModeFromConfig(
    const std::string& preferred,
    const std::vector<VkPresentModeKHR>& available)
{
    VkPresentModeKHR target = VK_PRESENT_MODE_MAX_ENUM_KHR;
    if (preferred == "fifo")           target = VK_PRESENT_MODE_FIFO_KHR;
    else if (preferred == "fifo_relaxed")   target = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    else if (preferred == "mailbox")   target = VK_PRESENT_MODE_MAILBOX_KHR;
    else if (preferred == "immediate") target = VK_PRESENT_MODE_IMMEDIATE_KHR;

    if (target != VK_PRESENT_MODE_MAX_ENUM_KHR) {
        if (std::find(available.begin(), available.end(), target) != available.end())
            return target;
        fprintf(stderr, "GoCL Proxy: preferred present mode '%s' not supported, using default\n",
                preferred.c_str());
    }
    for (auto m : available)
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
    return VK_PRESENT_MODE_FIFO_KHR;
}

// ---------------------------------------------------------------------------
// ASTC→ETC2 transcoding (lazy load)
// ---------------------------------------------------------------------------
typedef int (*TranscodeFunc)(const uint8_t*, uint32_t, uint32_t, uint8_t**, size_t*);
static TranscodeFunc g_TranscodeFunc = nullptr;
static void*        g_TranscodeLib   = nullptr;

static bool EnsureTranscodeLib() {
    if (g_TranscodeFunc) return true;
#if defined(_WIN32) || defined(__MINGW32__)
    g_TranscodeLib = LoadLibraryA("gocl_transcoder.dll");
    if (!g_TranscodeLib) {
        fprintf(stderr, "GoCL Proxy: cannot load transcoder DLL, ASTC fallback disabled\n");
        return false;
    }
    g_TranscodeFunc = (TranscodeFunc)GetProcAddress((HMODULE)g_TranscodeLib, "GoCL_TranscodeToETC2");
#else
    g_TranscodeLib = dlopen("libgocl_transcoder.so", RTLD_NOW);
    if (!g_TranscodeLib) {
        fprintf(stderr, "GoCL Proxy: cannot load transcoder lib, ASTC fallback disabled\n");
        return false;
    }
    g_TranscodeFunc = (TranscodeFunc)dlsym(g_TranscodeLib, "GoCL_TranscodeToETC2");
#endif
    if (!g_TranscodeFunc) {
#if defined(_WIN32) || defined(__MINGW32__)
        FreeLibrary((HMODULE)g_TranscodeLib);
#else
        dlclose(g_TranscodeLib);
#endif
        g_TranscodeLib = nullptr;
        return false;
    }
    return true;
}

// ===================================================================
// Intercepted Vulkan functions
// ===================================================================

static VkResult InterceptedCreateShaderModule(
    VkDevice device,
    const VkShaderModuleCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkShaderModule* pShaderModule)
{
    DeviceContext* ctx = GetDeviceContext(device);
    VkShaderModuleCreateInfo modInfo = *pCreateInfo;
    std::vector<uint32_t> patchedSPV;

    if (ctx && pCreateInfo->codeSize % 4 == 0) {
        size_t wordCount = pCreateInfo->codeSize / 4;
        std::vector<uint32_t> spv((const uint32_t*)pCreateInfo->pCode,
                                  (const uint32_t*)pCreateInfo->pCode + wordCount);
        auto opts = GoCL::LegacyShaderEmulator::BuildOptions(ctx->caps);
        patchedSPV = GoCL::LegacyShaderEmulator::Process(std::move(spv), opts);
        if (GoCL::LegacyShaderEmulator::Validate(patchedSPV, ctx->caps)) {
            modInfo.pCode    = patchedSPV.data();
            modInfo.codeSize = patchedSPV.size() * 4;
        }
    }
    if (!ctx || !ctx->fpCreateShaderModule)
        return VK_ERROR_INITIALIZATION_FAILED;
    return ctx->fpCreateShaderModule(device, &modInfo, pAllocator, pShaderModule);
}

static VkResult InterceptedCreateGraphicsPipelines(
    VkDevice device, VkPipelineCache pipelineCache,
    uint32_t count, const VkGraphicsPipelineCreateInfo* pInfos,
    const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines)
{
    DeviceContext* ctx = GetDeviceContext(device);
    if (!ctx || !ctx->fpCreateGraphicsPipelines)
        return VK_ERROR_INITIALIZATION_FAILED;
    return ctx->fpCreateGraphicsPipelines(device, pipelineCache, count, pInfos, pAllocator, pPipelines);
}

static VkResult InterceptedCreateComputePipelines(
    VkDevice device, VkPipelineCache pipelineCache,
    uint32_t count, const VkComputePipelineCreateInfo* pInfos,
    const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines)
{
    DeviceContext* ctx = GetDeviceContext(device);
    if (!ctx || !ctx->fpCreateComputePipelines)
        return VK_ERROR_INITIALIZATION_FAILED;
    return ctx->fpCreateComputePipelines(device, pipelineCache, count, pInfos, pAllocator, pPipelines);
}

static VkResult InterceptedCreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{
    DeviceContext* ctx = GetDeviceContext(device);
    VkSwapchainCreateInfoKHR modInfo = *pCreateInfo;

    if (ctx && ctx->config.maxFramesInFlight > 0)
        modInfo.minImageCount = ctx->config.maxFramesInFlight;

    VkSurfaceCapabilitiesKHR caps{};
    bool haveCaps = false;
    if (ctx && ctx->fpGetSurfaceCaps) {
        VkResult r = ctx->fpGetSurfaceCaps(ctx->physDev, modInfo.surface, &caps);
        if (r == VK_SUCCESS) {
            haveCaps = true;
            modInfo.minImageCount = std::max(modInfo.minImageCount, caps.minImageCount);
            if (caps.maxImageCount > 0)
                modInfo.minImageCount = std::min(modInfo.minImageCount, caps.maxImageCount);

            ctx->vramTracker.Refresh(ctx->physDev, ctx->hasMemoryBudgetExt);
            uint32_t vramMB = ctx->vramTracker.RemainingMB();
            if (vramMB < 256) {
                uint32_t safe = std::max(2u, caps.minImageCount);
                if (caps.maxImageCount > 0) safe = std::min(safe, caps.maxImageCount);
                modInfo.minImageCount = safe;
            }
        }
    }

    if (ctx && ctx->config.dynamicResolutionScaling.enabled && haveCaps) {
        float scale = std::clamp(ctx->config.dynamicResolutionScaling.scale, 0.25f, 1.0f);
        ctx->vramTracker.Refresh(ctx->physDev, ctx->hasMemoryBudgetExt);
        if (ctx->vramTracker.RemainingMB() < 128) scale = std::min(scale, 0.5f);
        uint32_t w = std::clamp((uint32_t)(modInfo.imageExtent.width * scale),
                                caps.minImageExtent.width, caps.maxImageExtent.width);
        uint32_t h = std::clamp((uint32_t)(modInfo.imageExtent.height * scale),
                                caps.minImageExtent.height, caps.maxImageExtent.height);
        modInfo.imageExtent.width = w; modInfo.imageExtent.height = h;
    }

    if (ctx && !ctx->config.preferredPresentMode.empty() && ctx->fpGetPresentModes) {
        uint32_t modeCount = 0;
        ctx->fpGetPresentModes(ctx->physDev, modInfo.surface, &modeCount, nullptr);
        if (modeCount > 0) {
            std::vector<VkPresentModeKHR> modes(modeCount);
            ctx->fpGetPresentModes(ctx->physDev, modInfo.surface, &modeCount, modes.data());
            if (modeCount > 0)
                modInfo.presentMode = ChoosePresentModeFromConfig(ctx->config.preferredPresentMode, modes);
        }
    }

    if (!ctx || !ctx->fpCreateSwapchainKHR) {
        std::lock_guard<std::mutex> lock(g_nextGdpaMutex);
        auto it = next_gdpa.find(device);
        if (it != next_gdpa.end()) {
            auto fn = (PFN_vkCreateSwapchainKHR)it->second.gdpa(device, "vkCreateSwapchainKHR");
            if (fn) return fn(device, &modInfo, pAllocator, pSwapchain);
        }
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return ctx->fpCreateSwapchainKHR(device, &modInfo, pAllocator, pSwapchain);
}

static VkResult InterceptedCreateImage(
    VkDevice device, const VkImageCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkImage* pImage)
{
    DeviceContext* ctx = GetDeviceContext(device);
    if (!ctx || !ctx->fpCreateImage) return VK_ERROR_INITIALIZATION_FAILED;
    VkImageCreateInfo modInfo = *pCreateInfo;
    ImageOverride override{};
    if (!g_skipAstcTranscode && !ctx->caps.astcLdrHW && IsASTCFormat(pCreateInfo->format) && ctx->caps.etc2Support) {
        override.originalFormat = pCreateInfo->format;
        override.actualFormat = VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
        ParseASTCBlockSize(pCreateInfo->format, override.blockW, override.blockH);
        modInfo.format = override.actualFormat;
    }
    VkResult res = ctx->fpCreateImage(device, &modInfo, pAllocator, pImage);
    if (res == VK_SUCCESS && override.actualFormat != VK_FORMAT_UNDEFINED) {
        std::lock_guard<std::mutex> lock(g_imageMutex);
        g_imageOverrideMap[*pImage] = override;
    }
    return res;
}

static void InterceptedDestroyImage(VkDevice device, VkImage image,
                                    const VkAllocationCallbacks* pAllocator) {
    { std::lock_guard<std::mutex> lock(g_imageMutex); g_imageOverrideMap.erase(image); }
    DeviceContext* ctx = GetDeviceContext(device);
    if (ctx && ctx->fpDestroyImage) ctx->fpDestroyImage(device, image, pAllocator);
}

static VkResult InterceptedAllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkCommandBuffer* pCommandBuffers)
{
    DeviceContext* ctx = GetDeviceContext(device);
    if (!ctx || !ctx->fpAllocateCommandBuffers) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res = ctx->fpAllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);
    if (res == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_cmdBufferMutex);
        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i)
            g_cmdBufferDeviceMap[pCommandBuffers[i]] = device;
    }
    return res;
}

static void InterceptedFreeCommandBuffers(
    VkDevice device, VkCommandPool cmdPool, uint32_t count,
    const VkCommandBuffer* pCommandBuffers)
{
    { std::lock_guard<std::mutex> lock(g_cmdBufferMutex);
      for (uint32_t i = 0; i < count; ++i) g_cmdBufferDeviceMap.erase(pCommandBuffers[i]); }
    DeviceContext* ctx = GetDeviceContext(device);
    if (ctx && ctx->fpFreeCommandBuffers) ctx->fpFreeCommandBuffers(device, cmdPool, count, pCommandBuffers);
}

static VkResult InterceptedBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                                            VkDeviceSize offset) {
    DeviceContext* ctx = GetDeviceContext(device);
    if (!ctx || !ctx->fpBindBufferMemory) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res = ctx->fpBindBufferMemory(device, buffer, memory, offset);
    if (res == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_bufferMemMutex);
        g_bufferMemoryMap[buffer] = memory;
    }
    return res;
}

static void InterceptedDestroyBuffer(VkDevice device, VkBuffer buffer,
                                     const VkAllocationCallbacks* pAllocator) {
    { std::lock_guard<std::mutex> lock(g_bufferMemMutex); g_bufferMemoryMap.erase(buffer); }
    DeviceContext* ctx = GetDeviceContext(device);
    if (ctx && ctx->fpDestroyBuffer) ctx->fpDestroyBuffer(device, buffer, pAllocator);
}

// --- ASTC→ETC2 transcoding inside vkCmdCopyBufferToImage ---
static void InterceptedCmdCopyBufferToImage(
    VkCommandBuffer cmdBuf, VkBuffer srcBuffer, VkImage dstImage,
    VkImageLayout dstLayout, uint32_t regionCount, const VkBufferImageCopy* pRegions)
{
    VkDevice device = getDeviceFromCommandBuffer(cmdBuf);
    if (device == VK_NULL_HANDLE) return;
    if (g_skipAstcTranscode) {
        DeviceContext* ctx = GetDeviceContext(device);
        if (ctx && ctx->fpCmdCopyBufferToImage)
            ctx->fpCmdCopyBufferToImage(cmdBuf, srcBuffer, dstImage, dstLayout, regionCount, pRegions);
        return;
    }

    DeviceContext* ctx = GetDeviceContext(device);
    if (!ctx || !ctx->fpCmdCopyBufferToImage) return;

    ImageOverride overrideCopy{};
    bool imageOverridden = false;
    {
        std::lock_guard<std::mutex> lock(g_imageMutex);
        auto it = g_imageOverrideMap.find(dstImage);
        if (it != g_imageOverrideMap.end()) {
            overrideCopy = it->second;
            imageOverridden = true;
        }
    }
    if (!imageOverridden) {
        ctx->fpCmdCopyBufferToImage(cmdBuf, srcBuffer, dstImage, dstLayout, regionCount, pRegions);
        return;
    }
    if (!ctx->fpMapMemory || !ctx->fpUnmapMemory) {
        ctx->fpCmdCopyBufferToImage(cmdBuf, srcBuffer, dstImage, dstLayout, regionCount, pRegions);
        return;
    }

    for (uint32_t r = 0; r < regionCount; ++r) {
        VkBufferImageCopy region = pRegions[r];
        uint32_t w = region.imageExtent.width, h = region.imageExtent.height;
        if (w == 0 || h == 0) continue;

        VkDeviceSize astcSize = ASTCCompressedSize(w, h, overrideCopy.blockW, overrideCopy.blockH);
        VkDeviceSize rawSize = w * h * 4ull;

        VkDeviceMemory srcMemory = VK_NULL_HANDLE;
        { std::lock_guard<std::mutex> lock(g_bufferMemMutex);
          auto it = g_bufferMemoryMap.find(srcBuffer);
          if (it != g_bufferMemoryMap.end()) srcMemory = it->second; }
        if (srcMemory == VK_NULL_HANDLE) continue;

        void* srcData = nullptr;
        if (ctx->fpMapMemory(device, srcMemory, region.bufferOffset, astcSize, 0, &srcData) != VK_SUCCESS) continue;

        // ASTC decompress
        { std::lock_guard<std::mutex> lock(g_decodeScratchMutex);
          if (g_decodeScratch.size() < rawSize) g_decodeScratch.resize(rawSize);
          uint8_t* rawPixels = g_decodeScratch.data();
          astcenc_config cfg{};
          astcenc_config_init(ASTCENC_PRF_LDR, overrideCopy.blockW, overrideCopy.blockH, 1, ASTCENC_PRE_FAST, 0, &cfg);
          astcenc_context* astcCtx = nullptr;
          bool ok = false;
          if (astcenc_context_alloc(&cfg, 1, &astcCtx, 0) == ASTCENC_SUCCESS) {
              astcenc_image img{};
              img.dim_x = w; img.dim_y = h; img.dim_z = 1;
              img.data_type = ASTCENC_TYPE_U8;
              void* slice = rawPixels; img.data = &slice;
              astcenc_swizzle swz{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
              ok = (astcenc_decompress_image(astcCtx, (const uint8_t*)srcData, astcSize, &img, &swz, 0) == ASTCENC_SUCCESS);
              astcenc_context_free(astcCtx);
          }
          ctx->fpUnmapMemory(device, srcMemory);
          if (!ok) continue;

          // Transcode to ETC2
          if (!EnsureTranscodeLib()) continue;
          uint8_t* etc2Data = nullptr; size_t etc2Size = 0;
          if (!g_TranscodeFunc(rawPixels, w, h, &etc2Data, &etc2Size) || !etc2Data || etc2Size == 0) {
              free(etc2Data); continue;
          }
          if (etc2Size > 256ull * 1024 * 1024) { free(etc2Data); continue; }
          if (!EnsureStagingBuffer(ctx, device, etc2Size)) { free(etc2Data); continue; }

          void* stagingPtr = nullptr;
          if (ctx->fpMapMemory(device, g_stagingMemory, 0, etc2Size, 0, &stagingPtr) != VK_SUCCESS) {
              free(etc2Data); continue;
          }
          memcpy(stagingPtr, etc2Data, etc2Size);
          if (ctx->fpFlushMappedMemoryRanges) {
              VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, g_stagingMemory, 0, etc2Size};
              ctx->fpFlushMappedMemoryRanges(device, 1, &range);
          }
          ctx->fpUnmapMemory(device, g_stagingMemory);
          free(etc2Data);

          VkBufferImageCopy newRegion = region;
          newRegion.bufferOffset = 0;
          newRegion.bufferRowLength = 0;
          newRegion.bufferImageHeight = 0;
          ctx->fpCmdCopyBufferToImage(cmdBuf, g_stagingBuffer, dstImage, dstLayout, 1, &newRegion);
        }
    }
}

// --- vkCmdDrawIndirect (non-indexed) ---------------------------------------
static void InterceptedCmdDrawIndirect(
    VkCommandBuffer cmdBuf,
    VkBuffer buffer, VkDeviceSize offset,
    uint32_t drawCount, uint32_t stride)
{
    VkDevice device = getDeviceFromCommandBuffer(cmdBuf);
    DeviceContext* ctx = GetDeviceContext(device);
    if (!ctx) return;

    uint32_t pipelineIndex = 0;
    {
        std::lock_guard<std::mutex> lock(g_cmdBufferPipelineIndexMutex);
        auto it = g_cmdBufferPipelineIndexMap.find(cmdBuf);
        if (it != g_cmdBufferPipelineIndexMap.end())
            pipelineIndex = it->second;
    }

    if (ctx->dgcSupported && ctx->dgcManagerNonIndexed &&
        ctx->dgcManagerNonIndexed->recordDrawRange(cmdBuf, buffer, offset,
                                                   stride, drawCount, pipelineIndex))
        return;

    if (ctx->fpCmdDrawIndirect)
        ctx->fpCmdDrawIndirect(cmdBuf, buffer, offset, drawCount, stride);
}

// --- vkCmdDrawIndexedIndirect (indexed) ------------------------------------
static void InterceptedCmdDrawIndexedIndirect(
    VkCommandBuffer cmdBuf,
    VkBuffer buffer, VkDeviceSize offset,
    uint32_t drawCount, uint32_t stride)
{
    VkDevice device = getDeviceFromCommandBuffer(cmdBuf);
    DeviceContext* ctx = GetDeviceContext(device);
    if (!ctx) return;

    uint32_t pipelineIndex = 0;
    {
        std::lock_guard<std::mutex> lock(g_cmdBufferPipelineIndexMutex);
        auto it = g_cmdBufferPipelineIndexMap.find(cmdBuf);
        if (it != g_cmdBufferPipelineIndexMap.end())
            pipelineIndex = it->second;
    }

    if (ctx->dgcSupported && ctx->dgcManagerIndexed &&
        ctx->dgcManagerIndexed->recordDrawRange(cmdBuf, buffer, offset,
                                                stride, drawCount, pipelineIndex))
        return;

    if (ctx->fpCmdDrawIndexedIndirect)
        ctx->fpCmdDrawIndexedIndirect(cmdBuf, buffer, offset, drawCount, stride);
}

// --- vkCmdDrawIndirectCount (non-indexed with count buffer) ----------------
static void InterceptedCmdDrawIndirectCount(
    VkCommandBuffer cmdBuf,
    VkBuffer buffer, VkDeviceSize offset,
    VkBuffer countBuffer, VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount, uint32_t stride)
{
    VkDevice device = getDeviceFromCommandBuffer(cmdBuf);
    DeviceContext* ctx = GetDeviceContext(device);
    if (!ctx) return;

    uint32_t pipelineIndex = 0;
    {
        std::lock_guard<std::mutex> lock(g_cmdBufferPipelineIndexMutex);
        auto it = g_cmdBufferPipelineIndexMap.find(cmdBuf);
        if (it != g_cmdBufferPipelineIndexMap.end())
            pipelineIndex = it->second;
    }

    if (ctx->dgcSupported && ctx->dgcManagerNonIndexed &&
        ctx->dgcManagerNonIndexed->recordDrawRange(cmdBuf, buffer, offset,
                                                   stride, maxDrawCount, pipelineIndex))
    {
        VkBufferDeviceAddressInfo addrInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        addrInfo.buffer = countBuffer;
        VkDeviceAddress countAddr = vkGetBufferDeviceAddress(device, &addrInfo);
        if (countAddr) {
            ctx->pendingCountAddress = countAddr;
            ctx->pendingMaxCount = maxDrawCount;
            return;
        }
    }

    if (ctx->fpCmdDrawIndirectCount)
        ctx->fpCmdDrawIndirectCount(cmdBuf, buffer, offset,
                                    countBuffer, countBufferOffset,
                                    maxDrawCount, stride);
}

// --- vkCmdDrawIndexedIndirectCount (indexed with count buffer) -------------
static void InterceptedCmdDrawIndexedIndirectCount(
    VkCommandBuffer cmdBuf,
    VkBuffer buffer, VkDeviceSize offset,
    VkBuffer countBuffer, VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount, uint32_t stride)
{
    VkDevice device = getDeviceFromCommandBuffer(cmdBuf);
    DeviceContext* ctx = GetDeviceContext(device);
    if (!ctx) return;

    uint32_t pipelineIndex = 0;
    {
        std::lock_guard<std::mutex> lock(g_cmdBufferPipelineIndexMutex);
        auto it = g_cmdBufferPipelineIndexMap.find(cmdBuf);
        if (it != g_cmdBufferPipelineIndexMap.end())
            pipelineIndex = it->second;
    }

    if (ctx->dgcSupported && ctx->dgcManagerIndexed &&
        ctx->dgcManagerIndexed->recordDrawRange(cmdBuf, buffer, offset,
                                                stride, maxDrawCount, pipelineIndex))
    {
        VkBufferDeviceAddressInfo addrInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        addrInfo.buffer = countBuffer;
        VkDeviceAddress countAddr = vkGetBufferDeviceAddress(device, &addrInfo);
        if (countAddr) {
            ctx->pendingCountAddress = countAddr;
            ctx->pendingMaxCount = maxDrawCount;
            return;
        }
    }

    if (ctx->fpCmdDrawIndexedIndirectCount)
        ctx->fpCmdDrawIndexedIndirectCount(cmdBuf, buffer, offset,
                                           countBuffer, countBufferOffset,
                                           maxDrawCount, stride);
}

static void InterceptedCmdBindPipeline(
    VkCommandBuffer cmdBuf, VkPipelineBindPoint bindPoint, VkPipeline pipeline)
{
    if (bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        VkDevice device = getDeviceFromCommandBuffer(cmdBuf);
        DeviceContext* ctx = GetDeviceContext(device);
        if (ctx && ctx->dgcSupported && ctx->dgcManager) {
            uint32_t idx = ctx->dgcManager->getPipelineIndex(pipeline);
            if (idx != UINT32_MAX) {
                std::lock_guard<std::mutex> lock(g_cmdBufferPipelineIndexMutex);
                g_cmdBufferPipelineIndexMap[cmdBuf] = idx;
            }
        }
    }
    VkDevice device = getDeviceFromCommandBuffer(cmdBuf);
    DeviceContext* ctx = GetDeviceContext(device);
    if (ctx && ctx->fpCmdBindPipeline)
        ctx->fpCmdBindPipeline(cmdBuf, bindPoint, pipeline);
}

static VkResult InterceptedEndCommandBuffer(VkCommandBuffer cmdBuf) {
    VkDevice device = getDeviceFromCommandBuffer(cmdBuf);
    DeviceContext* ctx = GetDeviceContext(device);
    if (ctx) {
        if (ctx->dgcManagerIndexed)
            ctx->dgcManagerIndexed->execute(cmdBuf, ctx->pendingCountAddress, ctx->pendingMaxCount);
        if (ctx->dgcManagerNonIndexed)
            ctx->dgcManagerNonIndexed->execute(cmdBuf, ctx->pendingCountAddress, ctx->pendingMaxCount);
        ctx->pendingCountAddress = 0;
        ctx->pendingMaxCount = 0;
    }
    auto realFn = (PFN_vkEndCommandBuffer)vkGetDeviceProcAddr(device, "vkEndCommandBuffer");
    return realFn ? realFn(cmdBuf) : VK_ERROR_INITIALIZATION_FAILED;
}

static void InterceptedCmdExecuteGeneratedCommandsEXT(
    VkCommandBuffer cmdBuf, VkBool32 isPreprocessed,
    const VkGeneratedCommandsInfoEXT* pInfo)
{
    VkDevice device = getDeviceFromCommandBuffer(cmdBuf);
    DeviceContext* ctx = GetDeviceContext(device);
    if (ctx && ctx->fpCmdExecuteGeneratedCommandsEXT)
        ctx->fpCmdExecuteGeneratedCommandsEXT(cmdBuf, isPreprocessed, pInfo);
}

static void InterceptedCmdPreprocessGeneratedCommandsEXT(
    VkCommandBuffer cmdBuf, const VkGeneratedCommandsInfoEXT* pInfo,
    VkCommandBuffer stateCommandBuffer)
{
    VkDevice device = getDeviceFromCommandBuffer(cmdBuf);
    DeviceContext* ctx = GetDeviceContext(device);
    if (ctx && ctx->fpCmdPreprocessGeneratedCommandsEXT)
        ctx->fpCmdPreprocessGeneratedCommandsEXT(cmdBuf, pInfo, stateCommandBuffer);
}

// --- Pipeline cache interception ---
static VkResult InterceptedCreatePipelineCache(
    VkDevice device, const VkPipelineCacheCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkPipelineCache* pPipelineCache)
{
    DeviceContext* ctx = GetDeviceContext(device);
    if (!ctx || !ctx->fpCreatePipelineCache) return VK_ERROR_INITIALIZATION_FAILED;
    VkPipelineCacheCreateInfo modInfo = *pCreateInfo;
    if (pCreateInfo->initialDataSize == 0) {
        std::string path = ctx->config.pipelineCachePath;
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (f) {
            std::streamsize size = f.tellg(); f.seekg(0);
            std::vector<uint8_t> data(size);
            f.read(reinterpret_cast<char*>(data.data()), size);
            if (data.size() >= sizeof(PipelineCacheHeader)) {
                auto header = reinterpret_cast<const PipelineCacheHeader*>(data.data());
                if (header->vendorID == ctx->caps.vendorID &&
                    header->driverVersion == ctx->caps.driverVersion &&
                    memcmp(header->pipelineCacheUUID, ctx->caps.pipelineCacheUUID, VK_UUID_SIZE) == 0) {
                    modInfo.initialDataSize = data.size();
                    modInfo.pInitialData = data.data();
                }
            }
        }
    }
    return ctx->fpCreatePipelineCache(device, &modInfo, pAllocator, pPipelineCache);
}

static void InterceptedDestroyPipelineCache(
    VkDevice device, VkPipelineCache cache, const VkAllocationCallbacks* pAllocator)
{
    DeviceContext* ctx = GetDeviceContext(device);
    if (ctx && cache && !ctx->config.pipelineCachePath.empty()) {
        size_t dataSize = 0;
        vkGetPipelineCacheData(device, cache, &dataSize, nullptr);
        if (dataSize > 0) {
            std::vector<uint8_t> data(dataSize);
            vkGetPipelineCacheData(device, cache, &dataSize, data.data());
            PipelineCacheHeader header;
            header.vendorID = ctx->caps.vendorID;
            header.deviceID = ctx->caps.deviceID;
            header.driverVersion = ctx->caps.driverVersion;
            memcpy(header.pipelineCacheUUID, ctx->caps.pipelineCacheUUID, VK_UUID_SIZE);

            std::string tmp = ctx->config.pipelineCachePath + ".tmp";
            std::ofstream out(tmp, std::ios::binary);
            if (out) {
                out.write(reinterpret_cast<const char*>(&header), sizeof(header));
                out.write(reinterpret_cast<const char*>(data.data()), data.size());
                out.close();
                if (out.good()) std::filesystem::rename(tmp, ctx->config.pipelineCachePath);
            }
        }
    }
    if (ctx && ctx->fpDestroyPipelineCache) ctx->fpDestroyPipelineCache(device, cache, pAllocator);
}

// ---------------------------------------------------------------------------
// GetDeviceProcAddr interceptor
// ---------------------------------------------------------------------------
static PFN_vkVoidFunction InterceptedGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (!pName) return nullptr;

    if (strcmp(pName, "vkCreateShaderModule") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCreateShaderModule);
    if (strcmp(pName, "vkCreateGraphicsPipelines") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCreateGraphicsPipelines);
    if (strcmp(pName, "vkCreateComputePipelines") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCreateComputePipelines);
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCreateSwapchainKHR);
    if (strcmp(pName, "vkCreateImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCreateImage);
    if (strcmp(pName, "vkDestroyImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedDestroyImage);
    if (strcmp(pName, "vkCmdCopyBufferToImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCmdCopyBufferToImage);
    if (strcmp(pName, "vkAllocateCommandBuffers") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedAllocateCommandBuffers);
    if (strcmp(pName, "vkFreeCommandBuffers") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedFreeCommandBuffers);
    if (strcmp(pName, "vkBindBufferMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedBindBufferMemory);
    if (strcmp(pName, "vkDestroyBuffer") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedDestroyBuffer);
    if (strcmp(pName, "vkCmdDrawIndirect") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCmdDrawIndirect);
    if (strcmp(pName, "vkCmdDrawIndexedIndirect") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCmdDrawIndexedIndirect);
    if (strcmp(pName, "vkCmdDrawIndirectCount") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCmdDrawIndirectCount);
    if (strcmp(pName, "vkCmdDrawIndexedIndirectCount") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCmdDrawIndexedIndirectCount);
    if (strcmp(pName, "vkCmdBindPipeline") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCmdBindPipeline);
    if (strcmp(pName, "vkCmdExecuteGeneratedCommandsEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCmdExecuteGeneratedCommandsEXT);
    if (strcmp(pName, "vkCmdPreprocessGeneratedCommandsEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCmdPreprocessGeneratedCommandsEXT);
    if (strcmp(pName, "vkEndCommandBuffer") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedEndCommandBuffer);
    if (strcmp(pName, "vkCreatePipelineCache") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCreatePipelineCache);
    if (strcmp(pName, "vkDestroyPipelineCache") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedDestroyPipelineCache);

    std::lock_guard<std::mutex> lock(g_nextGdpaMutex);
    auto it = next_gdpa.find(device);
    return (it != next_gdpa.end()) ? it->second.gdpa(device, pName) : nullptr;
}

// ---------------------------------------------------------------------------
// Instance-level intercepts
// ---------------------------------------------------------------------------
static VkResult InterceptedCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance)
{
    const VkLayerInstanceCreateInfo* layerInfo = nullptr;
    for (auto p = (const VkBaseInStructure*)pCreateInfo->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
            layerInfo = (const VkLayerInstanceCreateInfo*)p;
            break;
        }
    }

    VkInstanceCreateInfo modInfo = *pCreateInfo;

    VkApplicationInfo appInfo;
    if (pCreateInfo->pApplicationInfo) {
        appInfo = *pCreateInfo->pApplicationInfo;
    } else {
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "VulkanApp";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "GoCL";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_0;
    }

    if (appInfo.apiVersion < VK_API_VERSION_1_1) {
        appInfo.apiVersion = VK_API_VERSION_1_1;
    }
    modInfo.pApplicationInfo = &appInfo;

    PFN_vkGetInstanceProcAddr next = nullptr;
    if (layerInfo && layerInfo->function == VK_LAYER_LINK_INFO) {
        next = layerInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        const_cast<VkLayerInstanceCreateInfo*>(layerInfo)->u.pLayerInfo =
            layerInfo->u.pLayerInfo->pNext;
        if (next)
            g_realCreateDevice = (PFN_vkCreateDevice)next(VK_NULL_HANDLE, "vkCreateDevice");
    }

    auto realCreate = next ? (PFN_vkCreateInstance)next(VK_NULL_HANDLE, "vkCreateInstance") : nullptr;
    VkResult res = realCreate ? realCreate(&modInfo, pAllocator, pInstance) : VK_ERROR_INITIALIZATION_FAILED;

    if (res == VK_SUCCESS && next) {
        std::lock_guard<std::mutex> lock(g_instanceMutex);
        g_capturedInstance = *pInstance;
        std::lock_guard<std::mutex> lockGpa(g_nextGpaMutex);
        next_gpa[*pInstance] = { next };
    }
    return res;
}

static void InterceptedDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    {
        std::lock_guard<std::mutex> lock(g_instanceMutex);
        if (g_capturedInstance == instance) g_capturedInstance = VK_NULL_HANDLE;
    }
    PFN_vkDestroyInstance realDestroy = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_nextGpaMutex);
        auto it = next_gpa.find(instance);
        if (it != next_gpa.end()) {
            realDestroy = (PFN_vkDestroyInstance)it->second.gpa(instance, "vkDestroyInstance");
            next_gpa.erase(instance);
        }
    }
    if (realDestroy) realDestroy(instance, pAllocator);
}

static VkResult InterceptedCreateDevice(
    VkPhysicalDevice physDev, const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkDevice* pDevice)
{
    const VkLayerDeviceCreateInfo* layerInfo = nullptr;
    for (auto p = (const VkBaseInStructure*)pCreateInfo->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            layerInfo = (const VkLayerDeviceCreateInfo*)p; break;
        }
    }
    PFN_vkGetDeviceProcAddr next = nullptr;
    if (layerInfo && layerInfo->function == VK_LAYER_LINK_INFO) {
        next = layerInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
        const_cast<VkLayerDeviceCreateInfo*>(layerInfo)->u.pLayerInfo = layerInfo->u.pLayerInfo->pNext;
    }
    if (!g_realCreateDevice) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res = g_realCreateDevice(physDev, pCreateInfo, pAllocator, pDevice);
    if (res != VK_SUCCESS) return res;

    if (next) {
        std::lock_guard<std::mutex> lock(g_nextGdpaMutex);
        next_gdpa[*pDevice] = { next };
    }

    DeviceContext& ctx = g_deviceContexts[*pDevice];
    ctx.device  = *pDevice;
    ctx.physDev = physDev;
    ctx.vramTracker = GoCL::VRAMBudgetTracker(physDev, ctx.hasMemoryBudgetExt);
    ctx.caps = GoCL::CapabilityOracle::Query(physDev);
    ctx.config = GetConfig();

    if (std::getenv("GOCL_SKIP_ASTC")) g_skipAstcTranscode = true;
    if (!g_skipAstcTranscode && ctx.config.skipAstcTranscode) g_skipAstcTranscode = true;

    if (ctx.config.stagingSizeMB > 0)
        g_stagingSize = static_cast<VkDeviceSize>(ctx.config.stagingSizeMB) * 1024ull * 1024ull;

    if (ctx.caps.dgcSupported && !ctx.config.disableDGC) {
        auto idxMgr = std::make_unique<ProxyDGCManager>();
        if (idxMgr->init(ctx.device, ctx.physDev, 4096, true)) {
            ctx.dgcManagerIndexed = std::move(idxMgr);
        }
        auto nonIdxMgr = std::make_unique<ProxyDGCManager>();
        if (nonIdxMgr->init(ctx.device, ctx.physDev, 4096, false)) {
            ctx.dgcManagerNonIndexed = std::move(nonIdxMgr);
        }
        ctx.dgcSupported = (ctx.dgcManagerIndexed || ctx.dgcManagerNonIndexed);
        if (ctx.dgcSupported) {
            fprintf(stderr, "GoCL Proxy: DGC enabled for this device\n");
        }
    }

    auto& gdpa = next_gdpa[*pDevice];
    ctx.fpCreateShaderModule          = (PFN_vkCreateShaderModule)         gdpa.gdpa(*pDevice, "vkCreateShaderModule");
    ctx.fpCreateGraphicsPipelines     = (PFN_vkCreateGraphicsPipelines)    gdpa.gdpa(*pDevice, "vkCreateGraphicsPipelines");
    ctx.fpCreateComputePipelines      = (PFN_vkCreateComputePipelines)     gdpa.gdpa(*pDevice, "vkCreateComputePipelines");
    ctx.fpCreateSwapchainKHR          = (PFN_vkCreateSwapchainKHR)         gdpa.gdpa(*pDevice, "vkCreateSwapchainKHR");
    ctx.fpCreateImage                 = (PFN_vkCreateImage)                gdpa.gdpa(*pDevice, "vkCreateImage");
    ctx.fpDestroyImage                = (PFN_vkDestroyImage)               gdpa.gdpa(*pDevice, "vkDestroyImage");
    ctx.fpAllocateCommandBuffers      = (PFN_vkAllocateCommandBuffers)     gdpa.gdpa(*pDevice, "vkAllocateCommandBuffers");
    ctx.fpFreeCommandBuffers          = (PFN_vkFreeCommandBuffers)         gdpa.gdpa(*pDevice, "vkFreeCommandBuffers");
    ctx.fpBindBufferMemory            = (PFN_vkBindBufferMemory)           gdpa.gdpa(*pDevice, "vkBindBufferMemory");
    ctx.fpDestroyBuffer               = (PFN_vkDestroyBuffer)              gdpa.gdpa(*pDevice, "vkDestroyBuffer");
    ctx.fpCmdCopyBufferToImage        = (PFN_vkCmdCopyBufferToImage)       gdpa.gdpa(*pDevice, "vkCmdCopyBufferToImage");
    ctx.fpMapMemory                   = (PFN_vkMapMemory)                  gdpa.gdpa(*pDevice, "vkMapMemory");
    ctx.fpUnmapMemory                 = (PFN_vkUnmapMemory)                gdpa.gdpa(*pDevice, "vkUnmapMemory");
    ctx.fpCmdDrawIndirect             = (PFN_vkCmdDrawIndirect)            gdpa.gdpa(*pDevice, "vkCmdDrawIndirect");
    ctx.fpCmdDrawIndexedIndirect      = (PFN_vkCmdDrawIndexedIndirect)     gdpa.gdpa(*pDevice, "vkCmdDrawIndexedIndirect");
    ctx.fpCmdDrawIndirectCount        = (PFN_vkCmdDrawIndirectCount)       gdpa.gdpa(*pDevice, "vkCmdDrawIndirectCount");
    ctx.fpCmdDrawIndexedIndirectCount = (PFN_vkCmdDrawIndexedIndirectCount)gdpa.gdpa(*pDevice, "vkCmdDrawIndexedIndirectCount");
    ctx.fpCmdBindPipeline             = (PFN_vkCmdBindPipeline)            gdpa.gdpa(*pDevice, "vkCmdBindPipeline");
    ctx.fpCmdExecuteGeneratedCommandsEXT    = (PFN_vkCmdExecuteGeneratedCommandsEXT)   gdpa.gdpa(*pDevice, "vkCmdExecuteGeneratedCommandsEXT");
    ctx.fpCmdPreprocessGeneratedCommandsEXT = (PFN_vkCmdPreprocessGeneratedCommandsEXT)gdpa.gdpa(*pDevice, "vkCmdPreprocessGeneratedCommandsEXT");
    ctx.fpEndCommandBuffer                  = (PFN_vkEndCommandBuffer)                 gdpa.gdpa(*pDevice, "vkEndCommandBuffer");
    ctx.fpGetSurfaceCaps              = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)gdpa.gdpa(*pDevice, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    ctx.fpGetPresentModes             = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)gdpa.gdpa(*pDevice, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    ctx.fpFlushMappedMemoryRanges     = (PFN_vkFlushMappedMemoryRanges)         gdpa.gdpa(*pDevice, "vkFlushMappedMemoryRanges");
    ctx.fpCreatePipelineCache         = (PFN_vkCreatePipelineCache)             gdpa.gdpa(*pDevice, "vkCreatePipelineCache");
    ctx.fpDestroyPipelineCache        = (PFN_vkDestroyPipelineCache)            gdpa.gdpa(*pDevice, "vkDestroyPipelineCache");
    ctx.fpCreateBuffer                = (PFN_vkCreateBuffer)                    gdpa.gdpa(*pDevice, "vkCreateBuffer");
    ctx.fpGetBufferMemoryRequirements = (PFN_vkGetBufferMemoryRequirements)     gdpa.gdpa(*pDevice, "vkGetBufferMemoryRequirements");
    ctx.fpAllocateMemory              = (PFN_vkAllocateMemory)                  gdpa.gdpa(*pDevice, "vkAllocateMemory");
    ctx.fpFreeMemory                  = (PFN_vkFreeMemory)                      gdpa.gdpa(*pDevice, "vkFreeMemory");

    return res;
}

static void InterceptedDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    {
        std::lock_guard<std::mutex> lock(g_deviceContextsMutex);
        auto it = g_deviceContexts.find(device);
        if (it != g_deviceContexts.end() && it->second.fpDestroyDevice)
            it->second.fpDestroyDevice(device, pAllocator);
        g_deviceContexts.erase(device);
    }
    {
        std::lock_guard<std::mutex> lock(g_nextGdpaMutex);
        next_gdpa.erase(device);
    }
}

// ---------------------------------------------------------------------------
// Mandatory entry point
// ---------------------------------------------------------------------------
extern "C" PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!pName) return nullptr;

    if (strcmp(pName, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCreateInstance);
    if (strcmp(pName, "vkDestroyInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedDestroyInstance);
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedGetDeviceProcAddr);
    if (strcmp(pName, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCreateDevice);
    if (strcmp(pName, "vkDestroyDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedDestroyDevice);

    if (instance != VK_NULL_HANDLE) {
        if (strcmp(pName, "vkCreateShaderModule") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCreateShaderModule);
        if (strcmp(pName, "vkCreateGraphicsPipelines") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCreateGraphicsPipelines);
        if (strcmp(pName, "vkCreateComputePipelines") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCreateComputePipelines);
        if (strcmp(pName, "vkCreateSwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCreateSwapchainKHR);
        if (strcmp(pName, "vkCreateImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCreateImage);
        if (strcmp(pName, "vkDestroyImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedDestroyImage);
        if (strcmp(pName, "vkCmdCopyBufferToImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCmdCopyBufferToImage);
        if (strcmp(pName, "vkAllocateCommandBuffers") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedAllocateCommandBuffers);
        if (strcmp(pName, "vkFreeCommandBuffers") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedFreeCommandBuffers);
        if (strcmp(pName, "vkBindBufferMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedBindBufferMemory);
        if (strcmp(pName, "vkDestroyBuffer") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedDestroyBuffer);
        if (strcmp(pName, "vkCmdDrawIndirect") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCmdDrawIndirect);
        if (strcmp(pName, "vkCmdDrawIndexedIndirect") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCmdDrawIndexedIndirect);
        if (strcmp(pName, "vkCmdDrawIndirectCount") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCmdDrawIndirectCount);
        if (strcmp(pName, "vkCmdDrawIndexedIndirectCount") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCmdDrawIndexedIndirectCount);
        if (strcmp(pName, "vkCmdBindPipeline") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCmdBindPipeline);
        if (strcmp(pName, "vkCmdExecuteGeneratedCommandsEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCmdExecuteGeneratedCommandsEXT);
        if (strcmp(pName, "vkCmdPreprocessGeneratedCommandsEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCmdPreprocessGeneratedCommandsEXT);
        if (strcmp(pName, "vkEndCommandBuffer") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedEndCommandBuffer);
        if (strcmp(pName, "vkCreatePipelineCache") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedCreatePipelineCache);
        if (strcmp(pName, "vkDestroyPipelineCache") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&InterceptedDestroyPipelineCache);
    }

    std::lock_guard<std::mutex> lock(g_nextGpaMutex);
    auto it = next_gpa.find(instance);
    return (it != next_gpa.end()) ? it->second.gpa(instance, pName) : nullptr;
}