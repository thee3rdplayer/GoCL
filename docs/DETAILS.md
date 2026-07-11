# GoCL — Detailed Reference

This document covers every practical aspect of building, configuring, profiling, and deploying GoCL. For design rationale and subsystem relationships, see [ARCHITECTURE.md](./ARCHITECTURE.md).

---

## Configuration Reference (`GoCL.conf`)

`GoCL.conf` is optional. Safe defaults are used when the file is absent. The config file is searched in the following order:

1. **Environment variable** – `GOCL_CONFIG` (if set, points to an explicit path)
2. **Current working directory** – `./GoCL.conf`
3. **User config directory** (platform‑specific):
   - **Windows**: `%APPDATA%\GoCL\GoCL.conf`
   - **Linux (XDG)**: `$XDG_CONFIG_HOME/GoCL/GoCL.conf` (if `XDG_CONFIG_HOME` is set)
   - **Linux (fallback)**: `$HOME/.config/GoCL/GoCL.conf`
4. **System‑wide directory**:
   - **Windows**: `%PROGRAMDATA%\GoCL\GoCL.conf`
   - **Linux**: `/etc/GoCL/GoCL.conf`
5. **Defaults** – if no file is found, built‑in defaults are used (both the engine library and the proxy look for the file using the same order)

Settings are grouped by the subsystem they control. Entries marked **(proxy)** only affect pre‑built games injected via the proxy layer; all others are engine‑side and apply to applications linking GoCL directly.

```conf
# Texture compression (engine‑side)
[astc.thresholds]
largePixels  = 2097152    # > ~8 MB raw RGBA → 6×6 blocks
mediumPixels = 1048576    # > ~4 MB raw RGBA → 5×5 blocks
tightVRAM    = 512        # VRAM budget (MB) below which 6×6 is forced

# Rendering / pipeline (engine‑side, some proxy overrides noted)
[render]
max_frames_in_flight   = 2          # (proxy) 1 = low latency, 2 = balance, 3 = throughput
                                    # 4 = max
msaa_samples           = 1          # (engine‑side) 1 = off; clamped to device max
enable_tessellation    = false      # opt‑in only – developer must provide shaders
preferred_present_mode = ""         # (proxy) "fifo", "fifo_relaxed", "mailbox", 
                                    # "immediate", or "" for auto 
disable_dgc            = false      # (proxy) if true, disables DGC even when supported;
                                    # falls back to CPU‑driven indirect draws
skip_astc_transcode    = true       # disable ASTC→ETC2 transcoding in the proxy (e.g., when
                                    # another layer already handles it). Can be overridden by
                                    # the environment variable `GOCL_SKIP_ASTC`
 
# Software DynamicResolutionHelper emulation
[dynamic_resolution_scaling]
enabled = false                     # (proxy)
scale   = 0.7                       # (proxy)
quality = "balanced"                # (engine‑side utility)   
                                    # "ultra_quality", "quality", "balanced", "performance"

# Meshlet‑based culling (engine‑side utility)
[meshlet_culler]
enabled = true 

# Staging / memory (proxy)
[go_context]
staging_size_MB     = 64                # host‑visible upload buffer size
pipeline_cache_path = "gocl_cache.bin"  # resolved relative to current working directory
```

### Field Notes

| Key | Description |
|-----|-------------|
| `largePixels` | Textures larger than this (raw pixel count) get 6×6 ASTC blocks |
| `mediumPixels` | Textures larger than this get 5×5 blocks |
| `tightVRAM` | If free VRAM drops below this, compression is more aggressive |
| `max_frames_in_flight` | 1 = minimum VRAM, 3 = maximum throughput |
| `msaa_samples` | Clamped automatically to `framebufferColorSampleCounts` |
| `enable_tessellation` | Turns on tessellation support (state struct must be provided) |
| `skip_astc_transcode` | If true, the proxy will not transcode ASTC textures to ETC2 | 
| `disable_dgc` | Disables DGC fallback, forces CPU indirect draws |
| `preferred_present_mode` | Overrides present mode; empty means auto‑pick | 
| `dynamic_resolution_scaling.enabled` | Enable software DynamicResolutionHelper emulation |
| `dynamic_resolution_scaling.scale` | Render resolution scale per dimension (0.25 – 1.0) |
| `dynamic_resolution_scaling.quality` | Upscaling quality preset |
| `meshlet_culler.enabled` | Enable meshlet‑based GPU culling | 
| `staging_size_MB` | Size of the host‑visible buffer used for texture uploads |
| `pipeline_cache_path` | File path for caching Vulkan pipeline objects |

