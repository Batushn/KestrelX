#include "kestrel/gpu/svt.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace kestrel::gpu {

SvtTileShape standard_tile_shape_for_format(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_A8_UNORM:
            return {256, 256, 1, 1, 1};
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16_UINT:
            return {256, 128, 1, 2, 1};
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R16G16_FLOAT:
            return {128, 128, 1, 4, 1};
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R32G32_FLOAT:
            return {128, 64, 1, 8, 1};
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
            return {64, 64, 1, 16, 1};
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
            return {512, 256, 1, 8, 4};
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return {256, 256, 1, 16, 4};
        default:
            return {128, 128, 1, 4, 1};
    }
}

SvtVirtualTextureLayout compute_virtual_texture_layout(const SvtVirtualTextureDesc& desc) {
    SvtVirtualTextureLayout layout{};
    layout.desc = desc;
    layout.tile_shape = standard_tile_shape_for_format(desc.format);

    const uint32_t w0 = std::max(1u, desc.width);
    const uint32_t h0 = std::max(1u, desc.height);
    const uint32_t max_mips = std::max(1u, desc.mip_levels);

    uint32_t next_tile = 0;
    uint32_t packed_bytes = 0;
    bool in_packed_tail = false;

    for (uint32_t mip = 0; mip < max_mips; ++mip) {
        const uint32_t mw = std::max(1u, w0 >> mip);
        const uint32_t mh = std::max(1u, h0 >> mip);

        SvtMipTiling mt{};
        mt.mip = mip;
        mt.width_texels = mw;
        mt.height_texels = mh;

        if (!in_packed_tail &&
            mw >= layout.tile_shape.texels_w &&
            mh >= layout.tile_shape.texels_h) {
            mt.tiles_x = (mw + layout.tile_shape.texels_w - 1u) / layout.tile_shape.texels_w;
            mt.tiles_y = (mh + layout.tile_shape.texels_h - 1u) / layout.tile_shape.texels_h;
            mt.start_tile_index = next_tile;
            mt.is_packed_mip_tail = false;
            next_tile += mt.tiles_x * mt.tiles_y;
            ++layout.num_standard_mips;
        } else {
            in_packed_tail = true;
            mt.tiles_x = 1;
            mt.tiles_y = 1;
            mt.start_tile_index = next_tile;
            mt.is_packed_mip_tail = true;
            ++layout.num_packed_mips;

            const uint32_t bdim = std::max(1u, layout.tile_shape.block_dim);
            const uint32_t bw = (mw + bdim - 1u) / bdim;
            const uint32_t bh = (mh + bdim - 1u) / bdim;
            packed_bytes += bw * bh * layout.tile_shape.bytes_per_element;
        }
        layout.mips.push_back(mt);
    }

    if (layout.num_packed_mips > 0) {
        layout.num_tiles_for_packed_mips = std::max(1u, (packed_bytes + kSvtTileSizeBytes - 1u) / kSvtTileSizeBytes);
        next_tile += layout.num_tiles_for_packed_mips;
    }
    layout.total_tiles = next_tile;
    return layout;
}

VirtualTexturePageTable::VirtualTexturePageTable(SvtVirtualTextureDesc desc,
                                                 uint32_t physical_pool_tiles,
                                                 uint32_t heap_id)
    : layout_(compute_virtual_texture_layout(desc)),
      physical_pool_tiles_(physical_pool_tiles),
      heap_id_(heap_id) {
    free_physical_tiles_.reserve(physical_pool_tiles_);
    for (uint32_t i = physical_pool_tiles_; i > 0; --i) {
        free_physical_tiles_.push_back(i - 1u);
    }
}

bool VirtualTexturePageTable::is_valid_coord(VirtualTileCoord vtile) const {
    if (vtile.mip >= layout_.mips.size()) return false;
    const auto& m = layout_.mips[vtile.mip];
    return vtile.x < m.tiles_x && vtile.y < m.tiles_y;
}

void VirtualTexturePageTable::begin_frame(uint64_t frame_index) {
    current_frame_ = frame_index;
}

void VirtualTexturePageTable::touch_tile(VirtualTileCoord vtile) {
    auto it = page_table_.find(vtile);
    if (it != page_table_.end() && it->second.state != TileResidency::Unmapped) {
        it->second.last_used_frame = current_frame_;
        ++it->second.access_count;
    }
}

bool VirtualTexturePageTable::is_resident(VirtualTileCoord vtile) const {
    auto it = page_table_.find(vtile);
    return it != page_table_.end() && it->second.state != TileResidency::Unmapped;
}

std::optional<PhysicalTileAddr> VirtualTexturePageTable::lookup(VirtualTileCoord vtile) const {
    auto it = page_table_.find(vtile);
    if (it == page_table_.end() || it->second.state == TileResidency::Unmapped) {
        return std::nullopt;
    }
    return it->second.ptile;
}

