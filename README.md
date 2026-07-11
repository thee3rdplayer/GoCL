# GoCL — Cross‑Generational Vulkan Engine

GoCL (Gestalt Open Computing Language) is a lightweight static Vulkan library and a runtime proxy layer that
adapts to the hardware it runs on.  It detects GPU capabilities at startup,
rewrites shaders when needed, selects texture compression paths based on memory
and format support, and exposes runtime tuning through a plain‑text config file.

The same binary runs on integrated laptop graphics, handheld‑class devices,
and high‑end discrete GPUs — no platform‑specific builds.

**Key highlights**

- **Automatic shader emulation** – unsupported features are rewritten before the
  driver sees them, enabling modern shaders on older hardware.
- **Adaptive texture compression** – ASTC when available, ETC2 fallback
  everywhere else, with configurable aggressiveness.
- **Two ways to use it** – link the static library into your own engine, or
  inject the proxy into pre‑built Vulkan games without recompilation.
- **Zero per‑frame overhead** – shader patching happens once at pipeline
  creation; texture transcoding is lazy; indirect‑draw batching has no cost
  when idle.

## Using GoCL

### As a static library (build your own game)

Link against `GoCL_core.a` and include the headers from `src/`.  Your application
gains:

- Hardware‑adaptive rendering (capability oracle)
- Runtime shader emulation (FP16, Int8, Int16, SER, descriptor splitting)
- Intelligent texture compression (ASTC block selector + ETC2 fallback)
- VRAM budget tracking and pipeline cache persistence
- Software VRS emulation and meshlet‑based culling utilities
- Fully configurable via `GoCL.conf` 

`-DGOCL_ENABLE_LAYER=ON` to CMake.  Deploy it with `LD_PRELOAD` (Linux) or by
placing it alongside the game executable (Windows).  The proxy intercepts
Vulkan calls and transparently applies all of GoCL’s optimisations:

- Patches shaders before they reach the driver
- Transcodes ASTC textures to ETC2 on GPUs without hardware decode
- Tunes swap‑chain image count and present mode from `GoCL.conf`
- Splits large indirect draw calls into GPU‑friendly batches

The proxy is a Vulkan implicit layer that can be activated by setting
`ENABLE_GOCL_LAYER=1` and pointing `VK_LAYER_PATH` to the build directory.
It also works as a classic `LD_PRELOAD` shim on Linux or `vulkan‑1.dll` proxy
on Windows.

No game modifications are required — the same config file controls both the
engine and the proxy.  For full details, see **[DETAILS.md](./docs/DETAILS.md)**.

## Supported hardware

- **NVIDIA** – Kepler, Maxwell, Pascal, Turing, Ampere, Ada Lovelace
- **AMD** – GCN 1–5, RDNA 1/2/3
- **Intel** – Gen9 through Arc Alchemist
- **ARM Mali** – Bifrost and Valhall
- **Qualcomm Adreno** – 5xx and newer
- **Software renderers** – SwiftShader, Mesa LLVMpipe

## Quick start

```bash
git clone https://github.com/thee3rdplayer/GoCL.git
cd GoCL
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DGOCL_ENABLE_TESTS=ON -DGOCL_ENABLE_LAYER=ON
cmake --build build -j$(nproc)

# Run a Sascha Willems example (replace with your built example binary)
./path/to/SaschaWillems/example/binary

# Inject the proxy into an existing Vulkan game (Linux) 
LD_PRELOAD=./build/vulkan_proxy.so ./your_game  

# Windows (PowerShell / Command Prompt)
ren vulkan_proxy.dll vulkan-1.dll
# Move the renamed vulkan-1.dll into the game’s .exe folder.
``` 

# Caution

**Anticheat Detection:** Modern anticheats (like Easy Anti-Cheat or BattlEye) use heuristics to detect anomalies.  A proxy layer introduces behaviour deviation that these heuristics are trained to recognize.

**The Ban Logic:** While proxy games *can* be played online, playing them in a context where the anticheat expects standard game behavior (direct OS calls, no bypass layer) causes the detection engine to flag the user as a threat, resulting in account suspension. 

## Documentation

- **[ARCHITECTURE.md](./docs/ARCHITECTURE.md)** – design and subsystem overview
- **[DETAILS.md](./docs/DETAILS.md)** – configuration reference, build guide,
  proxy usage, profiling, and platform notes

## License and support

All code © 2026 Stanford Mukwena — [www.thee3rdplayer.blogspot.com](http://www.thee3rdplayer.blogspot.com).
See [`SUPPORT.md`](./SUPPORT.md) for ways to contribute or get help. 