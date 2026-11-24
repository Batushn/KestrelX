#pragma once

#include "kestrel/common/types.hpp"
#include "kestrel/gpu/rdna2_recomp.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kestrel::gpu {

struct ShaderCensusOptions {
    WaveMode wave_mode = WaveMode::Wave32;
    bool compile_dxil = true;
    size_t max_shaders = 0;  // 0 = unlimited
};

struct OpcodeCount {
    std::string mnemonic;
    uint64_t count = 0;
};

struct StageCensusBreakdown {
    uint64_t total = 0;
    uint64_t decoded_ok = 0;
    uint64_t ir_lifted_ok = 0;
    uint64_t hlsl_emitted_ok = 0;
    uint64_t dxc_compiled_ok = 0;
};

struct ShaderCensusReport {
    std::string input_path;
    WaveMode wave_mode = WaveMode::Wave32;
    bool dxc_enabled = false;
    bool dxc_available = false;

    uint64_t files_scanned = 0;
    uint64_t sgd5_archives = 0;
    uint64_t total_entries = 0;
    uint64_t unpacked_ok = 0;
    uint64_t unpack_failed = 0;
    uint64_t decoded_ok = 0;
    uint64_t ir_lifted_ok = 0;
    uint64_t hlsl_emitted_ok = 0;
    uint64_t dxc_compiled_ok = 0;
    uint64_t already_dxil = 0;

    uint64_t total_instructions = 0;
    uint64_t unknown_instructions = 0;
    uint64_t total_dxil_bytes = 0;

    StageCensusBreakdown compute;
    StageCensusBreakdown vertex;
    StageCensusBreakdown pixel;
    StageCensusBreakdown mesh;
    StageCensusBreakdown amplification;

    std::vector<OpcodeCount> top_unknown_opcodes;
    std::vector<std::string> sample_failures;
};

Result<ShaderCensusReport> run_shader_census(
    const std::filesystem::path& target,
    const ShaderCensusOptions& opt = {});

std::string format_shader_census_text(const ShaderCensusReport& report);
std::string format_shader_census_json(const ShaderCensusReport& report);

}  // namespace kestrel::gpu
