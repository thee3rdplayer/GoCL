# Architecture

## Overview

GoCL follows one rule: **detect capability first, decide later**.

At startup, the engine snapshots the GPU and driver features available on the active device. Rendering decisions, shader adjustments, and texture format choices are derived from that snapshot instead of hard‑coded vendor paths.

The same codebase powers two use‑cases:

- **Static library** – link `GoCL_core.a` into your own Vulkan application to get cross‑generational rendering, texture compression, and performance utilities out of the box.
- **Proxy layer** – a Vulkan implicit layer (`vulkan_proxy.so` (or `.dll`)) that registers via `VK_LAYER_GoCL_Proxy.json` and chains through the loader's dispatch tables. It also works as a classic `LD_PRELOAD` / `vulkan‑1.dll` shim for environments without layer support.

## Core Pipeline

1. Instance and device creation
2. Capability snapshot
3. Shader and pipeline adaptation
4. Texture format selection
5. Frame submission
6. Present path selection
7. Optional cache persistence

## Capability Model (`CapabilityOracle`)

GoCL queries the physical device for every feature that matters for cross‑generational rendering:

- Texture format support (ASTC LDR / HDR, ETC2)
- Precision tiers (FP16, subgroup ops)
- Descriptor indexing tiers
- Queue topology, GPU family, and generation (detected via extensions)
- VRAM budget (total and remaining, with `VK_EXT_memory_budget` or fallback)
- SPIR‑V version ceiling

These results are stored in `DeviceCapabilities` and fed to every subsystem.

## Shader Handling (`LegacyShaderEmulator`)

Shaders that use features the current hardware cannot support are rewritten before the driver ever sees them. Transformations include:

- FP16 widened to FP32 (with correct denormal handling)
- Oversized descriptor sets split across smaller bindings (engine‑side only; disabled in proxy because it requires matching pipeline layouts)

This runs once at pipeline creation — it adds zero per‑frame cost.

## Device‑Generated Commands (DGC)

**Engine‑side**  
When the device supports `VK_EXT_device_generated_commands`, the engine creates a fixed token layout (`PUSH_CONSTANT` + `DRAW_INDEXED`) and an execution set. Pipelines are marked with `INDIRECT_BINDABLE_BIT`. Indirect draw calls are replaced by a single `vkCmdExecuteGeneratedCommandsEXT`, shifting CPU work to the GPU.

**Proxy‑side**  
The proxy intercepts `vkCmdDrawIndirect*` and `vkCmdBindPipeline`, tracks the current pipeline per command buffer, and records indirect data into a persistent input buffer via `vkCmdCopyBuffer`. At `vkEndCommandBuffer`, it executes all accumulated sequences. If DGC is unsupported or disabled, the proxy falls back to forwarding the original indirect calls.

**Fallback**  
When DGC is not available, the engine/proxy transparently falls back to standard CPU‑driven indirect draws. The `disable_dgc` config flag forces fallback for debugging.

## Texture Path (`TextureManager` + `ASTCBlockSelector`)

Textures are loaded, analysed, and compressed according to the GPU's actual capabilities:

1. ASTC when hardware decode is available (Mali, Adreno, Intel Gen9‑12, NVIDIA with `VK_EXT_astc_decode_mode`)
2. ETC2 fallback for all other cases (mandated by the Vulkan spec)
3. Correct HDR path when `VK_EXT_texture_compression_astc_hdr` is present

Block sizes are chosen dynamically based on texture usage, VRAM pressure, and a hardware‑empathy hint (`isMobileTier` / `isLowBandwidth`). All thresholds can be overridden in `GoCL.conf`.

For pre‑compressed `.basis` assets, the engine can transcode directly to ETC2 via `TranscodeBasisToETC2`, bypassing the heavyweight UASTC compression step entirely. This path is ideal for applications that ship their own offline‑compressed basis files.

The ETC2 transcoder is initialised once with the required **global selector codebook**, ensuring correct decode quality as recommended by the Basis Universal specification. All transcode paths share this static transcoder.

## Engine‑Side Utilities (Static Library Only)

### Dynamic Resolution Scaling (`DynamicResolutionHelper`)

Renders the scene to a downscaled off‑screen target, then upscales with a high‑quality spatial filter. Quality presets and the scale factor are controlled via `GoCL.conf`.

### Meshlet‑Based Culling (`MeshletCuller`)

Provides GPU‑driven frustum culling using a compute shader, replacing mesh shaders on hardware that lacks `VK_EXT_mesh_shader`. Batching limits can be tuned in `GoCL.conf`.

Both utilities are available only when linking against the GoCL library. They are not injected by the proxy.

## Queue and Frame Management

GoCL handles:

- Separate graphics and present queues (common on mobile SoCs)
- Dedicated compute queue when available
- Opt‑in async compute (via `CommandPool::initCompute`)
- Double‑buffered frame synchronisation (binary or timeline semaphores, automatically selected)
- Dynamic viewport / scissor for resolution scaling

## Pipeline Cache

Pipeline cache data is persisted to disk and reused across runs, dramatically reducing shader recompilation hitches on older drivers and slower CPUs.

The cache file path can be configured via `pipeline_cache_path = "gocl_cache.bin"` in the `[go_context]` section of `GoCL.conf`. The path is resolved relative to the game's working directory (or absolute if provided). The proxy loads from this file when the application provides no initial cache, and saves the final cache on `vkDestroyPipelineCache`.

