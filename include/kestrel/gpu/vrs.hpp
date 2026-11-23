#pragma once

#include <d3d12.h>
#include <cstdint>
#include <functional>
#include <vector>

namespace kestrel::gpu {

// Clean-room RDNA2 / D3D12 VRS Tier 2 shading rate encoding:
// Bits [3:2] = log2(X axis rate) in {0, 1, 2} -> {1, 2, 4}
// Bits [1:0] = log2(Y axis rate) in {0, 1, 2} -> {1, 2, 4}
enum class VrsShadingRate : uint8_t {
    Rate1x1 = 0x0,
    Rate1x2 = 0x1,
    Rate2x1 = 0x4,
    Rate2x2 = 0x5,
    Rate2x4 = 0x6,
    Rate4x2 = 0x9,
    Rate4x4 = 0xA,
};

// Clean-room VRS Tier 2 rate combiner stages (per-draw/per-primitive and screen-space image).
enum class VrsCombiner : uint8_t {
    Passthrough = 0,
    Override = 1,
    Min = 2,
    Max = 3,
    Sum = 4,
};

struct VrsRateAxes {
    uint8_t x = 1;  // 1, 2, or 4 pixels
    uint8_t y = 1;  // 1, 2, or 4 pixels
};

// Clean-room decoded RDNA2 VRS Tier 2 control register state:
//   bits [3:0]   : base shading rate (4-bit (log2(x) << 2) | log2(y))
//   bits [6:4]   : combiner 0 (per-draw vs per-primitive)
//   bits [9:7]   : combiner 1 (stage 0 vs screen-space shading rate image)
//   bit  [10]    : enable screen-space VRS shading rate image
//   bit  [11]    : enable per-primitive VRS export
//   bits [15:12] : tile_size_log2 (3 -> 8x8, 4 -> 16x16, 0 defaults to 8x8)
struct Rdna2VrsControl {
    VrsShadingRate base_rate = VrsShadingRate::Rate1x1;
    VrsCombiner combiner0 = VrsCombiner::Passthrough;
    VrsCombiner combiner1 = VrsCombiner::Passthrough;
    bool enable_vrs_image = false;
    bool enable_per_primitive = false;
    uint32_t tile_size = 8;
};

struct VrsShadingRateImage {
    uint32_t render_width = 0;
    uint32_t render_height = 0;
    uint32_t tile_size = 8;
    uint32_t tiles_x = 0;
    uint32_t tiles_y = 0;
    std::vector<uint8_t> data;  // DXGI_FORMAT_R8_UINT row-major (tiles_x * tiles_y bytes)
};

// Tile-mask rate encode/decode (DXGI_FORMAT_R8_UINT compatible).
uint8_t encode_vrs_tile_rate(VrsShadingRate rate);
VrsShadingRate decode_vrs_tile_rate(uint8_t encoded);

// Axis helpers (pixel dimensions {1, 2, 4}).
VrsRateAxes vrs_rate_axes(VrsShadingRate rate);
VrsShadingRate vrs_rate_from_axes(uint8_t axis_x, uint8_t axis_y);

// Clean-room RDNA2 VRS control register encode/decode.
uint32_t encode_rdna2_vrs_control(const Rdna2VrsControl& ctrl);
Rdna2VrsControl decode_rdna2_vrs_control(uint32_t reg_word);

// VRS combiner evaluation (D3D12 / RDNA2 Tier 2 semantics).
VrsShadingRate combine_shading_rates(VrsShadingRate a, VrsShadingRate b, VrsCombiner combiner);

// D3D12 enum translation helpers.
D3D12_SHADING_RATE to_d3d12_shading_rate(VrsShadingRate rate);
VrsShadingRate from_d3d12_shading_rate(D3D12_SHADING_RATE rate);
D3D12_SHADING_RATE_COMBINER to_d3d12_combiner(VrsCombiner combiner);
VrsCombiner from_d3d12_combiner(D3D12_SHADING_RATE_COMBINER combiner);

// Build a DXGI_FORMAT_R8_UINT screen-space shading rate image for (width, height, tile_size).
VrsShadingRateImage build_vrs_shading_rate_image(
    uint32_t width, uint32_t height, uint32_t tile_size,
    const std::function<VrsShadingRate(uint32_t tile_x, uint32_t tile_y, uint32_t tiles_x, uint32_t tiles_y)>& rate_fn);

VrsShadingRateImage build_vrs_shading_rate_image(
    uint32_t width, uint32_t height, uint32_t tile_size, VrsShadingRate fill_rate);

}  // namespace kestrel::gpu
