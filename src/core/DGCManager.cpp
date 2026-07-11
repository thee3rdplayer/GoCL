#include "DGCManager.h"
#include "GoCLContext.h"

#include <cstdio>
#include <vector>

namespace GoCL {

// ---------- Extension function pointers ----------
// Loaded once from the device to avoid linker dependency on extension symbols.
// Must be populated before any DGC operation.
static PFN_vkCreateIndirectCommandsLayoutEXT      pfnCreateIndirectCommandsLayout = nullptr;
static PFN_vkCreateIndirectExecutionSetEXT        pfnCreateIndirectExecutionSet = nullptr;
static PFN_vkGetGeneratedCommandsMemoryRequirementsEXT pfnGetGeneratedCommandsMemReqs = nullptr;
static PFN_vkUpdateIndirectExecutionSetPipelineEXT      pfnUpdateExecutionSetPipeline = nullptr;
static PFN_vkDestroyIndirectExecutionSetEXT       pfnDestroyExecutionSet = nullptr;
static PFN_vkDestroyIndirectCommandsLayoutEXT     pfnDestroyCommandsLayout = nullptr;
static PFN_vkCmdExecuteGeneratedCommandsEXT       pfnCmdExecuteGeneratedCommands = nullptr;

bool DGCManager::init(GoCLContext& ctx) {
    m_device = ctx.device;
    m_physDev = ctx.physDev;

    // Load all DGC extension functions via device proc address.
    pfnCreateIndirectCommandsLayout = (PFN_vkCreateIndirectCommandsLayoutEXT)vkGetDeviceProcAddr(m_device, "vkCreateIndirectCommandsLayoutEXT");
    pfnCreateIndirectExecutionSet   = (PFN_vkCreateIndirectExecutionSetEXT)vkGetDeviceProcAddr(m_device, "vkCreateIndirectExecutionSetEXT");
    pfnGetGeneratedCommandsMemReqs  = (PFN_vkGetGeneratedCommandsMemoryRequirementsEXT)vkGetDeviceProcAddr(m_device, "vkGetGeneratedCommandsMemoryRequirementsEXT");
    pfnUpdateExecutionSetPipeline   = (PFN_vkUpdateIndirectExecutionSetPipelineEXT)vkGetDeviceProcAddr(m_device, "vkUpdateIndirectExecutionSetPipelineEXT");
    pfnDestroyExecutionSet          = (PFN_vkDestroyIndirectExecutionSetEXT)vkGetDeviceProcAddr(m_device, "vkDestroyIndirectExecutionSetEXT");
    pfnDestroyCommandsLayout        = (PFN_vkDestroyIndirectCommandsLayoutEXT)vkGetDeviceProcAddr(m_device, "vkDestroyIndirectCommandsLayoutEXT");
    pfnCmdExecuteGeneratedCommands  = (PFN_vkCmdExecuteGeneratedCommandsEXT)vkGetDeviceProcAddr(m_device, "vkCmdExecuteGeneratedCommandsEXT");

    // Verify all required DGC extension entry points were loaded successfully.
    // If any function pointer is null, the device does not support the extension
    // at the driver level, and DGC cannot be used.
    if (!pfnCreateIndirectCommandsLayout || !pfnCreateIndirectExecutionSet ||
        !pfnGetGeneratedCommandsMemReqs    || !pfnUpdateExecutionSetPipeline ||
        !pfnDestroyExecutionSet            || !pfnDestroyCommandsLayout ||
        !pfnCmdExecuteGeneratedCommands) {
        fprintf(stderr, "DGCManager: DGC extension not available\n");
        return false;
    }

    if (!ctx.caps.dgcSupported) {
        fprintf(stderr, "DGCManager: device does not support VK_EXT_device_generated_commands\n");
        return false;
    }

    // Define the token sequence.
    // We use: PUSH_CONSTANT (offset 0, size 128) + DRAW_INDEXED (action token, offset 128)
    // Both offsets and the push constant size must match the stream layout.
    // The stride per sequence is exactly the push constant size (128 bytes) – no extra data.
    std::vector<VkIndirectCommandsLayoutTokenEXT> tokens;

    // Token-specific info for push constants
    VkIndirectCommandsPushConstantTokenEXT pushConstantInfo{};
    pushConstantInfo.updateRange = { VK_SHADER_STAGE_ALL_GRAPHICS, 0, 128 };

    // Push constant token
    VkIndirectCommandsLayoutTokenEXT pushToken{};
    pushToken.sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
    pushToken.pNext = &pushConstantInfo;
    pushToken.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT;
    pushToken.offset = 0;   // Offset in the sequence data
    tokens.push_back(pushToken);

    // Action token – must be last.
    VkIndirectCommandsLayoutTokenEXT drawToken{};
    drawToken.sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
    drawToken.pNext = nullptr;  // no extra info
    drawToken.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_EXT;
    drawToken.offset = 128; // after push constants
    tokens.push_back(drawToken);

    VkIndirectCommandsLayoutCreateInfoEXT layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT;
    layoutCI.pNext = nullptr;
    layoutCI.flags = 0;
    layoutCI.shaderStages = VK_SHADER_STAGE_ALL_GRAPHICS;
    layoutCI.indirectStride = 128;  // stride = push constant size (per sequence)
    layoutCI.tokenCount = static_cast<uint32_t>(tokens.size());
    layoutCI.pTokens = tokens.data();

    VkResult r = pfnCreateIndirectCommandsLayout(
        m_device,
        &layoutCI,
        nullptr,
        &m_layout
    );

    if (r != VK_SUCCESS) {
        fprintf(stderr, "DGCManager: vkCreateIndirectCommandsLayoutEXT failed (%d)\n", r);
        return false;
    }

    return true;
}

bool DGCManager::initExecutionSet(uint32_t maxPipelines) {
    if (!m_layout) {
        fprintf(stderr, "DGCManager: cannot create execution set without layout\n");
        return false;
    }

    m_maxPipelines = maxPipelines;

    VkIndirectExecutionSetPipelineInfoEXT pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_PIPELINE_INFO_EXT;
    pipelineInfo.maxPipelineCount = maxPipelines;
    pipelineInfo.initialPipeline = VK_NULL_HANDLE;  // or a valid pipeline handle

    VkIndirectExecutionSetCreateInfoEXT execCI{};
    execCI.sType = VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_CREATE_INFO_EXT;
    execCI.type = VK_INDIRECT_EXECUTION_SET_INFO_TYPE_PIPELINES_EXT;
    execCI.info.pPipelineInfo = &pipelineInfo;

    VkResult r = pfnCreateIndirectExecutionSet(
        m_device,
        &execCI,
        nullptr,
        &m_executionSet
    );

    if (r != VK_SUCCESS) {
        fprintf(stderr, "DGCManager: vkCreateIndirectExecutionSetEXT failed (%d)\n", r);
        return false;
    }

    return true;
}

bool DGCManager::initBuffers(uint32_t maxSequences, uint32_t pushConstantSize) {
    m_maxSequences = maxSequences;
    m_pushConstantSize = pushConstantSize;
    m_currentSequenceCount = 0;

    // The stride per sequence is exactly the push constant size.
    VkDeviceSize perSequenceSize = pushConstantSize;
    VkDeviceSize inputBufferSize = perSequenceSize * maxSequences;

    if (!createBuffer(inputBufferSize,
                      VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,   // storage for the input stream
                      m_inputBuffer, m_inputMemory)) {
        return false;
    }

    m_inputSize = inputBufferSize;

    // Map the input buffer – must be HOST_VISIBLE (we'll handle memory type in createBuffer)
    VkResult r = vkMapMemory(m_device, m_inputMemory, 0, VK_WHOLE_SIZE, 0, &m_inputMapped);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "DGCManager: vkMapMemory for input buffer failed (%d)\n", r);
        return false;
    }

    // Query preprocess buffer size.
    VkGeneratedCommandsMemoryRequirementsInfoEXT memReqInfo{};
    memReqInfo.sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_EXT;
    memReqInfo.pNext = nullptr;
    memReqInfo.indirectCommandsLayout = m_layout;
    memReqInfo.indirectExecutionSet = m_executionSet;
    memReqInfo.maxSequenceCount = maxSequences;

    VkMemoryRequirements2 memReq2{};
    memReq2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;

    pfnGetGeneratedCommandsMemReqs(m_device, &memReqInfo, &memReq2);

    VkDeviceSize preprocessSize = memReq2.memoryRequirements.size;
    if (preprocessSize > 0) {
        if (!createBuffer(preprocessSize,
                          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          m_preprocessBuffer, m_preprocessMemory)) {
            return false;
        }
        m_preprocessSize = preprocessSize;
    }

    // After buffer creation and mapping, retrieve device addresses
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

