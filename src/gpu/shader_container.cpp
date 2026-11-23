#include "kestrel/gpu/shader_container.hpp"

#include <cstring>

namespace kestrel::gpu {

Result<ShaderContainer> parse_dxbc_container(const uint8_t* data, size_t size) {
    if (!data || size < 32 || std::memcmp(data, "DXBC", 4) != 0) {
        return fail("Invalid DXBC container header");
    }
    uint32_t declared_size = 0, chunk_count = 0;
    std::memcpy(&declared_size, data + 24, 4);
    std::memcpy(&chunk_count, data + 28, 4);
    if (chunk_count == 0 || chunk_count > 1024) {
        return fail("Invalid DXBC chunk count");
    }
    const size_t toc_end = 32ull + static_cast<size_t>(chunk_count) * 4ull;
    if (toc_end > declared_size || declared_size > size) {
        return fail("DXBC container size/TOC out of bounds");
    }

    ShaderContainer out;
    out.total_size = declared_size;
    out.chunks.reserve(chunk_count);
    for (uint32_t i = 0; i < chunk_count; ++i) {
        uint32_t off = 0;
        std::memcpy(&off, data + 32 + i * 4, 4);
        if (off < toc_end || static_cast<size_t>(off) + 8 > declared_size) {
            return fail("DXBC chunk offset out of bounds");
        }
        uint32_t csize = 0;
        std::memcpy(&csize, data + off + 4, 4);
        if (static_cast<size_t>(off) + 8 + csize > declared_size) {
            return fail("DXBC chunk payload out of bounds");
        }
        DxbcChunk c;
        c.fourcc.assign(reinterpret_cast<const char*>(data + off), 4);
        c.offset = off;
        c.size = csize;
        c.data.assign(data + off + 8, data + off + 8 + csize);
        if (c.fourcc == "DXIL" && c.size >= 24) {
            out.has_dxil = true;
            uint32_t prog_ver = 0;
            std::memcpy(&prog_ver, c.data.data(), 4);
            out.dxil_stage = static_cast<uint16_t>(prog_ver >> 16);
        }
        if (c.fourcc == "XDAT" || c.fourcc == "XBCH" || c.fourcc == "XBIN") {
            out.has_xbox_native = true;
        }
        out.chunks.push_back(std::move(c));
    }
    return out;
}

}  // namespace kestrel::gpu
