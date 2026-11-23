<div align="center">

# KestrelX

**Clean-Room Xbox Series X|S Native Compatibility & Dynamic RDNA2 (GFX10.3) -> SM 6.6 DXIL Shader Recompilation Runtime**

[![Release: v0.4.0](https://img.shields.io/badge/Release-v0.4.0-2ea44f?style=for-the-badge)](https://github.com/Batushn/KestrelX/releases/tag/v0.4.0)
[![Tests: 18/18 PASS](https://img.shields.io/badge/Hardware_Tests-18%2F18_PASS-brightgreen?style=for-the-badge)](#verified-hardware-test-receipts)
[![Dual-GPU: NVIDIA + AMD](https://img.shields.io/badge/Dual--GPU-RTX_4060_%2B_Radeon_780M-0078D6?style=for-the-badge)](#hardware-visual-showcase)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C?style=for-the-badge)](CMakeLists.txt)
[![D3D12 Ultimate](https://img.shields.io/badge/D3D12_Ultimate-SM_6.6_DXIL-107C10?style=for-the-badge)](docs/ARCHITECTURE.md)
[![License: MIT](https://img.shields.io/badge/License-MIT_Clean--Room-blue?style=for-the-badge)](LICENSE)

*Built from scratch in C++23 under the MIT license — zero proprietary code, zero game assets, zero cryptographic keys.*

[Visual Showcase](#hardware-visual-showcase) • [Progress Dashboard](#live-progress-dashboard) • [Architecture](#architecture) • [Hardware Receipts](#verified-hardware-test-receipts) • [Build & Run](#building--running) • [Contribute](#contributing)

</div>

---

## Hardware Visual Showcase

Every image below is captured directly from real **D3D12 Ultimate** hardware after KestrelX dynamically decodes raw **GFX10.3 (RDNA2)** microcode, lifts it into CFG basic-block IR, emits SM 6.6 HLSL, compiles signed DXIL in-process via `dxcompiler.dll`, and renders on both **NVIDIA GeForce RTX 4060** and **AMD Radeon 780M**:

<table>
  <tr>
    <td align="center" width="50%">
      <img src="docs/first_light.png" alt="KestrelX v0.3.0 Visual First Light" width="256"/><br/>
      <b>v0.3.0 — Visual First Light (RGB Triangle)</b><br/>
      <sub>Raw RDNA2 <code>KXRV</code>/<code>KXRP</code> (<code>EXP pos0/param0/mrt0</code>) via <code>kestrel_test_title.exe</code> -> <code>KxDeviceProxy</code> -> <code>D3D12PresentX</code><br/>
      Dual-GPU max channel delta: <b>1/255</b> (RTX 4060 vs Radeon 780M)</sub>
    </td>
    <td align="center" width="50%">
      <img src="docs/textured_quad.png" alt="KestrelX v0.4.0 RDNA2 MUBUF + MIMG Textured Quad" width="256"/><br/>
      <b>v0.4.0 — Textured Quad (MUBUF + MIMG)</b><br/>
      <sub>Raw RDNA2 <code>buffer_load_dwordx4</code> Vertex Shader (<code>t1</code>) + <code>image_sample</code> Pixel Shader (<code>t0</code>/<code>s0</code>)<br/>
      Dual-GPU max channel delta: <b>0/255 bit-exact</b> (RTX 4060 vs Radeon 780M)</sub>
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
    M4["✅ v0.4.0 (Current)<br/>MUBUF/MTBUF + MIMG Quad,<br/>Disk DXIL Cache, Dev1..10, WASAPI"]
    M5["🚧 v0.5.0 (Next)<br/>Wave Intrinsics, 3D Cube + Depth,<br/>Root Sig Remap, GDK XUser/XStore"]
    M6["📋 v0.6.0+<br/>Mesh/Amp Shaders (ms_6_6),<br/>DirectStorage, Commercial Testing"]

    M1 --> M2 --> M3 --> M4 --> M5 --> M6

    style M1 fill:#1f6f3b,stroke:#2ea44f,color:#fff
    style M2 fill:#1f6f3b,stroke:#2ea44f,color:#fff
    style M3 fill:#1f6f3b,stroke:#2ea44f,color:#fff
    style M4 fill:#107c10,stroke:#56d364,stroke-width:3px,color:#fff
    style M5 fill:#9a6700,stroke:#d29922,color:#fff
    style M6 fill:#30363d,stroke:#8b949e,color:#c9d1d9
```

### Subsystem Completion Matrix

| Subsystem | Progress | Status | Implemented Today | Next Up |
| :--- | :--- | :---: | :--- | :--- |
| **PE32+ Loader & Manifest** | `█████████░ 90%` | ✅ Ready | `MicrosoftGame.config` XML, PE32+ relocs, guest DLL chaining, HLE IAT + stub thunks, `.pdata` SEH, TLS callbacks | Delay-load imports & ordinal-heavy GDK DLLs |
| **CPU VEH Zen 2 Shim** | `█████████░ 90%` | ✅ Ready | `AddVectoredExceptionHandler` decoder & emulator for AMD Zen 2 `CLZERO`, `MONITORX`, `MWAITX` | `RDPRU` / `MCOMMIT` edge cases |
| **Copy-on-Write Overlay VFS** | `█████████░ 95%` | ✅ Ready | `GAME:\`, `G:\`, `D:\`, `S:\`, `T:\` routing, CoW overlay, whiteouts, `..` traversal rejection | Case-insensitive path lookup cache |
| **RDNA2 -> IR -> SM 6.6 DXIL** | `███████░░░ 70%` | 🟢 Active | `SOP`/`VOP`/`FLAT`/`SMEM`/`DS` (LDS) + `s_barrier` + `EXP` (VS/PS) + `MUBUF`/`MTBUF` + `MIMG` (`image_sample`/`load`/`resinfo`), dual Wave32/Wave64, strict unsupported-op refusal | Wave intrinsics (`v_readlane_b32`, `s_and_saveexec`), Depth/Cube 3D, `ms_6_6`/`as_6_6` |
| **Persistent DXIL Shader Cache** | `█████████░ 90%` | ✅ Ready | In-memory + atomic on-disk `.kxdxil` cache (`%LOCALAPPDATA%/KestrelX/cache`), FNV-1a corruption eviction, `--cache-dir` | LRU disk size quota & cache stats CLI |
| **D3D12.X COM Proxy (`d3d12_xs`)** | `███████░░░ 70%` | 🟢 Active | Pure COM `KxDeviceProxy` (`ID3D12Device`–`Device2` + `Device3..10` QI), `CreatePipelineState` stream auto-recomp, `KxCommandQueueProxy`, `KxGraphicsCommandListProxy` telemetry | Descriptor heap & root-signature translation, enhanced barriers |
| **WindowHost & PresentX** | `████████░░ 80%` | ✅ Ready | Win32 `WindowHost` + `IDXGISwapChain3` flip-model (`--windowed`), headless GPU readback BMP capture (`--capture`), `D3D12PresentX` | Resize/fullscreen toggle & frame-pacing HUD |
| **Input (`gameinput.dll`)** | `████████░░ 80%` | ✅ Ready | COM `GameInputCreate`, dynamic `xinput1_4.dll`, Win32 keyboard fallback, deterministic mock provider | Rumble/haptics & multi-controller slots |
| **Audio (`xaudio2_9.dll`)** | `████████░░ 80%` | ✅ Ready | COM `XAudio2Create`, mastering/source voices, 96k-frame stereo float32/int16 ring buffer, RMS metering, live WASAPI shared-mode thread (`--audio`) | Submix voices & 3D spatialization (`X3DAudio`) |
| **KernelX & GDK HLE** | `██████░░░░ 60%` | 🟡 Growing | `XMemAlloc`/`Free`/`Size`, `TitleMemoryStatus`, `CreateFileA/W/2`, `ReadFileEx` async overlapped I/O, `XTaskQueue*`, `XGameGetXboxTitleId` | `XUser`, `XStore`, `XPackage`, DirectStorage stubs |
| **GFX10 Texture Tiling** | `████████░░ 85%` | ✅ Ready | Clean-room 2D swizzle/deswizzle (all 7 modes), BC1/BC7 4×4 block tiling, 256B-aligned multi-level mip-chains | 3D/Array volume tiling & live upload hook |

> **Honest Scope Note (`v0.4.0`):** KestrelX executes our clean-room Xbox-ABI test title, dynamically recompiles RDNA2 Compute/Vertex/Pixel shaders (`SMEM`, `LDS`, `EXP`, `MUBUF`, `MTBUF`, `MIMG`) on real NVIDIA & AMD GPUs, and persists compiled DXIL to disk. It does **not** yet boot commercial retail Xbox Series X|S games — closing that gap requires wave intrinsics, dynamic descriptor/root-signature remapping, mesh shaders, and broader GDK HLE coverage. Pick up any open issue to help build the next milestone!

---

## Architecture

```mermaid
flowchart LR
    A["Title Dir<br/>MicrosoftGame.config + PE32+"] --> B["PeLoader + Overlay VFS<br/>Relocs · IAT · .pdata SEH · TLS"]
    B --> C["Native x64 Execution<br/>+ VEH Zen 2 Shim<br/>(CLZERO / MONITORX / MWAITX)"]
    C --> D["HLE Runtime<br/>kernelx (Async I/O) · xgameruntime<br/>gameinput · xaudio2 (WASAPI)"]
    C --> E["d3d12_xs COM Proxy<br/>KxDeviceProxy (Dev1..10)<br/>KxGraphicsCommandListProxy"]
    E --> F["RDNA2 (GFX10.3) Decoder<br/>SOP · VOP · FLAT · SMEM · DS<br/>MUBUF · MTBUF · MIMG · EXP"]
    F --> G["CFG Basic-Block IR<br/>-> Dual-Wave SM 6.6 HLSL<br/>Wave32 (NVIDIA) · Wave64 (AMD)"]
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
    -> Hardware verified: [0] NVIDIA GeForce RTX 4060 Laptop GPU | Wave32 | DXIL=4020 B | both CFG branches bit-exact
    -> Hardware verified: [1] AMD Radeon(TM) 780M | Wave32 | DXIL=4020 B | both CFG branches bit-exact
    -> Hardware verified: [1] AMD Radeon(TM) 780M | Wave64 | DXIL=4020 B | both CFG branches bit-exact
  [PASS] End-to-end RDNA2 -> IR -> HLSL -> DXC -> Real Dual-GPU Hardware Execution
    -> Hardware verified LDS+SMEM: [0] NVIDIA GeForce RTX 4060 Laptop GPU | Wave32 | 64/64 reversed+added bit-exact
    -> Hardware verified LDS+SMEM: [1] AMD Radeon(TM) 780M | Wave32 | 64/64 reversed+added bit-exact
    -> Hardware verified LDS+SMEM: [1] AMD Radeon(TM) 780M | Wave64 | 64/64 reversed+added bit-exact
  [PASS] RDNA2 SMEM + DS (LDS) + s_barrier Workgroup Shuffle on Dual-GPU Hardware
  [PASS] RDNA2 EXP Vertex & Pixel (vs_6_6 / ps_6_6) Decode, IR Lift & DXC Compile
    -> Textured Quad verified: [0] NVIDIA GeForce RTX 4060 Laptop GPU | MUBUF VS + MIMG PS | 256x256 4-quadrant checkerboard | max_delta=0/255
    -> Textured Quad verified: [1] AMD Radeon(TM) 780M | MUBUF VS + MIMG PS | 256x256 4-quadrant checkerboard | max_delta=0/255
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

Summary: 18 passed, 0 failed
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

Contributions are warmly welcome! Every subsystem is modular and backed by fast, self-contained tests that run without any proprietary game files. See [CONTRIBUTING.md](CONTRIBUTING.md) and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), join the [Welcome Discussion](https://github.com/Batushn/KestrelX/discussions/9), or pick up one of the open [`good first issue`](https://github.com/Batushn/KestrelX/issues?q=is%3Aissue+is%3Aopen+label%3A%22good+first+issue%22) tasks.

## Legal & Clean-Room Policy

KestrelX is an independent, clean-room open-source interoperability project licensed under the [MIT License](LICENSE). This repository contains **zero** copyrighted game files, **zero** proprietary SDK headers or libraries, **zero** decryption keys, and **zero** code derived from Microsoft or third-party proprietary software. Do not submit pull requests or issues containing copyrighted assets, keys, retail logs, or pirated material.
