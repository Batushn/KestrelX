#pragma once

#include "kestrel/common/types.hpp"
#include "kestrel/gpu/rdna2_recomp.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace kestrel::gpu {

struct GpuAdapterInfo {
    uint32_t index = 0;
    std::string name;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint64_t dedicated_vram_mb = 0;
    uint32_t wave_lane_count_min = 0;
    uint32_t wave_lane_count_max = 0;
    uint32_t max_shader_model = 0;
    bool supports_wave32 = false;
    bool supports_wave64 = false;
    bool is_software = false;
};

struct ComputeDispatchResult {
    std::string adapter_name;
    std::vector<uint32_t> output_u32;
    double gpu_ms = 0.0;
};

// Enumerate all D3D12-capable adapters with real wave-size & SM capabilities.
Result<std::vector<GpuAdapterInfo>> enumerate_d3d12_adapters();

// Execute a compiled DXIL compute shader on a specific hardware D3D12 adapter.
// Binds `initial_buffer_u32` as RWByteAddressBuffer u0, root constants at b0, dispatches
// (groups_x, 1, 1), waits on a real GPU fence, and reads back the entire buffer.
Result<ComputeDispatchResult> run_compute_dxil_on_gpu(
    uint32_t adapter_index,
    const std::vector<uint8_t>& dxil,
    const std::vector<uint32_t>& initial_buffer_u32,
    const uint32_t root_constants[8],
    uint32_t groups_x);

}  // namespace kestrel::gpu
