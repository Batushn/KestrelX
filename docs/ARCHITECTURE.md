# KestrelX Architecture

KestrelX is organized as a modular static library (`kestrel_core`) paired with a thin CLI (`kestrel`) and a clean-room guest verification title (`kestrel_test_title.exe` + `kestrel_test_dll.dll`).

## Pipeline Overview

```text
  Title Dir (MicrosoftGame.config + PE32+ EXE/DLLs)
        │
        ▼
  [1] Loader & Overlay VFS
      ├── game_config: XML manifest -> TitleId, memory budgets, entry EXE
      ├── overlay_vfs: GAME:\ / G:\ / D:\ (CoW + whiteouts), S:\, T:\
      └── pe_loader:   VirtualAlloc -> relocs -> IAT (HLE / Guest DLL / Host / Stub thunk)
                       -> VirtualProtect -> RtlAddFunctionTable (.pdata SEH) -> TLS callbacks
        │
        ▼
  [2] Native x86-64 Execution + CPU VEH Shim
      └── veh_shim: Traps STATUS_ILLEGAL_INSTRUCTION (0xC000001D) on non-AMD CPUs
                    and emulates CLZERO (0F 01 FC), MONITORX (0F 01 FA), MWAITX (0F 01 FB)
        │
        ▼
  [3] HLE Runtime & Pure COM Proxy D3D12.X
      ├── kernelx / xmem:   64B-aligned XMemAlloc/Free/Size, TitleMemoryStatus, VFS CreateFileA
      ├── xgameruntime:     TitleId, DeviceType, XTaskQueue work/completion dispatcher
      └── d3d12_xs / _x:    KxDeviceProxy wraps ID3D12Device cleanly without vtable patching
        │
        ▼
  [4] Dynamic RDNA2 (GFX10.3) Shader Recompiler
      ├── shader_container: DXBC/DXIL chunk parser
      ├── rdna2_isa:        GFX10.3 decoder + disassembler
      ├── ir:               Basic-block CFG discovery + typed IR lifting
      ├── rdna2_recomp:     IR -> SM 6.6 HLSL ([WaveSize(32)] and [WaveSize(64)])
      ├── dxc_compiler:     In-process IDxcCompiler3 (dxcompiler.dll + dxil.dll) -> signed DXIL
      └── gfx10_tiling:     2D micro/macro-tile swizzle & deswizzle
```

## Key Design Decisions

### 1. Pure COM Proxy Instead of Driver Vtable Patching
Patching live `ID3D12Device` driver vtables in-place is brittle across GPU vendors, driver updates, debug layers, and translation layers (PIX, RenderDoc, vkd3d-proton). `KxDeviceProxy` implements `ID3D12Device` via standard COM aggregation/delegation, allowing safe interception of resource creation, PSO compilation, and console-specific extensions while forwarding standard calls cleanly.

### 2. IR-First Dual-Wave Shader Recompilation
Xbox Series X|S runs on AMD RDNA2 (GFX10.3), which supports both Wave32 and Wave64 execution modes. Desktop NVIDIA GPUs only execute 32-lane warps, while AMD RDNA2/3 GPUs execute both Wave32 and Wave64 natively. Lifting RDNA2 machine code into an explicit CFG basic-block IR before emitting SM 6.6 HLSL enables:
- Target-aware `[WaveSize(32)]` vs `[WaveSize(64)]` emission from the same IR.
- Future dead-code elimination, SSA promotion, and divergence analysis passes without coupling to HLSL string formatting.
- Hardware bit-exact regression testing on both NVIDIA and AMD GPUs.

### 3. Non-Destructive Copy-on-Write Overlay VFS
All write operations targeting read-only title volumes (`GAME:\`, `G:\`, `D:\`) are transparently redirected to a per-title overlay directory under the user data root. Deletions record logical whiteouts so the source game directory is guaranteed untouched.