bool DGCManager::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                              VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo bufCI{};
    bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCI.size = size;
    bufCI.usage = usage;
    bufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult r = vkCreateBuffer(m_device, &bufCI, nullptr, &buffer);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "DGCManager: vkCreateBuffer failed (%d)\n", r);
        return false;
    }

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(m_device, buffer, &memReq);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_physDev, &memProps);
    uint32_t memType = UINT32_MAX;

    // For preprocess buffer, prefer DEVICE_LOCAL.
    VkMemoryPropertyFlags preferredFlags;
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        // Preprocess buffer – likely device-local
        preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    } else {
        // Input buffer – must be mapped
        preferredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & preferredFlags) == preferredFlags) {
            memType = i;
            break;
        }
    }

    // Fallback: if no exact match, accept any DEVICE_LOCAL (for preprocess)
    if (memType == UINT32_MAX && (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)) {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((memReq.memoryTypeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                memType = i;
                break;
            }
        }
    }

    // Fallback: for input, accept any HOST_VISIBLE
    if (memType == UINT32_MAX && (usage & (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT))) {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((memReq.memoryTypeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                memType = i;
                break;
            }
        }
    }

    if (memType == UINT32_MAX) {
        fprintf(stderr, "DGCManager: no suitable memory type found\n");
        vkDestroyBuffer(m_device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo allocCI{};
    allocCI.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocCI.allocationSize = memReq.size;
    allocCI.memoryTypeIndex = memType;

    r = vkAllocateMemory(m_device, &allocCI, nullptr, &memory);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "DGCManager: vkAllocateMemory failed (%d)\n", r);
        vkDestroyBuffer(m_device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    r = vkBindBufferMemory(m_device, buffer, memory, 0);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "DGCManager: vkBindBufferMemory failed (%d)\n", r);
        vkFreeMemory(m_device, memory, nullptr);
        vkDestroyBuffer(m_device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        return false;
    }

    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo addrInfo{};
        addrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addrInfo.buffer = buffer;
        VkDeviceAddress addr = vkGetBufferDeviceAddress(m_device, &addrInfo);
        if (addr == 0) {
            fprintf(stderr, "DGCManager: vkGetBufferDeviceAddress returned 0\n");
            // Not fatal, but the buffer may not be usable for DGC.
        }
        // We don't store it here because we only need it for the preprocess buffer,
        // which is passed via the VkGeneratedCommandsInfoEXT structure.
    }

    return true;
}

