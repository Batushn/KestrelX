#pragma once

#include "kestrel/common/types.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace kestrel::loader {

struct PeSection {
    std::string name;
    uint32_t virtual_size = 0;
    uint32_t virtual_address = 0;
    uint32_t raw_size = 0;
    uint32_t raw_offset = 0;
    uint32_t characteristics = 0;
};

struct PeImportSymbol {
    bool by_ordinal = false;
    uint16_t ordinal = 0;
    std::string name;
    uint32_t iat_rva = 0;
};

struct PeImportModule {
    std::string dll_name;
    bool is_delay_load = false;
    std::vector<PeImportSymbol> symbols;
};

struct PeExportSymbol {
    uint32_t ordinal = 0;
    std::string name;
    uint32_t rva = 0;
    std::string forwarder;
};

struct PeDataDir {
    uint32_t rva = 0;
    uint32_t size = 0;
};

struct PeImage {
    std::string filename;
    std::vector<uint8_t> raw;
    uint16_t machine = 0;
    uint16_t characteristics = 0;
    uint16_t subsystem = 0;
    uint64_t image_base = 0;
    uint32_t size_of_image = 0;
    uint32_t size_of_headers = 0;
    uint32_t entry_point_rva = 0;
    bool is_dll = false;

    std::vector<PeDataDir> data_directories;
    std::vector<PeSection> sections;
    std::vector<PeImportModule> imports;
    std::vector<PeExportSymbol> exports;

    std::optional<size_t> rva_to_file_offset(uint32_t rva) const;
    std::optional<std::string> read_cstring_at_rva(uint32_t rva, size_t max_len = 512) const;
};

Result<PeImage> parse_pe64(std::vector<uint8_t> bytes, std::string filename = {});
Result<PeImage> parse_pe64_file(const std::filesystem::path& path);

}  // namespace kestrel::loader
