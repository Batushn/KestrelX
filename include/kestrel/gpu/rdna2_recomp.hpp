#pragma once

#include "kestrel/gpu/ir.hpp"

#include <cstdint>
#include <string>

namespace kestrel::gpu {

enum class WaveMode : uint8_t {
    Wave32 = 32,  // Optimal for NVIDIA (Warp 32) and RDNA Wave32
    Wave64 = 64,  // Native match for AMD RDNA2/3 Wave64
};

enum class ShaderStage : uint8_t {
    Compute,
    Vertex,
    Pixel,
};

struct RecompOptions {
    ShaderStage stage = ShaderStage::Compute;
    WaveMode wave_mode = WaveMode::Wave32;
    uint32_t group_size_x = 64;
    uint32_t group_size_y = 1;
    uint32_t group_size_z = 1;
    std::string entry_point = "main";
};

struct RecompResult {
    std::string hlsl;
    uint32_t blocks_emitted = 0;
    uint32_t vgpr_count = 0;
    uint32_t sgpr_count = 0;
    WaveMode wave_mode = WaveMode::Wave32;
};

// Emit SM 6.6 HLSL compute shader from lifted KestrelX CFG IR.
// Memory convention for compute kernels:
//   - Input thread ID in v0 (SV_DispatchThreadID.x)
//   - Linear global memory buffer bound at u0 (RWByteAddressBuffer g_mem)
//   - Optional push/root constants at b0 (cbuffer KxRoot : register(b0))
RecompResult emit_hlsl_from_ir(const IrFunction& fn, const RecompOptions& opt = {});

}  // namespace kestrel::gpu
