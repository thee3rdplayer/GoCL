# GoCL – Gestalt Open Computing Language

**Goal:** Heterogeneous Vulkan engine optimized across AMD, NVIDIA, and potentially Apple hardware using ASTC as the primary basis for optimization techniques.

## Key Features
- ASTC-based texture handling, with options for relaxed compression where appropriate.
- Optimized structure-of-arrays layouts for coalesced memory access.
- Multi-stream memory management with L2/shared cache tuning.
- Thread/block/warp optimization: occupancy, coalescing, and shuffle instructions.
- TMU and ROP-aware memory and texture sampling patterns.
- Algorithm-level optimizations for minimal divergence and better load balancing.

## Support Development
Help cover load shedding backups and hardware upgrades.

See [`SUPPORT.md`](./SUPPORT.md) for how to contribute to GoCL.

---

*All code is © 2025 Stanford Mukwena.*
