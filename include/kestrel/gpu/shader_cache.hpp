#pragma once

#include "kestrel/gpu/rdna2_recomp.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace kestrel::gpu {

// Bump whenever IR lifting or HLSL emission semantics change to auto-invalidate stale disk entries.
constexpr uint32_t kShaderCacheCompilerVersion = 0x00070001u;

struct ShaderCacheStats {
    uint64_t disk_hits = 0;
    uint64_t disk_misses = 0;
    uint64_t disk_writes = 0;
    uint64_t corrupt_evictions = 0;
};

// Persistent on-disk DXIL cache with atomic writes and FNV-1a payload integrity checks.
// Default root: KESTREL_SHADER_CACHE_DIR -> %LOCALAPPDATA%/KestrelX/cache -> temp/KestrelX/cache.
class DiskShaderCache {
public:
    explicit DiskShaderCache(std::filesystem::path root_dir = {});

    static std::filesystem::path default_cache_dir();
    static uint64_t make_key(ShaderStage stage, WaveMode wave, uint32_t group_x,
                             const uint8_t* rdna2_bytes, size_t len);

    void set_root_dir(std::filesystem::path root_dir);
    const std::filesystem::path& root_dir() const { return root_dir_; }

    bool load(uint64_t key, std::vector<uint8_t>& out_dxil);
    bool store(uint64_t key, const std::vector<uint8_t>& dxil);

    ShaderCacheStats stats() const { return stats_; }
    void reset_stats() { stats_ = {}; }

private:
    std::filesystem::path entry_path(uint64_t key) const;

    std::filesystem::path root_dir_;
    ShaderCacheStats stats_{};
};

}  // namespace kestrel::gpu