## Proxy Layer (`src/layer/Proxy.cpp`)

The proxy is a Vulkan implicit layer that negotiates with the loader via `VkLayerInstanceCreateInfo` / `VkLayerDeviceCreateInfo`. It intercepts key entry points and applies all of GoCL's cross‑generational optimisations.

**Activation**  
- Set `ENABLE_GOCL_LAYER=1` and point `VK_LAYER_PATH` to the directory containing `VkLayer_GoCL_Proxy.json` and `vulkan_proxy.so` (or `.dll`).
- As a fallback, the same library can be used as a classic `LD_PRELOAD` shim or `vulkan‑1.dll` proxy.

### Intercepted Entry Points

- `vkCreateInstance` / `vkDestroyInstance` – instance lifecycle and GPA chain
- `vkCreateDevice` / `vkDestroyDevice` – device capability snapshot and GDPA chain
- `vkGetDeviceProcAddr` – replaces critical device‑level function pointers
- `vkCreateShaderModule` – patches SPIR‑V before compilation
- `vkCreateGraphicsPipelines` / `vkCreateComputePipelines` – passthrough
- `vkCreateSwapchainKHR` – injects image count and present mode from `GoCL.conf`
- `vkCreateImage` / `vkDestroyImage` – replaces ASTC formats with ETC2 when hardware decode is absent
- `vkCmdCopyBufferToImage` – on‑the‑fly ASTC → ETC2
- `vkAllocateCommandBuffers`, `vkBindBufferMemory`, `vkDestroyBuffer` – track buffer‑memory associations for the transcode path

### ASTC → ETC2 Transcoding

When the target GPU lacks hardware ASTC decode, the proxy:

1. Replaces the image format at creation time
2. Intercepts copy commands, maps the source buffer
3. Decodes ASTC blocks to raw RGBA via `astcenc`
4. Compresses the raw pixels to UASTC and then transcodes to ETC2 (`TranscodeToETC2`) – this is necessary because the proxy only has access to the decoded RGBA data
5. Uploads the ETC2 data through a dedicated staging buffer

The heavy UASTC encoder is kept in the separate `gocl_transcoder.so` (or `.dll`) and loaded lazily, so it only affects games that actually use ASTC textures.

#### VRAM‑Aware Adaptation

The proxy tracks the available VRAM budget using `VK_EXT_memory_budget` (fallback to static heap size when the extension is unavailable). When memory pressure is detected, it automatically adjusts swapchain behaviour:

- **VRAM < 256 MB** – `minImageCount` is clamped to the surface's minimum (typically 2 instead of 3), reducing the number of swapchain images.
- **VRAM < 128 MB (and DRS enabled)** – the DRS scale factor is capped at 0.5×, further lowering the rendering resolution.

These thresholds are currently fixed. If DRS is not enabled, only the swapchain image count reduction applies. The VRAM budget query itself is always active when the extension is available, but no action is taken when memory is sufficient.

> **Note on `max_frames_in_flight` override**  
> The VRAM‑driven reduction takes precedence over the `max_frames_in_flight` setting in `GoCL.conf`. The typical order in `InterceptedCreateSwapchainKHR` is:
> 1. Set `modInfo.minImageCount = max_frames_in_flight` (if >0).
> 2. Clamp to surface capabilities.
> 3. **If VRAM < 256 MB**, overwrite with a safe value (≥2, within surface limits).
>
> Example: `max_frames_in_flight = 4`, surface supports 2–4 images, VRAM = 200 MB → final `minImageCount = 2`. The config value is ignored to save memory. Once VRAM recovers (e.g., after a level change or driver release), the next swapchain recreation will respect the configuration value again.

### Lazy Loading of Basis Universal

The heavy Basis Universal encoder is moved into `gocl_transcoder.so`, loaded on demand via `dlopen` / `LoadLibrary` only when an ASTC texture is encountered. The Callgrind trace lists all loaded shared objects (ob= entries) and their functions. Consequently, its static initializers are not invoked.

### Eager Symbol Binding

On Linux, the proxy is built with `-Wl,-z,now` to force all dynamic symbols to be resolved at load time. Expensive dynamic linking calls (dlopen, dlsym, dlclose) and their associated lazy-binding machinery appear exclusively within Vulkan initialization functions (vkCreateInstance, vkCreateDevice, layer/ICD negotiation) and are never executed inside the render-loop hot path (e.g., vkQueueSubmit, vkCmdDrawIndexed, vkCmdBeginRenderPass). Therefore, no lazy-binding overhead for a missing ASTC library occurs during rendering.

## Configuration Model (`ConfigLoader` / `GoCL.conf`)

All tunable values are read once at startup via the `ConfigSingleton`. If `GoCL.conf` is absent, hard‑coded defaults are used. Settings are divided into engine‑side (affect applications linking GoCL) and proxy‑layer (affect injected games). Both groups reside in the same file.

## Testing and Validation

The test suite covers:

- Push‑constant safety
- DynamicResolutionHelper and MeshletCuller initialisation smoke tests

The Sascha Willems Vulkan examples (e.g., `triangle`) can be used to validate the full pipeline (descriptor sets, UBO, projection, swapchain) visually. Build them separately from [their repository](https://github.com/SaschaWillems/Vulkan).