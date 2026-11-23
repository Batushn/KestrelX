# Contributing to KestrelX

Thank you for your interest in contributing to **KestrelX**! This project is intentionally structured so contributors can work on isolated subsystems (PE loader, HLE APIs, RDNA2 ISA decoding, CFG IR passes, HLSL codegen, GFX10 tiling, or D3D12.X COM proxies) and validate their work locally with zero proprietary game files.

## Clean-Room & Legal Rules (Mandatory)

1. **100% Clean-Room Code Only:** Only submit original code you wrote or permissively licensed code compatible with the MIT License.
2. **No Proprietary Material:** Never commit, paste, or link to copyrighted game files, console firmware/OS binaries, proprietary SDK headers/libs, cryptographic keys, or retail memory/shader dumps.
3. **Public Documentation Only:** Use publicly available hardware ISA manuals (e.g., AMD RDNA2 / GFX10.3 ISA documentation), public DirectX / DXIL / HLSL specifications, and clean-room unit tests.

## Development Workflow

1. Fork `Batushn/KestrelX` and create a topic branch (`git checkout -b feat/your-feature`).
2. Build and run the verification suite before and after your changes:
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts/build.ps1 -Config Release
   .\build\Release\kestrel.exe selftest
   .\build\Release\kestrel.exe run build\Release\test_title
   ```
3. Ensure zero warnings under MSVC `/W4 /permissive-`.
4. Add or extend unit/GPU tests in `tests/test_runner.cpp` (or `tests/test_title/` for end-to-end loader/HLE/D3D12.X tests).
5. Open a clear Pull Request explaining what changed, why, and the exact test output you observed.

## Good Areas to Contribute

- **RDNA2 ISA & IR (`include/kestrel/gpu/rdna2_isa.hpp`, `src/gpu/ir.cpp`, `src/gpu/rdna2_recomp.cpp`):** Add decoding + IR lifting + HLSL emission + GPU execution tests for `SMEM`, `MUBUF`, `DS` (LDS), `EXP`, or additional `VOP1/VOP2/VOP3` opcodes.
- **D3D12.X COM Proxies (`src/hle/d3d12x.cpp`):** Add `ID3D12CommandQueue` / `ID3D12GraphicsCommandList` proxy wrappers, PSO shader-recompilation interception, and swapchain presentation.
- **HLE Modules (`src/hle/`):** Expand `kernelx.dll` (wide-char VFS hooks, thread/sync APIs), `xgameruntime.dll` (`XUser`, `XPackage`, `XAsync`), audio, and input.
- **GFX10 Tiling (`src/gpu/gfx10_tiling.cpp`):** Add BCn block-compressed mipmap chain tiling, 3D/volume surfaces, and cross-validation tests.
