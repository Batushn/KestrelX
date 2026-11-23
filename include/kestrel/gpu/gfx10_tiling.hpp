#pragma once

#include <cstdint>
#include <vector>

namespace kestrel::gpu {

enum class SwizzleMode : uint8_t {
    Linear,
    Sw256B_S,
    Sw4KB_S,
    Sw64KB_S,
    Sw64KB_D,
    Sw64KB_S_X,
    Sw64KB_D_X,
};

struct SurfaceDesc {
    uint32_t width = 0;         // Width in texels (or blocks when block_width == 1)
    uint32_t height = 0;        // Height in texels (or blocks when block_height == 1)
    uint32_t bpp = 4;           // Bytes per element/block (1, 2, 4, 8 for BC1/BC4, 16 for BC3/BC7)
    SwizzleMode swizzle = SwizzleMode::Linear;
    uint32_t bank_xor = 0;
    uint32_t block_width = 1;   // 1 for uncompressed, 4 for BCn formats
    uint32_t block_height = 1;  // 1 for uncompressed, 4 for BCn formats
};

struct MipLevelLayout {
    uint32_t level = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t blocks_x = 0;
    uint32_t blocks_y = 0;
    size_t offset = 0;
    size_t size = 0;
};

// Compute tiled byte offset for a 2D element/block coordinate (x, y) using clean-room GFX10 tile equations.
size_t compute_gfx10_offset(const SurfaceDesc& desc, uint32_t x, uint32_t y);
size_t compute_gfx10_surface_size(const SurfaceDesc& desc);

// Compute 256-byte-aligned multi-mip layout for uncompressed or BCn compressed surfaces.
std::vector<MipLevelLayout> compute_gfx10_mip_chain(const SurfaceDesc& base_desc, uint32_t mip_levels);

std::vector<uint8_t> swizzle_2d(const SurfaceDesc& desc, const uint8_t* linear_src, size_t linear_pitch);
std::vector<uint8_t> deswizzle_2d(const SurfaceDesc& desc, const uint8_t* tiled_src, size_t tiled_size);

}  // namespace kestrel::gpu
