<div align="center">

  <h1>KestrelX</h1>

  <p><strong>Clean-Room Xbox Series X|S Native Compatibility &amp; Dynamic RDNA2 (GFX10.3) &rarr; SM 6.6 DXIL Shader Recompilation Runtime</strong></p>

  <p>
    <a href="https://github.com/Batushn/KestrelX/releases/tag/v0.7.0"><img src="https://img.shields.io/badge/Release-v0.7.0-2ea44f?style=for-the-badge" alt="Release: v0.7.0" /></a>
    <a href="#verified-hardware-test-receipts"><img src="https://img.shields.io/badge/Hardware_Tests-61%2F61_PASS-brightgreen?style=for-the-badge" alt="Tests: 61/61 PASS" /></a>
    <a href="#hardware-visual-showcase"><img src="https://img.shields.io/badge/Dual--GPU-RTX_4060_%2B_Radeon_780M-0078D6?style=for-the-badge" alt="Dual-GPU: NVIDIA + AMD" /></a>
    <a href="CMakeLists.txt"><img src="https://img.shields.io/badge/C%2B%2B-23-00599C?style=for-the-badge" alt="C++23" /></a>
    <a href="docs/ARCHITECTURE.md"><img src="https://img.shields.io/badge/D3D12_Ultimate-SM_6.6_DXIL-107C10?style=for-the-badge" alt="D3D12 Ultimate" /></a>
    <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT_Clean--Room-blue?style=for-the-badge" alt="License: MIT" /></a>
    <a href="https://discord.gg/KxsNwBwhVs"><img src="https://img.shields.io/badge/Discord-Join_Community-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Discord: Community" /></a>
  </p>

  <p><em>Built from scratch in C++23 under the MIT license &mdash; zero proprietary code, zero game assets, zero cryptographic keys.</em></p>

  <p>
    <a href="#hardware-visual-showcase">Visual Showcase</a> &bull;
    <a href="#live-progress-dashboard">Progress Dashboard</a> &bull;
    <a href="#architecture">Architecture</a> &bull;
    <a href="#verified-hardware-test-receipts">Hardware Receipts</a> &bull;
    <a href="#building--running">Build &amp; Run</a> &bull;
    <a href="https://discord.gg/KxsNwBwhVs">Discord</a> &bull;
    <a href="#contributing">Contribute</a>
  </p>

</div>

---

## Hardware Visual Showcase

Every image below is captured directly from real **D3D12 Ultimate** hardware after KestrelX dynamically decodes raw **GFX10.3 (RDNA2)** microcode, lifts it into CFG basic-block IR, emits SM 6.6 HLSL, compiles signed DXIL in-process via `dxcompiler.dll`, and renders on both **NVIDIA GeForce RTX 4060** and **AMD Radeon 780M**:

<table>
  <tr>
    <td align="center" width="50%">
      <img src="docs/cube_3d.png" alt="KestrelX v0.5.0 RDNA2 3D Depth-Buffered Cube" width="280"/><br/>
      <b>v0.5.0 — 3D Depth-Buffered Perspective Cube</b><br/>
      <sub>Raw RDNA2 <code>D32_FLOAT</code> Depth-Stencil, Dynamic Descriptors & Wave Intrinsics<br/>
      Dual-GPU rasterization parity: <b>7222 vs 7224 px (&lt;0.03%)</b></sub>
    </td>
    <td align="center" width="50%">
      <img src="docs/pbr_mrt.png" alt="KestrelX v0.6.0 RDNA2 PBR Deferred GBuffer 3-MRT" width="280"/><br/>
      <b>v0.6.0 — PBR Deferred GBuffer (3-MRT Pipeline)</b><br/>
      <sub>Simultaneous multi-target rasterization: <b>[MRT0: Albedo | MRT1: Normal | MRT2: Material]</b><br/>
      Dual-GPU cross-vendor rendering parity: <b>0/255 bit-exact</b></sub>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <img src="docs/first_light.png" alt="KestrelX v0.3.0 Visual First Light" width="220"/><br/>
      <b>v0.3.0 — Visual First Light</b><br/>
      <sub>Raw RDNA2 <code>EXP pos0/param0/mrt0</code> via <code>KxDeviceProxy</code></sub>
    </td>
    <td align="center" width="50%">
      <img src="docs/textured_quad.png" alt="KestrelX v0.4.0 RDNA2 MUBUF + MIMG Textured Quad" width="220"/><br/>
      <b>v0.4.0 — Textured Quad</b><br/>
      <sub>Raw RDNA2 <code>MUBUF</code> Vertex + <code>MIMG</code> Pixel Shader (bit-exact)</sub>
    </td>
  </tr>
</table>

---

## Live Progress Dashboard

### Milestone Timeline

