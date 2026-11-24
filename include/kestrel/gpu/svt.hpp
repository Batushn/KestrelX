#pragma once

#include "kestrel/common/types.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace kestrel::gpu {

// Standard D3D12 / RDNA2 PRT physical tile size = 64 KiB
constexpr uint32_t kSvtTileSizeBytes = 65536u;

struct SvtTileShape {
    uint32_t texels_w = 128;
    uint32_t texels_h = 128;
    uint32_t texels_d = 1;
    uint32_t bytes_per_element = 4;  // bytes per texel or compressed block
    uint32_t block_dim = 1;          // 1 for uncompressed, 4 for BCn
};

struct SvtVirtualTextureDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_levels = 1;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
};

struct SvtMipTiling {
    uint32_t mip = 0;
    uint32_t width_texels = 0;
    uint32_t height_texels = 0;
    uint32_t tiles_x = 0;
    uint32_t tiles_y = 0;
    uint32_t start_tile_index = 0;
    bool is_packed_mip_tail = false;
};

struct SvtVirtualTextureLayout {
    SvtVirtualTextureDesc desc;
    SvtTileShape tile_shape;
    uint32_t num_standard_mips = 0;
    uint32_t num_packed_mips = 0;
    uint32_t num_tiles_for_packed_mips = 0;
    uint32_t total_tiles = 0;
    std::vector<SvtMipTiling> mips;
};

struct VirtualTileCoord {
    uint32_t mip = 0;
    uint32_t x = 0;
    uint32_t y = 0;

    bool operator==(const VirtualTileCoord& o) const = default;
};

struct VirtualTileHash {
    size_t operator()(const VirtualTileCoord& c) const noexcept {
        uint64_t h = (static_cast<uint64_t>(c.mip) << 40) ^
                     (static_cast<uint64_t>(c.y) << 20) ^
                     static_cast<uint64_t>(c.x);
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccduLL;
        h ^= h >> 33;
        return static_cast<size_t>(h);
    }
};

struct PhysicalTileAddr {
    uint32_t heap_id = 0;
    uint32_t tile_index = 0;
};

enum class TileResidency : uint8_t {
    Unmapped = 0,
    Resident = 1,
    Pinned = 2,
};

struct PageTableEntry {
    VirtualTileCoord vtile{};
    PhysicalTileAddr ptile{};
    TileResidency state = TileResidency::Unmapped;
    uint64_t last_used_frame = 0;
    uint32_t access_count = 0;
};

struct SvtTileMappingOp {
    enum class Kind : uint8_t { Map, Evict };
    Kind kind = Kind::Map;
    VirtualTileCoord vtile{};
    PhysicalTileAddr ptile{};
    VirtualTileCoord evicted_from{};
};

struct SvtStats {
    uint32_t virtual_tiles_total = 0;
    uint32_t physical_pool_tiles = 0;
    uint32_t resident_tiles = 0;
    uint32_t free_physical_tiles = 0;
    uint64_t map_count = 0;
    uint64_t evict_count = 0;
    uint64_t page_faults = 0;
    uint64_t cache_hits = 0;
};

struct SvtFeedbackReport {
    uint64_t total_samples = 0;
    uint64_t resident_samples = 0;
    uint64_t fault_samples = 0;
    std::vector<VirtualTileCoord> faulted_tiles;
    std::vector<VirtualTileCoord> touched_tiles;
};

// Compute standard 64KB D3D12 tile dimensions for common uncompressed & BCn formats.
SvtTileShape standard_tile_shape_for_format(DXGI_FORMAT fmt);

// Compute full virtual tile layout (standard mips + packed mip tail).
SvtVirtualTextureLayout compute_virtual_texture_layout(const SvtVirtualTextureDesc& desc);

// Manages 64KB virtual-to-physical tile mappings with LRU eviction when the
// physical pool is smaller than the virtual texture footprint.
class VirtualTexturePageTable {
public:
    VirtualTexturePageTable(SvtVirtualTextureDesc desc,
                            uint32_t physical_pool_tiles,
                            uint32_t heap_id = 0);

    void begin_frame(uint64_t frame_index);
    void touch_tile(VirtualTileCoord vtile);
    bool is_resident(VirtualTileCoord vtile) const;
    std::optional<PhysicalTileAddr> lookup(VirtualTileCoord vtile) const;

    // Ensure requested virtual tiles are mapped to physical 64KB tiles,
    // evicting coldest unpinned LRU tiles if the physical pool is full.
    Result<std::vector<SvtTileMappingOp>> stream_tiles(
        std::span<const VirtualTileCoord> requested, bool pin = false);

    // Explicitly unmap a virtual tile back to NULL and return its physical tile to the free list.
    Result<void> unmap_tile(VirtualTileCoord vtile);

    std::vector<PageTableEntry> resident_entries() const;
    const SvtVirtualTextureLayout& layout() const { return layout_; }
    SvtStats stats() const;

private:
    bool is_valid_coord(VirtualTileCoord vtile) const;

    SvtVirtualTextureLayout layout_;
    uint32_t physical_pool_tiles_ = 0;
    uint32_t heap_id_ = 0;
    uint64_t current_frame_ = 0;

    std::vector<uint32_t> free_physical_tiles_;
    std::unordered_map<VirtualTileCoord, PageTableEntry, VirtualTileHash> page_table_;

    uint64_t map_count_ = 0;
    uint64_t evict_count_ = 0;
    uint64_t page_faults_ = 0;
    uint64_t cache_hits_ = 0;
};

// Analyzes GPU-written residency/UV feedback render targets and min-mip feedback maps.
class SvtFeedbackAnalyzer {
public:
    // RGBA8 feedback pixels:
    //   R >= 128 : TFE=1 fault at normalized UV (G/255, B/255)
    //   R < 128 && A > 0 : resident hit at normalized UV (G/255, B/255)
    static SvtFeedbackReport analyze_rgba8_feedback(
        const uint8_t* rgba8_pixels,
        uint32_t width,
        uint32_t height,
        uint32_t row_pitch_bytes,
        const SvtVirtualTextureLayout& layout,
        uint32_t target_mip = 0);

    // Min-mip feedback buffer (0xFF = unrequested, 0..mip_levels-1 = finest requested mip).
    static std::vector<VirtualTileCoord> analyze_min_mip_feedback(
        const uint8_t* min_mip_map,
        uint32_t map_w,
        uint32_t map_h,
        const SvtVirtualTextureLayout& layout);
};

// Execute a batch of Map/Evict 64KB tile operations on a D3D12 Reserved Resource.
Result<void> apply_svt_mappings_d3d12(
    ID3D12CommandQueue* queue,
    ID3D12Resource* reserved_resource,
    ID3D12Heap* tile_heap,
    std::span<const SvtTileMappingOp> ops);

}  // namespace kestrel::gpu
