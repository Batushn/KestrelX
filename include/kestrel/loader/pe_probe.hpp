#pragma once

#include "kestrel/common/types.hpp"
#include "kestrel/hle/registry.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kestrel::loader {

struct PeModuleProbe {
    std::string dll_name;
    std::string redirected_name;
    bool is_delay_load = false;
    bool hle_registered = false;
    bool guest_dll_found = false;
    bool host_dll_found = false;

    uint32_t total_symbols = 0;
    uint32_t hle_resolved = 0;
    uint32_t guest_resolved = 0;
    uint32_t host_resolved = 0;
    uint32_t missing_symbols = 0;

    std::vector<std::string> missing_symbol_names;
};

struct PeProbeReport {
    std::string target_path;
    std::string image_name;
    uint64_t image_base = 0;
    uint32_t size_of_image = 0;
    uint32_t entry_point_rva = 0;
    uint16_t subsystem = 0;
    bool has_relocations = false;
    bool has_pdata_seh = false;
    bool has_tls = false;
    bool agility_sdk_detected = false;
    uint32_t agility_sdk_version = 0;
    std::string agility_sdk_path;

    uint32_t total_modules = 0;
    uint32_t total_imports = 0;
    uint32_t hle_resolved = 0;
    uint32_t guest_resolved = 0;
    uint32_t host_resolved = 0;
    uint32_t missing_imports = 0;

    std::vector<PeModuleProbe> modules;
};

Result<PeProbeReport> run_pe_probe(
    const std::filesystem::path& target,
    const hle::Registry& hle);

std::string format_pe_probe_text(const PeProbeReport& report);
std::string format_pe_probe_json(const PeProbeReport& report);

}  // namespace kestrel::loader
