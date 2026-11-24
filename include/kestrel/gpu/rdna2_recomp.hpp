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
    Mesh,
    Amplification,
};

struct RecompOptions {
    ShaderStage stage = ShaderStage::Compute;
    WaveMode wave_mode = WaveMode::Wave32;
    uint32_t group_size_x = 64;
    uint32_t group_size_y = 1;
    uint32_t group_size_z = 1;
    std::string entry_point = "main";
    uint32_t max_mesh_verts = 0;   // 0 = auto from IrFunction::max_mesh_verts (default 64)
    uint32_t max_mesh_prims = 0;   // 0 = auto from IrFunction::max_mesh_prims (default 64)
    bool mesh_has_payload = false; // true when paired with an Amplification Shader
    bool enable_ir_opt = true;
    bool emit_wave_size_attr = true;
    bool use_amdhsa_abi = false;
    uint32_t amdhsa_kernarg_sgpr = 4;
    uint32_t amdhsa_wg_id_x_sgpr = 6;
    uint32_t amdhsa_wg_id_y_sgpr = 7;
    uint32_t amdhsa_wg_id_z_sgpr = 8;
};

struct RecompResult {
    std::string hlsl;
    uint32_t blocks_emitted = 0;
    uint32_t vgpr_count = 0;
    uint32_t sgpr_count = 0;
    WaveMode wave_mode = WaveMode::Wave32;
    bool uses_cbuf = false;
    bool uses_wave_ops = false;
    uint32_t mrt_mask = 0;
    uint32_t max_mesh_verts = 0;
    uint32_t max_mesh_prims = 0;
    uint32_t mesh_param_mask = 0;
    std::vector<DescriptorSlot> tex_bindings;
    std::vector<DescriptorSlot> samp_bindings;
    std::vector<DescriptorSlot> buf_srv_bindings;
};

struct RootBindingEntry {
    enum class Kind : uint8_t {
        CbvB0,
        UavU0,
        SrvTexture,
        SrvBuffer,
        Sampler,
    };
    Kind kind = Kind::CbvB0;
    uint32_t sgpr_base = 0;
    uint32_t hlsl_reg = 0;
    uint32_t root_param_index = 0;
};

struct RootSignatureRemapPlan {
    std::vector<RootBindingEntry> entries;
    uint32_t total_srv_slots = 0;
    uint32_t total_sampler_slots = 0;
    bool needs_cbv_b0 = false;
    bool needs_uav_u0 = false;
};

// Emit SM 6.6 HLSL shader from lifted KestrelX CFG IR.
// Memory convention for compute kernels:
//   - Input thread ID in v0 (SV_DispatchThreadID.x)
//   - Linear global memory buffer bound at u0 (RWByteAddressBuffer g_mem)
//   - Optional push/root constants at b0 (cbuffer KxRoot : register(b0))
RecompResult emit_hlsl_from_ir(const IrFunction& fn, const RecompOptions& opt = {});

// Build a deterministic D3D12 root-signature / descriptor-table remap plan
// from the SGPR resource tables discovered during RDNA2 lifting.
RootSignatureRemapPlan build_root_remap_plan(const IrFunction& fn, ShaderStage stage);

}  // namespace kestrel::gpu