---

## Minimal Vulkan Requirements

### Required
- `VK_KHR_surface`
- Platform surface extension for the host OS
- `VK_KHR_swapchain`
- Vulkan 1.1 core features

### Optional, Enabled Automatically When Present
- `VK_KHR_shader_float16_int8`
- `VK_EXT_descriptor_indexing`
- `VK_KHR_16bit_storage`
- `VK_EXT_device_generated_commands`
- `VK_EXT_texture_compression_astc_hdr`
- `VK_EXT_memory_budget`

---

## Recommended Hardware for Measurable Results

- **NVIDIA** – Kepler, Maxwell, Pascal, Turing
- **AMD** – GCN 1–5, RDNA 1/2/3
- **Intel** – Gen9 through Arc Alchemist
- **ARM Mali** – Bifrost and Valhall
- **Qualcomm Adreno** – 5xx and newer
- **Software renderers** – SwiftShader, Mesa LLVMpipe

No vendor‑specific code paths are required. The engine adapts automatically.

---

## Mobile / Platform Notes

### Android
- The system Vulkan loader is part of the OS image
- Replacing `libvulkan.so` system‑wide requires root access
- On rooted devices the proxy can be placed in the loader path
- Non‑rooted devices typically need an explicit layer manifest
- APK repackaging is possible but modifies the game binary

### MacOS
MacOS uses Metal rather than Vulkan directly. The proxy model does not apply without a translation layer such as MoltenVK.

### Hardware Empathy
The hardware‑hint fields `isMobileTier` and `isLowBandwidth` are automatically populated for Mali / Adreno / Intel integrated GPUs. They influence:

- ASTC block‑size selection (more aggressive compression)
- VRAM‑pressure thresholds (relaxed tight‑VRAM limits)

---

## Integration Status

Treat these features as integration‑dependent (they may rely on external projects or driver availability):

- FSR
- Android explicit‑layer deployment
- DirectX to Vulkan translation layer

Check [CHANGELOG.md](../CHANGELOG.md), recent commits, and release notes for current status.

---

## Validation

The project includes automated tests covering:

- Push‑constant safety
- DynamicResolutionHelper and MeshletCuller initialisation smoke tests

---

## Building

Set these variables once per session for convenience:

```bash
PROXY_SO="./build/vulkan_proxy.so"
APPNAME="./path/to/your/game/executable"
VULKAN_EXAMPLES_DIR="/path/to/SaschaWillems/examples/directory"
EXAMPLE_BIN="/path/to/SaschaWillems/examples/executable"
```

