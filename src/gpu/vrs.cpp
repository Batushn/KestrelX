#include "kestrel/gpu/vrs.hpp"

#include <algorithm>

namespace kestrel::gpu {

namespace {

uint8_t rate_to_x_shift(VrsShadingRate rate) {
    return static_cast<uint8_t>((static_cast<uint8_t>(rate) >> 2u) & 0x3u);
}

uint8_t rate_to_y_shift(VrsShadingRate rate) {
    return static_cast<uint8_t>(static_cast<uint8_t>(rate) & 0x3u);
}

VrsShadingRate shifts_to_valid_rate(uint8_t sx, uint8_t sy) {
    if (sx > 2u) sx = 2u;
    if (sy > 2u) sy = 2u;
    // D3D12 & RDNA2 disallow 4x1 and 1x4 aspect ratios; clamp the coarser axis conservatively.
    if (sx == 2u && sy == 0u) sx = 1u;
    if (sx == 0u && sy == 2u) sy = 1u;
    return static_cast<VrsShadingRate>((sx << 2u) | sy);
}

VrsCombiner clamp_combiner(uint8_t raw) {
    switch (raw) {
        case 0: return VrsCombiner::Passthrough;
        case 1: return VrsCombiner::Override;
        case 2: return VrsCombiner::Min;
        case 3: return VrsCombiner::Max;
        case 4: return VrsCombiner::Sum;
        default: return VrsCombiner::Passthrough;
    }
}

uint32_t tile_size_to_log2(uint32_t tile_size) {
    switch (tile_size) {
        case 8u: return 3u;
        case 16u: return 4u;
        case 32u: return 5u;
        default: return 3u;
    }
}

}  // namespace

uint8_t encode_vrs_tile_rate(VrsShadingRate rate) {
    const uint8_t sx = rate_to_x_shift(rate);
    const uint8_t sy = rate_to_y_shift(rate);
    return static_cast<uint8_t>(shifts_to_valid_rate(sx, sy));
}

VrsShadingRate decode_vrs_tile_rate(uint8_t encoded) {
    const uint8_t sx = static_cast<uint8_t>((encoded >> 2u) & 0x3u);
    const uint8_t sy = static_cast<uint8_t>(encoded & 0x3u);
    return shifts_to_valid_rate(sx, sy);
}

VrsRateAxes vrs_rate_axes(VrsShadingRate rate) {
    const uint8_t sx = rate_to_x_shift(rate);
    const uint8_t sy = rate_to_y_shift(rate);
    return {static_cast<uint8_t>(1u << sx), static_cast<uint8_t>(1u << sy)};
}

VrsShadingRate vrs_rate_from_axes(uint8_t axis_x, uint8_t axis_y) {
    const uint8_t sx = (axis_x >= 4u) ? 2u : (axis_x >= 2u) ? 1u : 0u;
    const uint8_t sy = (axis_y >= 4u) ? 2u : (axis_y >= 2u) ? 1u : 0u;
    return shifts_to_valid_rate(sx, sy);
}

uint32_t encode_rdna2_vrs_control(const Rdna2VrsControl& ctrl) {
    uint32_t word = 0;
    word |= static_cast<uint32_t>(encode_vrs_tile_rate(ctrl.base_rate)) & 0xFu;
    word |= (static_cast<uint32_t>(ctrl.combiner0) & 0x7u) << 4u;
    word |= (static_cast<uint32_t>(ctrl.combiner1) & 0x7u) << 7u;
    if (ctrl.enable_vrs_image) word |= (1u << 10u);
    if (ctrl.enable_per_primitive) word |= (1u << 11u);
    word |= (tile_size_to_log2(ctrl.tile_size) & 0xFu) << 12u;
    return word;
}

Rdna2VrsControl decode_rdna2_vrs_control(uint32_t reg_word) {
    Rdna2VrsControl ctrl{};
    ctrl.base_rate = decode_vrs_tile_rate(static_cast<uint8_t>(reg_word & 0xFu));
    ctrl.combiner0 = clamp_combiner(static_cast<uint8_t>((reg_word >> 4u) & 0x7u));
    ctrl.combiner1 = clamp_combiner(static_cast<uint8_t>((reg_word >> 7u) & 0x7u));
    ctrl.enable_vrs_image = ((reg_word >> 10u) & 0x1u) != 0u;
    ctrl.enable_per_primitive = ((reg_word >> 11u) & 0x1u) != 0u;
    const uint32_t log2_tile = (reg_word >> 12u) & 0xFu;
    if (log2_tile >= 3u && log2_tile <= 5u) {
        ctrl.tile_size = 1u << log2_tile;
    } else {
        ctrl.tile_size = 8u;
    }
    return ctrl;
}

VrsShadingRate combine_shading_rates(VrsShadingRate a, VrsShadingRate b, VrsCombiner combiner) {
    const uint8_t ax = rate_to_x_shift(a);
    const uint8_t ay = rate_to_y_shift(a);
    const uint8_t bx = rate_to_x_shift(b);
    const uint8_t by = rate_to_y_shift(b);

    switch (combiner) {
        case VrsCombiner::Passthrough:
            return shifts_to_valid_rate(ax, ay);
        case VrsCombiner::Override:
            return shifts_to_valid_rate(bx, by);
        case VrsCombiner::Min:
            return shifts_to_valid_rate(std::min(ax, bx), std::min(ay, by));
        case VrsCombiner::Max:
            return shifts_to_valid_rate(std::max(ax, bx), std::max(ay, by));
        case VrsCombiner::Sum: {
            const uint8_t sx = static_cast<uint8_t>(std::min<uint32_t>(2u, static_cast<uint32_t>(ax) + bx));
            const uint8_t sy = static_cast<uint8_t>(std::min<uint32_t>(2u, static_cast<uint32_t>(ay) + by));
            return shifts_to_valid_rate(sx, sy);
        }
        default:
            return shifts_to_valid_rate(ax, ay);
    }
}

D3D12_SHADING_RATE to_d3d12_shading_rate(VrsShadingRate rate) {
    switch (rate) {
        case VrsShadingRate::Rate1x1: return D3D12_SHADING_RATE_1X1;
        case VrsShadingRate::Rate1x2: return D3D12_SHADING_RATE_1X2;
        case VrsShadingRate::Rate2x1: return D3D12_SHADING_RATE_2X1;
        case VrsShadingRate::Rate2x2: return D3D12_SHADING_RATE_2X2;
        case VrsShadingRate::Rate2x4: return D3D12_SHADING_RATE_2X4;
        case VrsShadingRate::Rate4x2: return D3D12_SHADING_RATE_4X2;
        case VrsShadingRate::Rate4x4: return D3D12_SHADING_RATE_4X4;
        default: return D3D12_SHADING_RATE_1X1;
    }
}

VrsShadingRate from_d3d12_shading_rate(D3D12_SHADING_RATE rate) {
    return decode_vrs_tile_rate(static_cast<uint8_t>(rate));
}

D3D12_SHADING_RATE_COMBINER to_d3d12_combiner(VrsCombiner combiner) {
    switch (combiner) {
        case VrsCombiner::Passthrough: return D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
        case VrsCombiner::Override: return D3D12_SHADING_RATE_COMBINER_OVERRIDE;
        case VrsCombiner::Min: return D3D12_SHADING_RATE_COMBINER_MIN;
        case VrsCombiner::Max: return D3D12_SHADING_RATE_COMBINER_MAX;
        case VrsCombiner::Sum: return D3D12_SHADING_RATE_COMBINER_SUM;
        default: return D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
    }
}

VrsCombiner from_d3d12_combiner(D3D12_SHADING_RATE_COMBINER combiner) {
    return clamp_combiner(static_cast<uint8_t>(combiner));
}

VrsShadingRateImage build_vrs_shading_rate_image(
    uint32_t width, uint32_t height, uint32_t tile_size,
    const std::function<VrsShadingRate(uint32_t tile_x, uint32_t tile_y, uint32_t tiles_x, uint32_t tiles_y)>& rate_fn) {
    VrsShadingRateImage img{};
    img.render_width = width;
    img.render_height = height;
    img.tile_size = (tile_size == 0u) ? 8u : tile_size;
    if (width == 0u || height == 0u) return img;

    img.tiles_x = (width + img.tile_size - 1u) / img.tile_size;
    img.tiles_y = (height + img.tile_size - 1u) / img.tile_size;
    img.data.resize(static_cast<size_t>(img.tiles_x) * img.tiles_y);

    for (uint32_t ty = 0; ty < img.tiles_y; ++ty) {
        for (uint32_t tx = 0; tx < img.tiles_x; ++tx) {
            const VrsShadingRate rate = rate_fn ? rate_fn(tx, ty, img.tiles_x, img.tiles_y) : VrsShadingRate::Rate1x1;
            img.data[static_cast<size_t>(ty) * img.tiles_x + tx] = encode_vrs_tile_rate(rate);
        }
    }
    return img;
}

VrsShadingRateImage build_vrs_shading_rate_image(
    uint32_t width, uint32_t height, uint32_t tile_size, VrsShadingRate fill_rate) {
    return build_vrs_shading_rate_image(
        width, height, tile_size,
        [fill_rate](uint32_t, uint32_t, uint32_t, uint32_t) { return fill_rate; });
}

}  // namespace kestrel::gpu
