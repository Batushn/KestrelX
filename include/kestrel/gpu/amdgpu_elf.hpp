#pragma once

#include "kestrel/common/types.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace kestrel::gpu {

struct AmdgpuKernelArg {
    std::string value_kind;   // e.g. "global_buffer", "by_value", "hidden_block_count_x", "hidden_group_size_x", "hidden_global_offset_x"
    std::string type_name;
    uint32_t offset = 0;
    uint32_t size = 0;
    bool is_const = false;
};

struct AmdgpuKernelInfo {
    std::string name;
    std::string kd_symbol;
    std::string target;            // e.g. "amdgcn-amd-amdhsa--gfx1030"
    uint32_t kernarg_size = 0;
    uint32_t group_segment_bytes = 0;
    uint32_t private_segment_bytes = 0;
    uint32_t wavefront_size = 32;
    uint32_t sgpr_count = 0;
    uint32_t vgpr_count = 0;
    uint32_t user_sgpr_count = 0;
    uint32_t kernarg_sgpr = 4;     // Starting SGPR for kernarg_segment_ptr
    uint32_t wg_id_x_sgpr = 6;     // Starting SGPR for workgroup_id_x
    uint32_t wg_id_y_sgpr = 7;
    uint32_t wg_id_z_sgpr = 8;
    bool has_kernarg_ptr = false;
    bool has_wg_id_x = false;
    bool has_wg_id_y = false;
    bool has_wg_id_z = false;
    uint64_t code_va = 0;
    std::vector<uint32_t> code_words;
    std::vector<AmdgpuKernelArg> args;
};

struct AmdgpuElfImage {
    std::string target;
    std::vector<AmdgpuKernelInfo> kernels;
};

bool is_amdgpu_elf(std::span<const uint8_t> bytes);
Result<AmdgpuElfImage> parse_amdgpu_elf(std::span<const uint8_t> bytes);
Result<AmdgpuElfImage> load_amdgpu_elf_file(const std::filesystem::path& path);

// Build a packed AMDHSA kernarg buffer for `kernel` where:
//   - user_args provides sequential non-hidden argument values (64-bit offsets/values for pointers or 32-bit/64-bit scalars)
//   - group_size_x/y/z, block_count_x/y/z, global_offset_x/y/z populate AMDHSA hidden OpenCL arguments
std::vector<uint8_t> build_amdhsa_kernarg_blob(
    const AmdgpuKernelInfo& kernel,
    std::span<const uint64_t> user_arg_values,
    uint32_t block_count_x,
    uint32_t group_size_x,
    uint32_t block_count_y = 1,
    uint32_t block_count_z = 1,
    uint32_t group_size_y = 1,
    uint32_t group_size_z = 1,
    uint64_t global_offset_x = 0,
    uint64_t global_offset_y = 0,
    uint64_t global_offset_z = 0);

}  // namespace kestrel::gpu