void* DGCManager::mapInputBuffer(VkDeviceSize& outSize) {
    outSize = m_inputSize;
    return m_inputMapped;
}

void DGCManager::unmapInputBuffer() {
    // We keep the buffer mapped permanently, but if you need to flush,
    // you can call vkFlushMappedMemoryRanges here.
}

void DGCManager::updateExecutionSetPipeline(VkPipeline pipeline, uint32_t index) {
    if (!m_executionSet || index >= m_maxPipelines) {
        fprintf(stderr, "DGCManager: updateExecutionSetPipeline invalid index or execution set not ready\n");
        return;
    }

    VkWriteIndirectExecutionSetPipelineEXT write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_INDIRECT_EXECUTION_SET_PIPELINE_EXT;
    write.pNext = nullptr;
    write.index = index;
    write.pipeline = pipeline;

    pfnUpdateExecutionSetPipeline(
        m_device,
        m_executionSet,
        1,
        &write
    );
}

void DGCManager::destroy() {
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

    // Now safe to clear the device handle
    m_device = VK_NULL_HANDLE;
    m_physDev = VK_NULL_HANDLE;
}

DGCManager::DGCManager(DGCManager&& other) noexcept
    : m_device(other.m_device)
    , m_physDev(other.m_physDev)
    , m_layout(other.m_layout)
    , m_executionSet(other.m_executionSet)
    , m_inputBuffer(other.m_inputBuffer)
    , m_inputMemory(other.m_inputMemory)
    , m_inputMapped(other.m_inputMapped)
    , m_inputSize(other.m_inputSize)
    , m_inputAddress(other.m_inputAddress)
    , m_preprocessBuffer(other.m_preprocessBuffer)
    , m_preprocessMemory(other.m_preprocessMemory)
    , m_preprocessSize(other.m_preprocessSize)
    , m_preprocessAddress(other.m_preprocessAddress)
    , m_maxSequences(other.m_maxSequences)
    , m_pushConstantSize(other.m_pushConstantSize)
    , m_maxPipelines(other.m_maxPipelines)
    , m_currentSequenceCount(other.m_currentSequenceCount) {
    // Nullify source handles
    other.m_device = VK_NULL_HANDLE;
    other.m_physDev = VK_NULL_HANDLE;
    other.m_layout = VK_NULL_HANDLE;
    other.m_executionSet = VK_NULL_HANDLE;
    other.m_inputBuffer = VK_NULL_HANDLE;
    other.m_inputMemory = VK_NULL_HANDLE;
    other.m_inputMapped = nullptr;
    other.m_inputSize = 0;
    other.m_inputAddress = 0;
    other.m_preprocessBuffer = VK_NULL_HANDLE;
    other.m_preprocessMemory = VK_NULL_HANDLE;
    other.m_preprocessSize = 0;
    other.m_preprocessAddress = 0;
}

