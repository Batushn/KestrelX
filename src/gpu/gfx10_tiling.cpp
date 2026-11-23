#include "kestrel/gpu/gfx10_tiling.hpp"

#include <algorithm>
#include <cstring>

namespace kestrel::gpu {

namespace {

uint32_t morton2d(uint32_t x, uint32_t y) {
    uint32_t z = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        z |= ((x >> i) & 1u) << (2u * i);
        z |= ((y >> i) & 1u) << (2u * i + 1u);
    }
    return z;
}

uint32_t elem_width(const SurfaceDesc& d) {
    const uint32_t bw = std::max(1u, d.block_width);
    return (d.width + bw - 1u) / bw;
}

uint32_t elem_height(const SurfaceDesc& d) {
    const uint32_t bh = std::max(1u, d.block_height);
    return (d.height + bh - 1u) / bh;
}

void tile_dims(SwizzleMode mode, uint32_t bpp, uint32_t& tw, uint32_t& th, uint32_t& tile_bytes) {
    switch (mode) {
        case SwizzleMode::Linear: tile_bytes = bpp; tw = 1; th = 1; return;
        case SwizzleMode::Sw256B_S: tile_bytes = 256; break;
        case SwizzleMode::Sw4KB_S: tile_bytes = 4096; break;
        case SwizzleMode::Sw64KB_S:
        case SwizzleMode::Sw64KB_D:
        case SwizzleMode::Sw64KB_S_X:
        case SwizzleMode::Sw64KB_D_X:
            tile_bytes = 65536;
            break;
    }
    const uint32_t elems = std::max(1u, tile_bytes / std::max(1u, bpp));
    tw = 1;
    th = 1;
    while (tw * th < elems) {
        if (tw <= th) tw <<= 1;
        else th <<= 1;
    }
}

uint32_t intra_tile_index(SwizzleMode mode, uint32_t lx, uint32_t ly, uint32_t tw, uint32_t th, uint32_t bank_xor) {
    switch (mode) {
        case SwizzleMode::Linear:
            return 0;
        case SwizzleMode::Sw256B_S:
        case SwizzleMode::Sw4KB_S:
        case SwizzleMode::Sw64KB_S:
            return morton2d(lx, ly);
        case SwizzleMode::Sw64KB_D:
            return morton2d(ly, lx);
        case SwizzleMode::Sw64KB_S_X: {
            const uint32_t mask = (tw * th) - 1u;
            const uint32_t base = morton2d(lx, ly);
            const uint32_t hi = (base >> 4) ^ (bank_xor & (mask >> 4));
            return ((hi << 4) | (base & 0xFu)) & mask;
        }
        case SwizzleMode::Sw64KB_D_X: {
            const uint32_t mask = (tw * th) - 1u;
            const uint32_t base = morton2d(ly, lx);
            const uint32_t hi = (base >> 4) ^ (bank_xor & (mask >> 4));
            return ((hi << 4) | (base & 0xFu)) & mask;
        }
    }
    return 0;
}

}  // namespace

size_t compute_gfx10_offset(const SurfaceDesc& desc, uint32_t x, uint32_t y) {
    const uint32_t ew = elem_width(desc);
    if (desc.swizzle == SwizzleMode::Linear) {
        return (static_cast<size_t>(y) * ew + x) * desc.bpp;
    }
    uint32_t tw = 1, th = 1, tile_bytes = 256;
    tile_dims(desc.swizzle, desc.bpp, tw, th, tile_bytes);
    const uint32_t tiles_x = (ew + tw - 1) / tw;
    const uint32_t tx = x / tw;
    const uint32_t ty = y / th;
    const uint32_t lx = x % tw;
    const uint32_t ly = y % th;
    const uint32_t idx = intra_tile_index(desc.swizzle, lx, ly, tw, th, desc.bank_xor);
    return (static_cast<size_t>(ty) * tiles_x + tx) * tile_bytes + static_cast<size_t>(idx) * desc.bpp;
}

size_t compute_gfx10_surface_size(const SurfaceDesc& desc) {
    const uint32_t ew = elem_width(desc);
    const uint32_t eh = elem_height(desc);
    if (desc.swizzle == SwizzleMode::Linear) {
        return static_cast<size_t>(ew) * eh * desc.bpp;
    }
    uint32_t tw = 1, th = 1, tile_bytes = 256;
    tile_dims(desc.swizzle, desc.bpp, tw, th, tile_bytes);
    const uint32_t tiles_x = (ew + tw - 1) / tw;
    const uint32_t tiles_y = (eh + th - 1) / th;
    return static_cast<size_t>(tiles_x) * tiles_y * tile_bytes;
}

std::vector<MipLevelLayout> compute_gfx10_mip_chain(const SurfaceDesc& base_desc, uint32_t mip_levels) {
    std::vector<MipLevelLayout> out;
    size_t cur_offset = 0;
    for (uint32_t lvl = 0; lvl < mip_levels; ++lvl) {
        SurfaceDesc d = base_desc;
        d.width = std::max(1u, base_desc.width >> lvl);
        d.height = std::max(1u, base_desc.height >> lvl);
        const size_t sz = compute_gfx10_surface_size(d);
        cur_offset = (cur_offset + 255u) & ~size_t(255u);
        out.push_back(MipLevelLayout{
            lvl,
            d.width,
            d.height,
            elem_width(d),
            elem_height(d),
            cur_offset,
            sz,
        });
        cur_offset += sz;
    }
    return out;
}

std::vector<uint8_t> swizzle_2d(const SurfaceDesc& desc, const uint8_t* linear_src, size_t linear_pitch) {
    const uint32_t ew = elem_width(desc);
    const uint32_t eh = elem_height(desc);
    std::vector<uint8_t> dst(compute_gfx10_surface_size(desc), 0);
    for (uint32_t y = 0; y < eh; ++y) {
        for (uint32_t x = 0; x < ew; ++x) {
            const size_t off = compute_gfx10_offset(desc, x, y);
            if (off + desc.bpp <= dst.size()) {
                std::memcpy(dst.data() + off, linear_src + y * linear_pitch + x * desc.bpp, desc.bpp);
            }
        }
    }
    return dst;
}

std::vector<uint8_t> deswizzle_2d(const SurfaceDesc& desc, const uint8_t* tiled_src, size_t tiled_size) {
    const uint32_t ew = elem_width(desc);
    const uint32_t eh = elem_height(desc);
    const size_t linear_pitch = static_cast<size_t>(ew) * desc.bpp;
    std::vector<uint8_t> dst(linear_pitch * eh, 0);
    for (uint32_t y = 0; y < eh; ++y) {
        for (uint32_t x = 0; x < ew; ++x) {
            const size_t off = compute_gfx10_offset(desc, x, y);
            if (off + desc.bpp <= tiled_size) {
                std::memcpy(dst.data() + y * linear_pitch + x * desc.bpp, tiled_src + off, desc.bpp);
            }
        }
    }
    return dst;
}

}  // namespace kestrel::gpu