> Some examples require additional assets (textures, models). For detailed usage, please see the official [Sascha Willems Vulkan examples](https://github.com/SaschaWillems/Vulkan).

### Clean Rebuild

```bash
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DGOCL_ENABLE_TESTS=ON -DGOCL_ENABLE_LAYER=ON
cmake --build build -j$(nproc)
```

### Configure

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \ 
  -DGOCL_ENABLE_TESTS=ON \
  -DGOCL_ENABLE_LAYER=ON
```

### Build + Test

```bash
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure -V
```

---

## Deploying the Proxy Layer

GoCL's proxy can be used in two ways: as a **developer‑mode** injector (via `VK_LAYER_PATH`) or as a **production‑installed** implicit layer.

### Developer Mode (Quick Test)

Set the environment variables to point to your build directory:

```bash
export VK_LAYER_PATH=/path/to/GoCL/build
export ENABLE_GOCL_LAYER=1
./your_game
```

No file copying required. The layer is active for the current shell session.

### Disabling ASTC→ETC2 Transcoding via Environment Variable

If you are running under a system that already performs ASTC transcoding (e.g., custom driver), you can tell the proxy to skip its own conversion by setting `GOCL_SKIP_ASTC=1`. This avoids duplicate work and can improve performance.

```bash
GOCL_SKIP_ASTC=1 LD_PRELOAD=./vulkan_proxy.so ./your_game
```

Or export it globally:

```bash
export GOCL_SKIP_ASTC=1
./your_game
```

On Windows:

```cmd
set GOCL_SKIP_ASTC=1
your_game.exe
```

### Making Activation Permanent

The manifest tells the Vulkan loader to skip the layer **unless** the variable `ENABLE_GOCL_LAYER` is set. To avoid having to export it every time, choose one of these options:

- **Remove the `"enable_environment"` line** from `VkLayer_GoCL_Proxy.json`. The loader will then always activate the layer when it is discovered.

- **Export the variable in your shell profile** (e.g., `~/.bashrc` or `~/.zshrc`):
  ```bash
  export ENABLE_GOCL_LAYER=1
  ```
  This keeps the toggle but makes it persistent.

- **Use the disable variable** as an emergency off‑switch: setting `DISABLE_GOCL_LAYER=1` forcibly suppresses the layer even when it is installed and enabled.

### Production Installation (Permanent)

Place **both** `VkLayer_GoCL_Proxy.json` and `vulkan_proxy.so` (or `.dll`) into one of the standard implicit‑layer directories. The Vulkan loader automatically discovers them there.

| Platform | System‑wide Path | User‑specific Path |
|----------|------------------|---------------------|
| Linux | `/etc/vulkan/implicit_layer.d/` | `$HOME/.local/share/vulkan/implicit_layer.d/` |
| Windows | `%PROGRAMDATA%\Vulkan\implicit_layer.d\` | `%LOCALAPPDATA%\Vulkan\implicit_layer.d\` |

After placing the files, if you kept the `"enable_environment"` guard, set `ENABLE_GOCL_LAYER=1` before running any Vulkan application. No `VK_LAYER_PATH` is needed.

### Transcoder Library

The proxy uses a separate shared library (`gocl_transcoder.so` on Linux, `gocl_transcoder.dll` on Windows) that provides two entry points:

- `GoCL_TranscodeToETC2(raw, w, h)` – Compresses raw RGBA pixels to UASTC and then transcodes to ETC2. Used by the proxy when an ASTC texture needs to be converted.
- `GoCL_TranscodeBasisToETC2(data, size)` – Transcodes a pre‑compressed `.basis` blob directly to ETC2 without any re‑compression. Available for applications that link the engine and ship their own basis files.

The library is loaded on demand by the proxy and does not need to be registered in the Vulkan manifest. Place it in the same directory as `vulkan_proxy.so` (or `.dll`).

For engine‑side usage, you can call the static methods directly: `TextureManager::TranscodeToETC2(raw, w, h)` and `TextureManager::TranscodeBasisToETC2(data, size)`.

---

## Running the Sascha Willems Triangle Example

**Without proxy**
```bash
"$EXAMPLE_BIN"
```

**With proxy + MangoHud**
```bash
MANGOHUD=1 LD_PRELOAD="$PROXY_SO" "$EXAMPLE_BIN"
```

---

## Linking Against GoCL (`GoCL_core.a`)

This section explains how to use GoCL as a static library in your own Vulkan application. The steps assume you already have a Vulkan instance, a window surface (e.g., created via GLFW), and a basic CMake build.

Some features are demonstration-oriented and may expose fewer configuration options than production systems. The core rendering configuration remains fully supported.

### 1. Include the Public Headers

When you add the GoCL library target to your CMake project, the target `GoCL` already exports the `src` directory as a **public** include path. You don't need to set any extra include directories yourself.

In your source files you can directly write:

```cpp
#include "core/GoCLContext.h"
#include "render/FrameLoop.h"
#include "render/PipelineBuilder.h"
#include "render/TextureManager.h"
// … and optionally the engine‑side utilities:
#include "render/DynamicResolutionHelper.h"
#include "render/MeshletCuller.h"
```

### 2. Link the Library

```cmake
target_link_libraries(YourGame PRIVATE GoCL Vulkan::Vulkan)
```

### 3. Create the GoCL Context

```cpp
auto ctx = GoCL::GoCLContext::Create(instance);

/*
Internally:
- Enumerates physical devices
- Selects graphics queue family
- Creates logical device
*/

ctx.SetupPresentQueue(surface);
```

### 4. Set Up the Frame Loop

```cpp
GoCL::FrameLoop frameLoop;
if (!frameLoop.init(ctx, surface, width, height))
{
    throw std::runtime_error("FrameLoop initialization failed");
}
```

### 5. Build a Graphics Pipeline

```cpp
GoCL::PipelineDesc desc;
desc.renderPass = frameLoop.renderPass();
desc.layout     = GoCL::PipelineBuilder::CreateLayout(
                     ctx.device, descSetLayout, pushConstSize,
                     ctx.caps.maxPushConstantsSize);

GoCL::ShaderSource src{ vertexSPIRV, fragmentSPIRV };
VkPipeline pipeline = GoCL::PipelineBuilder::CreateGraphicsPipeline(ctx, src, desc);

/*
Note:
All pipelines sharing push constant data must use
compatible VkPipelineLayouts. Incompatible layouts
will trigger validation errors during draw/dispatch.
*/
```

### 6. Load Textures

```cpp
// Caller owns:
VkImage
VkDeviceMemory

// TextureManager owns:
// (staging resources only)
VkDeviceMemory texMemory;
VkImage tex = GoCL::TextureManager::LoadTexture(
    ctx, "assets/diffuse.png", GoCL::TextureUsage::Diffuse, &texMemory);

// … at shutdown: ctx.DestroyTexture(tex, texMemory);
```

### 7. Use Engine‑Side Utilities (Optional)

- **DynamicResolutionHelper** — Renders at a lower internal resolution and upscales
- **MeshletCuller** — GPU‑driven frustum culling without mesh shaders

### 8. GPU‑Driven Indirect Drawing (DGC)

```cpp
if (ctx.caps.dgcSupported) {
    GoCL::DGCManager dgc;
    // creates layout + execution set
    dgc.init(ctx);  
    dgc.initBuffers(maxSequences, pushConstSize);

    // Map input buffer, fill pipeline index + draw commands
    VkDeviceSize mappedSize;
    void* mapped = dgc.mapInputBuffer(mappedSize);
    uint32_t pipelineIndex = 0;
    VkDrawIndexedIndirectCommand cmd = { ... };
    // …
    dgc.unmapInputBuffer();

    // Execute
    VkGeneratedCommandsInfoEXT info{};
    // … set info using dgc.layout, dgc.executionSet, etc.
    vkCmdExecuteGeneratedCommandsEXT(cmdBuf, VK_FALSE, &info);
}
```

### 9. Main Loop

```cpp
bool needsResize = false;
while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    frameLoop.drawFrame(ctx, [&](VkCommandBuffer cmd, VkFramebuffer /*fb*/,
                                 uint32_t frameIdx, VkExtent2D extent) {

        // If pipeline uses dynamic state:
        // vkCmdSetViewport()
        // vkCmdSetScissor()
    }, needsResize);

    // needsResize is typically set when:
    // - vkAcquireNextImageKHR returns VK_ERROR_OUT_OF_DATE_KHR
    // - vkQueuePresentKHR returns VK_ERROR_OUT_OF_DATE_KHR
    // - vkQueuePresentKHR returns VK_SUBOPTIMAL_KHR
    if (needsResize) {
        int w, h; glfwGetFramebufferSize(window, &w, &h);
        frameLoop.resize(ctx, w, h);
    }
}
```

### 10. Tear Down

```cpp
// Shutdown only.
// Avoid using every frame or during resize.
vkDeviceWaitIdle(ctx.device);
dgc.destroy();
vkDestroyPipeline(ctx.device, pipeline, nullptr);
vkDestroyPipelineLayout(ctx.device, desc.layout, nullptr);
frameLoop.destroy();
ctx.Destroy();