DGCManager& DGCManager::operator=(DGCManager&& other) noexcept {
    if (this == &other) return *this;

    // Destroy current resources
    destroy();

    // Transfer ownership
    m_device = other.m_device;
    m_physDev = other.m_physDev;
    m_layout = other.m_layout;
    m_executionSet = other.m_executionSet;
    m_inputBuffer = other.m_inputBuffer;
    m_inputMemory = other.m_inputMemory;
    m_inputMapped = other.m_inputMapped;
    m_inputSize = other.m_inputSize;
    m_inputAddress = other.m_inputAddress;
    m_preprocessBuffer = other.m_preprocessBuffer;
    m_preprocessMemory = other.m_preprocessMemory;
    m_preprocessSize = other.m_preprocessSize;
    m_preprocessAddress = other.m_preprocessAddress;
    m_maxSequences = other.m_maxSequences;
    m_pushConstantSize = other.m_pushConstantSize;
    m_maxPipelines = other.m_maxPipelines;
    m_currentSequenceCount = other.m_currentSequenceCount;

    // Nullify source
    other.m_device = VK_NULL_HANDLE;
    other.m_physDev = VK_NULL_HANDLE;
    other.m_layout = VK_NULL_HANDLE;
    other.m_executionSet = VK_NULL_HANDLE;
    other.m_inputBuffer = VK_NULL_HANDLE;
    other.m_inputMemory = VK_NULL_HANDLE;
    other.m_inputMapped = nullptr;
    other.m_inputSize = 0;
    other.m_inputAddress = 0;
    other.m_preprocessBuffer = VK_NULL_HANDLE;
    other.m_preprocessMemory = VK_NULL_HANDLE;
    other.m_preprocessSize = 0;
    other.m_preprocessAddress = 0;

    return *this;
}

}   // namespace GoCL