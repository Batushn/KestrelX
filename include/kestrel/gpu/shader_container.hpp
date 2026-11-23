#pragma once

#include "kestrel/common/types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace kestrel::gpu {

struct DxbcChunk {
    std::string fourcc;
    uint32_t offset = 0;
    uint32_t size = 0;
    std::vector<uint8_t> data;
};

struct ShaderContainer {
    uint32_t total_size = 0;
    std::vector<DxbcChunk> chunks;
    bool has_dxil = false;
    bool has_xbox_native = false;
    uint16_t dxil_stage = 0xFFFF;
};

Result<ShaderContainer> parse_dxbc_container(const uint8_t* data, size_t size);

}  // namespace kestrel::gpu