// Only if your application called glfwInit().
glfwTerminate();
```

All features can be tuned via `GoCL.conf`. See the configuration reference section for every available key.

---

## Injecting into Pre‑Built Games (Proxy)

The proxy library (`vulkan_proxy.so` on Linux, `vulkan_proxy.dll` on Windows) is built when `-DGOCL_ENABLE_LAYER=ON` is passed to CMake.

**Deployment**
- **Linux:** `LD_PRELOAD=/path/to/vulkan_proxy.so ./game`
- **Windows:** Rename `vulkan_proxy.dll` to `vulkan‑1.dll` and place it in the game's executable directory.

**What It Does Automatically**
- Patches SPIR‑V shaders (`vkCreateShaderModule`)
- Adjusts swap‑chain image count and present mode from `GoCL.conf`
- Transcodes ASTC textures to ETC2 on GPUs that lack hardware decode

### Why the Proxy Does Not Enable Validation Layers

The proxy is designed to be **invisible** – it must not alter the frame time or memory footprint of the game it injects. Vulkan validation layers add measurable CPU overhead to every API call, which contradicts the proxy's zero‑per‑frame‑cost guarantee.

To debug a game with validation, set the environment variable:

```bash
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
```

### Multi‑GPU Systems (e.g., NVIDIA Optimus)

```bash
__NV_PRIME_RENDER_OFFLOAD=1 __VK_LAYER_NV_optimus=NVIDIA_only \
  __GLX_VENDOR_LIBRARY_NAME=nvidia LD_PRELOAD="$PROXY_SO" "$APPNAME"