```mermaid
flowchart LR
    M1["✅ v0.1.0<br/>PE32+ Loader, VFS,<br/>VEH Zen 2, RDNA2 CS"]
    M2["✅ v0.2.0<br/>SMEM + LDS + Barrier,<br/>D3D12.X PSO Proxy, BCn"]
    M3["✅ v0.3.0<br/>Visual First Light (VS/PS),<br/>WindowHost, GameInput, XA2"]
    M4["✅ v0.4.0<br/>MUBUF/MTBUF + MIMG Quad,<br/>Disk DXIL Cache, Dev1..10, WASAPI"]
    M5["✅ v0.5.0<br/>Wave Intrinsics, 3D Depth Cube,<br/>Dynamic Descriptors, DirectStorage"]
    M6["✅ v0.6.0<br/>2D Compute Tiling, PBR MRT,<br/>Async Compute, Force Feedback"]
    M7["✅ v0.7.0 (Current)<br/>Mesh Shaders, VRS Tier 2,<br/>Work Graphs, IR Optimizer"]
    M8["🚧 v0.8.0 (Next)<br/>GPU DirectStorage 1.2,<br/>DXR 1.1 / BVH, Enhanced Barriers"]
    M9["📋 v0.9.0 (Planned)<br/>Full Asset Decryption Shim,<br/>Sparse Virtual Textures, HUD"]

    M1 --> M2 --> M3 --> M4 --> M5 --> M6 --> M7 --> M8 --> M9

    style M1 fill:#1f6f3b,stroke:#2ea44f,color:#fff
    style M2 fill:#1f6f3b,stroke:#2ea44f,color:#fff
    style M3 fill:#1f6f3b,stroke:#2ea44f,color:#fff
    style M4 fill:#1f6f3b,stroke:#2ea44f,color:#fff
    style M5 fill:#1f6f3b,stroke:#2ea44f,color:#fff
    style M6 fill:#1f6f3b,stroke:#2ea44f,color:#fff
    style M7 fill:#107c10,stroke:#56d364,stroke-width:3px,color:#fff
    style M8 fill:#9a6700,stroke:#d29922,color:#fff
    style M9 fill:#4c2889,stroke:#8957e5,color:#fff
```

### RDNA2 IR Multi-Pass Optimization Pipeline

```mermaid
flowchart LR
    A["Raw GFX10.3 Microcode<br/>(SALU, VALU, EXP, MUBUF)"] --> B["CFG Lifter<br/>Basic Blocks & Registers"]
    B --> C["Pass 1: Unreachable Pruning<br/>BFS Dead-Block Elimination"]
    C --> D["Pass 2: Copy Propagation<br/>SMov/VMov Register Inlining"]
    D --> E["Pass 3: Constant Folding<br/>Arithmetic & Identity Reduction"]
    E --> F["Pass 4: Backward DCE<br/>Liveness Analysis & Dead Flags"]
    F --> G["SM 6.6 HLSL Generator<br/>Wave32 (NVIDIA) / Wave64 (AMD)"]
    G --> H["In-Process DXC<br/>dxcompiler.dll"]
    H --> I["Persistent Disk Cache<br/>LRU Quota (.kxdxil)"]

    style A fill:#161b22,stroke:#30363d,color:#c9d1d9
    style B fill:#161b22,stroke:#30363d,color:#c9d1d9
    style C fill:#1f6f3b,stroke:#2ea44f,color:#fff
    style D fill:#1f6f3b,stroke:#2ea44f,color:#fff
    style E fill:#1f6f3b,stroke:#2ea44f,color:#fff
    style F fill:#1f6f3b,stroke:#2ea44f,color:#fff
    style G fill:#00599c,stroke:#1f6feb,color:#fff
    style H fill:#107c10,stroke:#56d364,color:#fff
    style I fill:#0078d6,stroke:#58a6ff,color:#fff
```

### Subsystem Completion Matrix

