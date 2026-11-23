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
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bpp = 4;  // bytes per pixel / block (1, 2, 4, 8, 16)
    SwizzleMode swizzle = SwizzleMode::Linear;
    uint32_t bank_xor = 0;
};

// Compute tiled byte offset for a 2D coordinate (x, y) using clean-room GFX10 micro/macro-tile equations.
size_t compute_gfx10_offset(const SurfaceDesc& desc, uint32_t x, uint32_t y);
size_t compute_gfx10_surface_size(const SurfaceDesc& desc);

std::vector<uint8_t> swizzle_2d(const SurfaceDesc& desc, const uint8_t* linear_src, size_t linear_pitch);
std::vector<uint8_t> deswizzle_2d(const SurfaceDesc& desc, const uint8_t* tiled_src, size_t tiled_size);

}  // namespace kestrel::gpu