```

---

## Performance Measurement

### MangoHud (FPS / Frame Time)

```bash
MANGOHUD=1 LD_PRELOAD="$PROXY_SO" "$APPNAME"
```

### Valgrind / Callgrind (CPU Instruction Analysis)

**Native run**
```bash
valgrind --tool=callgrind --callgrind-out-file=callgrind.no_proxy.out "$APPNAME"
```

**Proxy run**
```bash
LD_PRELOAD="$PROXY_SO" \
valgrind --tool=callgrind --callgrind-out-file=callgrind.with_proxy.out "$APPNAME"
```

**Compare**
```bash
kcachegrind callgrind.no_proxy.out callgrind.with_proxy.out
```

**Annotated summary (hot functions only)**
```bash
callgrind_annotate --threshold=1.0 callgrind.no_proxy.out > native_hot.txt
callgrind_annotate --threshold=1.0 callgrind.with_proxy.out > proxy_hot.txt
diff native_hot.txt proxy_hot.txt
```

### RenderDoc (Frame Capture)

RenderDoc frame capture is initiated by pointing the application at the executable and defining necessary environment variables within the "Environment Modification" panel, including `LD_PRELOAD` and any specific offload variables.

---

## Debug

```bash
gdb ./build/path/to/file
```

---

## Proxy Performance Characteristics

After optimisations (eager symbol binding and lazy‑loaded Basis Universal), the proxy's instruction footprint is indistinguishable from native:

- **FPS:** Identical within measurement noise (±1‑3%)
- **Frame time:** Identical
- **CPU instruction count:** Native ≈3.955B, Proxy ≈3.806B (proxy has ~3.8% fewer instructions total)
- **Per‑frame overhead:** Zero

Measured on a Maxwell‑era GTX 960M with Sascha Willems examples.

---

## Troubleshooting

### `shader_data.h` Not Found After Initial Clone

Point clangd at the build directory:

```yaml
# .clangd (project root)
CompileFlags:
  CompilationDatabase: build
```

Or symlink:
```bash
ln -sf build/compile_commands.json compile_commands.json
```

### Enabling Vulkan Validation Layers for the Test Suite

The test binaries do not enable `VK_LAYER_KHRONOS_validation` by default. To run any test with full validation, set the environment variable:

```bash
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
ctest --test-dir build --output-on-failure -V
```

### Proxy Messages Not Appearing

The proxy writes to `stderr`. Run your app from a terminal.

### ASTC Textures Not Being Transcoded

The transcoding path only activates when:
- The GPU lacks hardware ASTC decode (`astcLdrHW == false`)
- The application creates an image with a `VK_FORMAT_ASTC_*` format

Check the capability test output for your GPU's ASTC support.