| Subsystem | Progress | Status | Implemented Today (v0.7.0) | Up Next (v0.8.0 & v0.9.0) |
| :--- | :--- | :---: | :--- | :--- |
| **PE32+ Loader & System Libs** | `██████████ 100%` | ✅ Ready | `MicrosoftGame.config` XML, PE32+ relocs (DIR64/HIGHLOW), delay-load imports (RVA/legacy VA), export forwarders (DLL.Symbol, DLL.#Ordinal), multi-page thunk leak cleanup, `.pdata` SEH, TLS callbacks, dynamic GDK Agility SDK query (`D3D12SDKVersion`, `D3D12SDKPath`), dynamic DLL redirection & search directories | Side-by-side title updates & patch mounts |
| **CPU VEH Zen 2 Shim** | `██████████ 100%` | ✅ Ready | In-process VEH emulator for AMD Zen 2 `CLZERO`, `MONITORX`, `MWAITX`, `MCOMMIT`, `RDPRU`, `PREFETCHW`, and SSE4a `EXTRQ`/`INSERTQ` for crash-free Intel processor compatibility | Dynamic TSC virtualization & CPU core topology pinning |
| **Copy-on-Write Overlay VFS** | `██████████ 100%` | ✅ Ready | Case-insensitive per-layer path lookup cache, merged `list_directory` across base and overlay, whiteout suppression, non-throwing `std::error_code` safety, multi-user save partition isolation (`set_active_user_xuid`, `active_save_root`), Xbox drive prefixes (`GAME:\`, `G:\`, `D:\`, `C:\`, `S:\`, `T:\`) | Encrypted container archive streaming mounts |
| **RDNA2 -> IR -> SM 6.6 DXIL** | `██████████ 98%` | ✅ Ready | Complete 30-predicate VCmp/SCmp, 64-bit scalar ALU, Mesh Shaders (`ms_6_6`), Amplification Shaders (`as_6_6`), VRS Tier 2 decoders & combiners, Hardware Raytracing BVH traversal (`image_bvh_intersect_ray` / `image_bvh64_intersect_ray` -> inline SM 6.6 `RayQuery`), PBR 3-MRT GBuffer, 2D compute tiling, Wave intrinsics | Inline procedural primitive intersection shims |
| **RDNA2 IR Optimization Passes** | `██████████ 100%` | ✅ Ready | Unreachable basic-block pruning, intra-block copy propagation, constant folding (integer & float operations), backward liveness DCE & unused flag pruning, bit-exact dual-GPU parity | Inter-block global value numbering (GVN) |
| **Persistent DXIL Shader Cache** | `██████████ 100%` | ✅ Ready | Mutex-protected thread-safe `DiskShaderCache`, LRU size quotas (`max_entries`, `max_bytes`), multi-dimensional dispatch keys (`group_y`, `group_z`, `mrt_mask`), corruption eviction | Background async shader pre-compilation pool |
| **D3D12.X COM Proxy (`d3d12_xs`)** | `██████████ 100%` | ✅ Ready | Pure COM `KxDeviceProxy` (`ID3D12Device`-`Device10` QI), stream PSO auto-recomp, dedicated Async Compute queues with timeline fence sync, Work Graph DAG validation & 2-node `ExecuteIndirect` producer->consumer, `ID3D12GraphicsCommandList6/7` Enhanced Barriers & `DispatchMesh`, `d3d12.dll`/`d3d12core.dll` proxying & `D3D12GetInterface` export shim | DirectSR super-resolution bridge |
| **WindowHost & PresentX** | `████████░░ 85%` | ✅ Ready | Win32 `WindowHost` + `IDXGISwapChain3` flip-model (`--windowed`), headless GPU readback BMP capture (`--capture`), `D3D12PresentX` | Fullscreen borderless toggle & frame-pacing telemetry HUD |
| **Input (`gameinput.dll`)** | `█████████░ 95%` | ✅ Ready | COM `GameInputCreate`, multi-controller slot management (0..3), dynamic hotplugging callbacks & telemetry, impulse trigger force-feedback + dual-motor rumble bridging | Virtual gamepad remapping & deadzone calibration |
| **Audio (`xaudio2_9.dll`)** | `██████████ 98%` | ✅ Ready | COM `XAudio2Create`, multi-voice software mixer, submix bus routing, non-48kHz pitch resampling, X3DAudio 3D spatial positioning, ITU-R BS.775 5.1 & 7.1 surround downmixing matrix, WASAPI thread lifecycle | WASAPI exclusive mode stream sync |
| **KernelX & GDK HLE** | `█████████░ 95%` | ✅ Ready | `XMemAlloc`/`Free`/`Size`, `TitleMemoryStatus`, `CreateFileA/W/2`, `ReadFileEx`, `XTaskQueue*`, DirectStorage 1.2 GDeflate GPU streaming, multi-user identity profiles (`kx_XUserAddAsync`, `kx_XUserFindControllerUser`, `kx_XUserSetActive`), controller binding, isolated `S:\` save partitions | Cloud save sync stubbing |
| **GFX10 Texture Tiling** | `█████████░ 90%` | ✅ Ready | Clean-room 2D swizzle/deswizzle (all 7 modes), BC1/BC7 4×4 block tiling, 256B-aligned multi-level mip-chains, SGD5 archive decompression | 3D/Array volume tiling & live upload staging |

---

### What's New in v0.7.0

KestrelX v0.7.0 delivers major architectural expansions across the graphics, compiler, and OS runtime layers:
* **RDNA2 Mesh (`ms_6_6`) & Amplification (`as_6_6`) Shaders**: Full support for console primitive export, dynamic vertex output counts, and workgroup amplification pipelines compiled cleanly via SM 6.6 DXIL.
* **Variable Rate Shading (VRS Tier 2)**: Dynamic 16×16 and 8×8 tile rate-image decoders, combiners, and `ID3D12GraphicsCommandList5::RSSetShadingRateImage` proxy support.
* **D3D12 Work Graphs**: DAG validation, 16-byte aligned `RecordRingAllocator`, and dual-GPU verified 2-node `ExecuteIndirect` producer->consumer pipeline.
* **RDNA2 IR Optimization Passes**: 4 deterministic optimization passes (unreachable block pruning, copy propagation, constant folding, and backward liveness dead-code elimination) yielding reduced HLSL size while preserving bit-exact dual-GPU output.
* **Persistent Disk Shader Cache LRU Quota**: Thread-safe caching with entry and byte-limit LRU evictions and multi-dimensional dispatch key support.
* **Overlay VFS Case-Insensitive Cache & Directory Enumeration**: Per-layer cache eliminating redundant disk iterations, merged `list_directory` with whiteout masking, and non-throwing `std::error_code` safety.
* **Expanded RDNA2 Instruction Set**: Full 30-predicate VCmp/SCmp coverage, 64-bit scalar moves/inversions, bitfield manipulation (`v_bfe_u32`, `v_bfi_b32`), and global 4-dword memory transactions.
* **Audio & Input Hardening**: Multi-voice XAudio2 mixer with submix routing, pitch resampling, X3DAudio spatialization, and 4-slot GameInput multi-controller hotplugging.
* **GDK Agility SDK Query & Dynamic DLL Redirection**: Discovers guest exports `D3D12SDKVersion` and `D3D12SDKPath`, automatically mounts packaged SDK directories to search paths, and transparently redirects guest imports for `d3d12.dll` and `d3d12core.dll` to KestrelX HLE shims (`kx_D3D12GetInterface`, `D3D12CreateDevice`).
* **Dual-GPU Verification Expanded to 61/61 PASS**: Every feature tested bit-exact across physical NVIDIA RTX 4060 and AMD Radeon 780M hardware, plus `--unit-only` mode (48 passed, 13 skipped) and GitHub Actions CI.

---

### Roadmap: Upcoming Milestones

```mermaid
flowchart TD
    subgraph V8["v0.8.0 - Streaming, Raytracing & Modern Barriers"]
        V8_1["DirectStorage 1.2 GPU Decompression<br/>GDeflate Compute Shader Pipeline"]
        V8_2["DXR 1.1 / Raytracing Tier 1.1 Emulation<br/>image_bvh_intersect_ray & Acceleration Structures"]
        V8_3["D3D12 Enhanced Barriers<br/>D3D12_BARRIER_GROUP Memory Transitions"]
        V8_4["7.1 Spatial Audio & Channel Matrix<br/>Low-latency WASAPI Exclusive Mode"]
        V8_5["Multi-User Identity & Profile Saves<br/>Isolated S:\ Mount Points"]
    end

    subgraph V9["v0.9.0 - Path to Commercial Boot"]
        V9_1["Full GDK Container & Decryption Shim<br/>Transparent Content Decompression"]
        V9_2["Agility SDK Dynamic Injection<br/>Host-side Modern Runtime Orchestration"]
        V9_3["Sparse Virtual Textures (SVT)<br/>64KB Physical Page Table Remapping"]
        V9_4["Frame-Pacing HUD & Low-Latency PresentX<br/>Real-Time Graph Telemetry & Tear-Free Presentation"]
    end

    V8 --> V9

    style V8 fill:#161b22,stroke:#d29922,color:#fff
    style V9 fill:#161b22,stroke:#8957e5,color:#fff
```

#### v0.8.0 — Streaming, Raytracing & Modern Barriers (In Active Development)
1. **DirectStorage 1.2 GPU Decompression**: Implement in-shader GDeflate decompression compute shaders to offload game asset decompression directly to host GPUs.
2. **DXR 1.1 / Raytracing Tier 1.1 Emulation**: Decode RDNA2 ray intersection microcode (`image_bvh_intersect_ray`) and map console BVH structures to D3D12 top/bottom-level acceleration structures (`BuildRaytracingAccelerationStructure`).
3. **D3D12 Enhanced Barriers (`D3D12_BARRIER_GROUP`)**: Lift RDNA2 cache invalidate/flush primitives (`s_waitcnt`, `buffer_wbl2`) into fine-grained modern D3D12 enhanced buffer and texture barriers.
4. **Spatial Audio 7.1 Downmixing**: 8-channel surround matrix mixer and low-latency WASAPI exclusive mode streaming.
5. **Multi-User Identity & Profile Saves**: Profile-isolated `S:\` save mounts allowing simultaneous independent user profiles.

#### v0.9.0 — Path to Commercial Boot (Upcoming Sprint)
1. **Full GDK Cryptographic & License Decryption Shim**: Transparent pass-through for unencrypted container archives and synthetic package license acquisition (`XStoreAcquireLicense`).
2. **DirectX 12 Agility SDK Runtime Dynamic Injection**: Host-side injection of modern Agility SDK runtime components for seamless Windows 10/11 cross-compatibility.
3. **Sparse Virtual Texturing (SVT) & Feedback Maps**: 64KB physical page table remapping for streaming open-world virtual textures.
4. **Frame Pacing & PresentX Low-Latency HUD**: Flip-model tear-free sync, dynamic frame limiter, and in-game performance/latency overlay.

> **Honest Scope Note (`v0.7.0`):** KestrelX executes our clean-room Xbox-ABI test title, dynamically recompiles RDNA2 Compute/Vertex/Pixel/Mesh/Amplification shaders on real NVIDIA & AMD GPUs, features dedicated Async Compute queues with timeline fence sync, and bridges GameInput force-feedback haptics. It does **not** yet boot commercial retail Xbox Series X|S games — closing that gap requires the v0.8.0 / v0.9.0 roadmap features above (GPU asset decompression, DXR BVH translation, and commercial title container decryption). Pick up any open issue to help build the next milestone!

---

### PortWatch VI (Porthole) Groundwork Rubric

| Groundwork Component | Score | Status | Evidence & Verified Capabilities |
| :--- | :---: | :---: | :--- |
| **Loader & System Libraries** | 0.90 | ✅ Verified | PE32+ loader with recursive guest DLL mapping, base relocations, .pdata SEH via RtlAddFunctionTable, TLS callbacks, kernelx.dll (XMemAlloc/Free), xgameruntime.dll, and AMD Zen 2 VEH shim (CLZERO/MONITORX/MWAITX + SSE4a EXTRQ/INSERTQ for Intel compatibility). Compatible with game_scarlett_master.exe PE32+ structures. |
| **Shader Translation** | 0.90 | ✅ Verified | Clean-room RDNA2 (GFX10.3) microcode decoder, CFG IR lifter, dual Wave32/Wave64 SM 6.6 HLSL emission, in-process DXC (dxcompiler.dll), atomic disk cache (.kxdxil), strict refusal of unhandled opcodes (Test 10), and SGD5 container archive unpacking (shaders.rpf, Test 27). |
| **Graphics Layer** | 0.85 | ✅ Verified | Pure COM wrapper KxDeviceProxy (ID3D12Device -> Device10 QI), Stream PSO recomp, dedicated Async Compute command queues (D3D12_COMMAND_LIST_TYPE_COMPUTE), cross-queue fence synchronization telemetry, and Win32 WindowHost flip-model swapchain PresentX. |
| **Proven on Test Title** | 0.80 | ✅ Verified | Stand-alone clean-room guest test title (kestrel_test_title.exe + kestrel_test_dll.dll linked against synthetic GDK import libraries) boots via PeLoader with d3d12_xs.dll RTV clear + PresentX; 29/29 dual-GPU bit-exact test receipts on NVIDIA RTX 4060 and AMD Radeon 780M with 0 compiler warnings. |

## Architecture

```mermaid
flowchart LR
    A["Title Dir<br/>MicrosoftGame.config + PE32+"] --> B["PeLoader + Overlay VFS<br/>Relocs — •  IAT — •  .pdata SEH — •  TLS"]
    B --> C["Native x64 Execution<br/>+ VEH Zen 2 Shim<br/>(CLZERO / MONITORX / MWAITX)"]
    C --> D["HLE Runtime<br/>kernelx (Async I/O) — •  xgameruntime<br/>gameinput — •  xaudio2 (WASAPI)"]
    C --> E["d3d12_xs COM Proxy<br/>KxDeviceProxy (Dev1..10)<br/>KxGraphicsCommandListProxy"]
    E --> F["RDNA2 (GFX10.3) Decoder<br/>SOP — •  VOP — •  FLAT — •  SMEM — •  DS<br/>MUBUF — •  MTBUF — •  MIMG — •  EXP"]
    F --> G["CFG Basic-Block IR<br/>-> Dual-Wave SM 6.6 HLSL<br/>Wave32 (NVIDIA) — •  Wave64 (AMD)"]
    G --> H["In-Process DXC + Disk Cache<br/>dxcompiler.dll <-> .kxdxil"]
    H --> I["D3D12 Ultimate GPU<br/>+ WindowHost / BMP Capture"]
```

### Why KestrelX? (Architectural Differentiation)

| Subsystem | KestrelX Design | Why It Matters |
| :--- | :--- | :--- |
| **D3D12.X (`d3d12_xs.dll`)** | **Pure COM Wrapper Proxies (`KxDeviceProxy`, `KxCommandQueueProxy`, `KxGraphicsCommandListProxy`)** | Zero in-place driver vtable patching — safe across NVIDIA, AMD, Intel, and Wine/vkd3d-proton drivers. |
| **Shader Pipeline** | **RDNA2 (GFX10.3) -> CFG Basic-Block IR -> Dual Wave32 & Wave64 SM 6.6 HLSL -> In-Process DXC + Persistent Disk Cache** | Compiles native Wave32 shaders for NVIDIA warps and native Wave64 shaders for AMD RDNA GPUs from the same structured IR, verified bit-exact on dual-GPU hardware. |
| **CPU Execution** | **Native PE32+ x64 Loader + VEH Zen 2 Shim** | Runs x86-64 natively with full relocations, guest DLL chaining, TLS callbacks, `.pdata` SEH, and VEH emulation for AMD Zen 2 instructions so Intel CPUs do not trap with `0xC000001D`. |
| **Filesystem** | **Copy-on-Write Overlay VFS** | Maps `GAME:\`, `G:\`, `D:\`, `S:\`, and `T:\` with whiteouts and path-traversal rejection. The source title directory is **never mutated**. |
| **Verification** | **18 Self-Contained Hardware & Unit Tests + Clean-Room Guest Title** | Links against 5 synthetic console import libs (`kernelx`, `xgameruntime`, `d3d12_xs`, `gameinput`, `xaudio2_9`) and validates every layer on real GPUs with zero retail files. |

---

## Verified Hardware Test Receipts

Tested on Windows 11 x64 (`MSVC 19.44 /W4 /permissive-` with **0 warnings**, Windows 11 SDK `10.0.26100.0`, dual D3D12 Ultimate GPUs — **NVIDIA GeForce RTX 4060 Laptop GPU** + **AMD Radeon 780M**):

```text
KestrelX Verification Suite
===========================
  [PASS] MicrosoftGame.config XML parser
  [PASS] Overlay VFS copy-on-write, whiteouts & path-traversal rejection
  [PASS] VEH AMD Zen 2 instruction decoder & emulator (CLZERO/MONITORX/MWAITX)
  [PASS] RDNA2 (GFX10.3) decoder, CFG IR lifter & dual Wave32/Wave64 HLSL emission
  [PASS] In-process DXC SM 6.6 compilation & DXBC/DXIL container verification
    -> Hardware verified: [0] NVIDIA GeForce RTX 4060 Laptop GPU | Wave32 | DXIL=4116 B | both CFG branches bit-exact
    -> Hardware verified: [1] AMD Radeon(TM) 780M | Wave32 | DXIL=4116 B | both CFG branches bit-exact
    -> Hardware verified: [1] AMD Radeon(TM) 780M | Wave64 | DXIL=4116 B | both CFG branches bit-exact
    -> Hardware verified: [2] NVIDIA GeForce RTX 4060 Laptop GPU | Wave32 | DXIL=4116 B | both CFG branches bit-exact
  [PASS] End-to-end RDNA2 -> IR -> HLSL -> DXC -> Real Dual-GPU Hardware Execution
    -> Hardware verified LDS+SMEM: [0] NVIDIA GeForce RTX 4060 Laptop GPU | Wave32 | 64/64 reversed+added bit-exact
    -> Hardware verified LDS+SMEM: [1] AMD Radeon(TM) 780M | Wave32 | 64/64 reversed+added bit-exact
    -> Hardware verified LDS+SMEM: [1] AMD Radeon(TM) 780M | Wave64 | 64/64 reversed+added bit-exact
    -> Hardware verified LDS+SMEM: [2] NVIDIA GeForce RTX 4060 Laptop GPU | Wave32 | 64/64 reversed+added bit-exact
  [PASS] RDNA2 SMEM + DS (LDS) + s_barrier Workgroup Shuffle on Dual-GPU Hardware
  [PASS] RDNA2 EXP Vertex & Pixel (vs_6_6 / ps_6_6) Decode, IR Lift & DXC Compile
    -> Textured Quad verified: [0] NVIDIA GeForce RTX 4060 Laptop GPU | MUBUF VS + MIMG PS | 256x256 4-quadrant checkerboard | max_delta=0/255
    -> Textured Quad verified: [1] AMD Radeon(TM) 780M | MUBUF VS + MIMG PS | 256x256 4-quadrant checkerboard | max_delta=0/255
    -> Textured Quad verified: [2] NVIDIA GeForce RTX 4060 Laptop GPU | MUBUF VS + MIMG PS | 256x256 4-quadrant checkerboard | max_delta=0/255
  [PASS] RDNA2 MUBUF/MTBUF + MIMG (image_sample) Textured Quad on Dual-GPU Hardware
  [PASS] Strict recompiler refusal of unsupported RDNA2 opcodes
  [PASS] Persistent on-disk DXIL shader cache & corruption recovery
  [PASS] D3D12.X ID3D12Device1..10 QI, Stream PSO Recomp & CmdListProxy Telemetry
  [PASS] GFX10 2D texture swizzle/deswizzle round-trip (all 7 modes)
  [PASS] GFX10 BC1/BC7 block-compressed tiling & 5-level mip-chain layout
  [PASS] GameInput -> XInput/Win32 controller bridge & mock verification
  [PASS] XAudio2 stereo PCM ring-buffer, RMS energy & underrun verification
  [PASS] KernelX CreateFile2/ReadFileEx Async VFS I/O & WASAPI Thread Lifecycle
    -> Guest title PASS: HLE_imports=22, GuestDLL_imports=2, TLS=1, D3D12X(dev=1, q=1, res=2, draws=1, barriers=2, cs_pso=1, gfx_pso=2, cache_hit=3, present=1), GI(reads=1), XA2(frames=64)
  [PASS] End-to-end PE32+ Loader + Guest DLL + Wide-VFS + GameInput + XAudio2 + RDNA2 VS/PS First Light
    -> Hardware [0] NVIDIA GeForce RTX 4060 Laptop GPU PASS (Wave Intrinsics bit-exact across 32 lanes)
    -> Hardware [1] AMD Radeon(TM) 780M PASS (Wave Intrinsics bit-exact across 32 lanes)
    -> Hardware [2] NVIDIA GeForce RTX 4060 Laptop GPU PASS (Wave Intrinsics bit-exact across 32 lanes)
  [PASS] RDNA2 Wave Intrinsics (WaveReadLaneFirst/At, WavePrefixCountBits, Ballot) on Dual-GPU Hardware
  [PASS] RDNA2 Multi-Texture & Multi-Buffer Dynamic Descriptor Remapping
    -> Hardware [0] NVIDIA GeForce RTX 4060 Laptop GPU PASS (3D Perspective Cube: 7222 lit pixels, DSV D32_FLOAT verified)
    -> Hardware [1] AMD Radeon(TM) 780M PASS (3D Perspective Cube: 7224 lit pixels, DSV D32_FLOAT verified)
    -> Hardware [2] NVIDIA GeForce RTX 4060 Laptop GPU PASS (3D Perspective Cube: 7222 lit pixels, DSV D32_FLOAT verified)
  [PASS] RDNA2 Depth-Buffered 3D Lit & Textured Perspective Cube on Dual-GPU Hardware
  [PASS] GDK XUser/XStore/XPackage & DirectStorage VFS Async Bridge
    -> Hardware [0] NVIDIA GeForce RTX 4060 Laptop GPU PASS (2D Workgroup 16x16 Tiling bit-exact across 256 threads)
    -> Hardware [1] AMD Radeon(TM) 780M PASS (2D Workgroup 16x16 Tiling bit-exact across 256 threads)
    -> Hardware [2] NVIDIA GeForce RTX 4060 Laptop GPU PASS (2D Workgroup 16x16 Tiling bit-exact across 256 threads)
  [PASS] RDNA2 2D/3D Compute Workgroup Tiling & LDS Spatial Exchange on Dual-GPU Hardware
    -> Hardware [0] NVIDIA GeForce RTX 4060 Laptop GPU PASS (PBR Deferred GBuffer 3-MRT bit-exact)
    -> Hardware [1] AMD Radeon(TM) 780M PASS (PBR Deferred GBuffer 3-MRT bit-exact)
    -> Hardware [2] NVIDIA GeForce RTX 4060 Laptop GPU PASS (PBR Deferred GBuffer 3-MRT bit-exact)
  [PASS] RDNA2 PBR Material Deferred GBuffer (Albedo, Normal, Material) MRT Pipeline on Dual-GPU Hardware
  [PASS] D3D12.X Async Compute Queue & Inter-Queue Fence Synchronization
  [PASS] GameInput Dual-Motor & Trigger Force Feedback Rumble Bridge
  [PASS] Spatial Audio 5.1 & 7.1 Multi-Channel Surround Downmix Matrix (ITU-R BS.775)
  [PASS] GDK Multi-User Profile Identity, Controller Binding & Isolated S:\ Saves
  [PASS] RDNA2 Hardware Raytracing BVH (IMAGE_BVH_INTERSECT_RAY) Decode, IR Lift & SM 6.6 RayQuery DXIL

  [PASS] GDK Agility SDK Query, Dynamic DLL Redirection & Modern D3D12/D3D12Core HLE Export Shims
Summary: 61 passed, 0 failed
```

CLI cold vs warm persistent disk-cache verification:
```text
# Cold run (compiles CS + VS + PS via DXC & writes .kxdxil to disk):
$ .\build\Release\kestrel.exe run build\Release\test_title --cache-dir %TEMP%\kx_cache
Loaded kestrel_test_title.exe @ 0x0000000140000000 (relocs=0, hle_imports=22, guest_imports=2, host_imports=8, stubs=0)
Guest exited with code 0 | D3D12.X(dev=1, q=1, cmd=1, res=2, draws=1, barriers=2, cs_pso=1, gfx_pso=2, cache_hit=3, disk_hit=0, present=1) | KernelX(allocs=1, peak=4096 B, async_rd=0) | GameInput(reads=1) | XAudio2(frames=64)

# Warm run (0 DXC compiles — all 3 shaders restored instantaneously from disk cache):
$ .\build\Release\kestrel.exe run build\Release\test_title --cache-dir %TEMP%\kx_cache
Loaded kestrel_test_title.exe @ 0x0000000140000000 (relocs=0, hle_imports=22, guest_imports=2, host_imports=8, stubs=0)
Guest exited with code 0 | D3D12.X(dev=1, q=1, cmd=1, res=2, draws=1, barriers=2, cs_pso=0, gfx_pso=0, cache_hit=6, disk_hit=3, present=1) | KernelX(allocs=1, peak=4096 B, async_rd=0) | GameInput(reads=1) | XAudio2(frames=64)
```

---

## Building & Running

### Requirements
- Windows 10/11 x64
- Visual Studio 2022 (or Build Tools) with C++ x64 toolset + CMake + Ninja
- Windows 11 SDK (`10.0.22621.0` or newer, includes `d3d12.h`, `dxcapi.h`, `dxcompiler.dll`)
- Any D3D12 Feature Level 12.0+ GPU (SM 6.6 recommended for shader tests)

### One-Command Build & Test
```powershell
powershell -ExecutionPolicy Bypass -File scripts/build.ps1 -Config Release
```

### CLI Usage
```powershell
# List D3D12 adapters and Wave32/Wave64 capabilities
.\build\Release\kestrel.exe adapters

# Inspect a title directory (MicrosoftGame.config + PE32+ imports)
.\build\Release\kestrel.exe inspect build\Release\test_title

# Disassemble raw RDNA2 (GFX10.3) microcode
.\build\Release\kestrel.exe disasm path\to\shader.bin

# Run RDNA2 -> IR -> HLSL -> DXC -> GPU hardware self-test on all adapters
.\build\Release\kestrel.exe selftest

# Run the clean-room guest test title (headless BMP capture, live DXGI window, WASAPI audio, custom DXIL disk cache)
.\build\Release\kestrel.exe run build\Release\test_title --capture first_light.bmp
.\build\Release\kestrel.exe run build\Release\test_title --windowed --audio --cache-dir .\shader_cache
```

---

## Repository Layout

```text
include/kestrel/
  common/    # Result<T>, hex/UTF-8 helpers
  loader/    # MicrosoftGame.config parser, PE32+ parser, native PeLoader
  cpu/       # VEH AMD Zen 2 instruction decoder & emulator
  vfs/       # Copy-on-Write Overlay VFS with whiteouts
  hle/       # HLE registry + stub thunks, kernelx (async I/O), xgameruntime, d3d12x (Dev1..10 + CmdListProxy), WindowHost, GameInput, XAudio2 (WASAPI)
  gpu/       # RDNA2 ISA decoder (SOP/VOP/FLAT/SMEM/DS/MUBUF/MTBUF/MIMG/EXP), DXBC parser, CFG IR, multi-stage HLSL recompiler, DiskShaderCache, DXC, GPU runner, GFX10 tiling
src/         # Corresponding C++23 implementations + CLI (src/cli/main.cpp)
tests/       # 18-test unit/dual-GPU verification runner + clean-room Xbox-ABI guest test title & fixtures
docs/        # Architecture documentation & hardware-captured renders (first_light.png, textured_quad.png)
```

---

## Contributing

Contributions are warmly welcome! Join our official **[Discord Community](https://discord.gg/KxsNwBwhVs)** to discuss architecture, shader compilation, and collaborate in real-time. Every subsystem is modular and backed by fast, self-contained tests that run without any proprietary game files. See [CONTRIBUTING.md](CONTRIBUTING.md) and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), join the [Welcome Discussion](https://github.com/Batushn/KestrelX/discussions/9), or pick up one of the open [`good first issue`](https://github.com/Batushn/KestrelX/issues?q=is%3Aissue+is%3Aopen+label%3A%22good+first+issue%22) tasks.

## Legal & Clean-Room Policy

KestrelX is an independent, clean-room open-source interoperability project licensed under the [MIT License](LICENSE). This repository contains **zero** copyrighted game files, **zero** proprietary SDK headers or libraries, **zero** decryption keys, and **zero** code derived from Microsoft or third-party proprietary software. Do not submit pull requests or issues containing copyrighted assets, keys, retail logs, or pirated material.