Result<std::vector<SvtTileMappingOp>> VirtualTexturePageTable::stream_tiles(
    std::span<const VirtualTileCoord> requested, bool pin) {
    std::vector<SvtTileMappingOp> ops;
    if (physical_pool_tiles_ == 0 && !requested.empty()) {
        return fail("SVT physical tile pool has 0 tiles");
    }

    for (const auto& vtile : requested) {
        if (!is_valid_coord(vtile)) {
            return fail(std::format("SVT virtual tile out of bounds: mip={} ({},{})",
                                    vtile.mip, vtile.x, vtile.y));
        }

        auto existing = page_table_.find(vtile);
        if (existing != page_table_.end() && existing->second.state != TileResidency::Unmapped) {
            existing->second.last_used_frame = current_frame_;
            ++existing->second.access_count;
            if (pin) existing->second.state = TileResidency::Pinned;
            ++cache_hits_;
            continue;
        }

        ++page_faults_;
        PhysicalTileAddr ptile{heap_id_, 0};

        if (!free_physical_tiles_.empty()) {
            ptile.tile_index = free_physical_tiles_.back();
            free_physical_tiles_.pop_back();
        } else {
            // Find coldest unpinned LRU resident tile to evict.
            auto victim = page_table_.end();
            for (auto it = page_table_.begin(); it != page_table_.end(); ++it) {
                if (it->second.state != TileResidency::Resident) continue;
                if (victim == page_table_.end() ||
                    it->second.last_used_frame < victim->second.last_used_frame ||
                    (it->second.last_used_frame == victim->second.last_used_frame &&
                     it->second.access_count < victim->second.access_count)) {
                    victim = it;
                }
            }
            if (victim == page_table_.end()) {
                return fail("SVT physical tile pool exhausted (all tiles pinned)");
            }

            ptile = victim->second.ptile;
            SvtTileMappingOp evict_op{};
            evict_op.kind = SvtTileMappingOp::Kind::Evict;
            evict_op.vtile = victim->first;
            evict_op.ptile = {};
            evict_op.evicted_from = victim->first;
            ops.push_back(evict_op);

            page_table_.erase(victim);
            ++evict_count_;
        }

        PageTableEntry entry{};
        entry.vtile = vtile;
        entry.ptile = ptile;
        entry.state = pin ? TileResidency::Pinned : TileResidency::Resident;
        entry.last_used_frame = current_frame_;
        entry.access_count = 1;
        page_table_[vtile] = entry;
        ++map_count_;

        SvtTileMappingOp map_op{};
        map_op.kind = SvtTileMappingOp::Kind::Map;
        map_op.vtile = vtile;
        map_op.ptile = ptile;
        ops.push_back(map_op);
    }

    return ops;
}

Result<void> VirtualTexturePageTable::unmap_tile(VirtualTileCoord vtile) {
    auto it = page_table_.find(vtile);
    if (it == page_table_.end()) {
        return fail("SVT unmap_tile: tile is not mapped");
    }
    free_physical_tiles_.push_back(it->second.ptile.tile_index);
    page_table_.erase(it);
    ++evict_count_;
    return {};
}

std::vector<PageTableEntry> VirtualTexturePageTable::resident_entries() const {
    std::vector<PageTableEntry> out;
    out.reserve(page_table_.size());
    for (const auto& [_, entry] : page_table_) {
        out.push_back(entry);
    }
    std::sort(out.begin(), out.end(), [](const PageTableEntry& a, const PageTableEntry& b) {
        if (a.vtile.mip != b.vtile.mip) return a.vtile.mip < b.vtile.mip;
        if (a.vtile.y != b.vtile.y) return a.vtile.y < b.vtile.y;
        return a.vtile.x < b.vtile.x;
    });
    return out;
}

SvtStats VirtualTexturePageTable::stats() const {
    return SvtStats{
        layout_.total_tiles,
        physical_pool_tiles_,
        static_cast<uint32_t>(page_table_.size()),
        static_cast<uint32_t>(free_physical_tiles_.size()),
        map_count_,
        evict_count_,
        page_faults_,
        cache_hits_,
    };
}

SvtFeedbackReport SvtFeedbackAnalyzer::analyze_rgba8_feedback(
    const uint8_t* rgba8_pixels,
    uint32_t width,
    uint32_t height,
    uint32_t row_pitch_bytes,
    const SvtVirtualTextureLayout& layout,
    uint32_t target_mip) {
    SvtFeedbackReport report{};
    if (!rgba8_pixels || width == 0 || height == 0 || target_mip >= layout.mips.size()) {
        return report;
    }

    const auto& mip_info = layout.mips[target_mip];
    if (mip_info.tiles_x == 0 || mip_info.tiles_y == 0) return report;

    std::unordered_map<VirtualTileCoord, uint64_t, VirtualTileHash> fault_counts;
    std::unordered_map<VirtualTileCoord, uint64_t, VirtualTileHash> touch_counts;

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = rgba8_pixels + static_cast<size_t>(y) * row_pitch_bytes;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t r = row[x * 4 + 0];
            const uint8_t g = row[x * 4 + 1];
            const uint8_t b = row[x * 4 + 2];
            const uint8_t a = row[x * 4 + 3];
            if (a == 0 && r == 0 && g == 0 && b == 0) continue;

            ++report.total_samples;
            const float u = std::clamp(static_cast<float>(g) / 255.0f, 0.0f, 0.9999f);
            const float v = std::clamp(static_cast<float>(b) / 255.0f, 0.0f, 0.9999f);
            const uint32_t tx = std::min(mip_info.tiles_x - 1u, static_cast<uint32_t>(u * mip_info.tiles_x));
            const uint32_t ty = std::min(mip_info.tiles_y - 1u, static_cast<uint32_t>(v * mip_info.tiles_y));
            const VirtualTileCoord vt{target_mip, tx, ty};

            ++touch_counts[vt];
            if (r >= 128u) {
                ++report.fault_samples;
                ++fault_counts[vt];
            } else {
                ++report.resident_samples;
            }
        }
    }

    std::vector<std::pair<VirtualTileCoord, uint64_t>> sorted_faults(fault_counts.begin(), fault_counts.end());
    std::sort(sorted_faults.begin(), sorted_faults.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.second != rhs.second) return lhs.second > rhs.second;
        if (lhs.first.mip != rhs.first.mip) return lhs.first.mip < rhs.first.mip;
        if (lhs.first.y != rhs.first.y) return lhs.first.y < rhs.first.y;
        return lhs.first.x < rhs.first.x;
    });
    for (const auto& [vt, _] : sorted_faults) {
        report.faulted_tiles.push_back(vt);
    }

    for (const auto& [vt, _] : touch_counts) {
        report.touched_tiles.push_back(vt);
    }
    std::sort(report.touched_tiles.begin(), report.touched_tiles.end(), [](const auto& a, const auto& b) {
        if (a.mip != b.mip) return a.mip < b.mip;
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });
    return report;
}

std::vector<VirtualTileCoord> SvtFeedbackAnalyzer::analyze_min_mip_feedback(
    const uint8_t* min_mip_map,
    uint32_t map_w,
    uint32_t map_h,
    const SvtVirtualTextureLayout& layout) {
    std::vector<VirtualTileCoord> out;
    if (!min_mip_map || map_w == 0 || map_h == 0 || layout.mips.empty()) return out;

    std::unordered_map<VirtualTileCoord, bool, VirtualTileHash> seen;
    for (uint32_t my = 0; my < map_h; ++my) {
        for (uint32_t mx = 0; mx < map_w; ++mx) {
            const uint8_t req_mip = min_mip_map[static_cast<size_t>(my) * map_w + mx];
            if (req_mip == 0xFFu || req_mip >= layout.mips.size()) continue;
            const auto& m = layout.mips[req_mip];
            if (m.is_packed_mip_tail) continue;
            const uint32_t tx = std::min(m.tiles_x - 1u, (mx * m.tiles_x) / map_w);
            const uint32_t ty = std::min(m.tiles_y - 1u, (my * m.tiles_y) / map_h);
            const VirtualTileCoord vt{req_mip, tx, ty};
            if (seen.emplace(vt, true).second) {
                out.push_back(vt);
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.mip != b.mip) return a.mip > b.mip;  // coarser mips first
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });
    return out;
}

Result<void> apply_svt_mappings_d3d12(
    ID3D12CommandQueue* queue,
    ID3D12Resource* reserved_resource,
    ID3D12Heap* tile_heap,
    std::span<const SvtTileMappingOp> ops) {
    if (!queue || !reserved_resource) {
        return fail("apply_svt_mappings_d3d12: null queue or reserved_resource");
    }
    if (ops.empty()) return {};

    for (const auto& op : ops) {
        D3D12_TILED_RESOURCE_COORDINATE coord{};
        coord.X = op.vtile.x;
        coord.Y = op.vtile.y;
        coord.Z = 0;
        coord.Subresource = op.vtile.mip;

        D3D12_TILE_REGION_SIZE region{};
        region.NumTiles = 1;
        region.UseBox = FALSE;
        region.Width = 1;
        region.Height = 1;
        region.Depth = 1;

        if (op.kind == SvtTileMappingOp::Kind::Evict) {
            const D3D12_TILE_RANGE_FLAGS flag = D3D12_TILE_RANGE_FLAG_NULL;
            const UINT count = 1;
            queue->UpdateTileMappings(
                reserved_resource, 1, &coord, &region,
                nullptr, 1, &flag, nullptr, &count,
                D3D12_TILE_MAPPING_FLAG_NONE);
        } else {
            if (!tile_heap) {
                return fail("apply_svt_mappings_d3d12: Map operation requires non-null tile_heap");
            }
            const D3D12_TILE_RANGE_FLAGS flag = D3D12_TILE_RANGE_FLAG_NONE;
            const UINT heap_offset = op.ptile.tile_index;
            const UINT count = 1;
            queue->UpdateTileMappings(
                reserved_resource, 1, &coord, &region,
                tile_heap, 1, &flag, &heap_offset, &count,
                D3D12_TILE_MAPPING_FLAG_NONE);
        }
    }
    return {};
}

}  // namespace kestrel::gpu
