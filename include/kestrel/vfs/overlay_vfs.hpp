#pragma once

#include "kestrel/common/types.hpp"

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kestrel::vfs {

enum class MountKind {
    GameReadOnly,    // GAME:\, G:\, D:\, C:\ -> read base, write to overlay
    PersistentSave,  // S:\ -> persistent local storage
    TempScratch,     // T:\ -> title scratch volume
};

struct DirEntry {
    std::string name;
    bool is_directory = false;
};

struct CacheStats {
    size_t cache_hits = 0;
    size_t cache_misses = 0;
    size_t dir_scans = 0;
};

class OverlayVfs {
public:
    OverlayVfs(std::filesystem::path game_root, std::filesystem::path data_root);

    // Resolve a console path (e.g. "GAME:\data\file.bin", "S:\save.dat") to a host path.
    // For read operations, checks overlay first then read-only base.
    // For write operations, always returns an overlay / data path so game_root is never modified.
    Result<std::filesystem::path> resolve(std::string_view console_path, bool for_write) const;

    // Delete a file logically: deletes overlay copy and records a whiteout if present in game_root.
    Result<void> remove_file(std::string_view console_path);

    // Check logical existence (honoring whiteouts).
    bool exists(std::string_view console_path) const;

    // Enumerate merged child entries of a virtual directory across upper and lower layers,
    // suppressing whited-out entries and deduplicating case-insensitively (upper layer wins).
    Result<std::vector<DirEntry>> list_directory(std::string_view virtual_dir) const;

    // Invalidate all per-layer path/directory caches (e.g., if external host changes occur).
    void invalidate_caches() const;

    CacheStats cache_stats() const;

    const std::filesystem::path& game_root() const { return game_root_; }
    const std::filesystem::path& overlay_root() const { return overlay_root_; }
    const std::filesystem::path& save_root() const { return save_root_; }
    const std::filesystem::path& temp_root() const { return temp_root_; }

private:
    struct ParsedPath {
        MountKind mount;
        std::vector<std::string> parts;
        std::filesystem::path rel;
        std::string lower_rel;
        std::string canonical_key;
    };

    struct CachedChild {
        std::string actual_name;
        bool is_directory = false;
    };

    struct DirIndex {
        std::unordered_map<std::string, CachedChild> children;
    };

    struct CachedLookup {
        bool exists = false;
        bool is_directory = false;
        std::filesystem::path host_path;
    };

    struct LayerCache {
        std::filesystem::path root;
        std::unordered_map<std::string, DirIndex> dirs;
        std::unordered_map<std::string, CachedLookup> path_cache;

        CachedLookup lookup(const std::vector<std::string>& parts,
                            std::string_view lower_rel,
                            CacheStats& stats);
        const DirIndex* get_dir(const std::vector<std::string>& parts,
                                std::string_view lower_rel,
                                CacheStats& stats);
        void invalidate(std::string_view lower_rel);
        void clear();
    };

    Result<ParsedPath> parse_console_path(std::string_view console_path) const;

    std::filesystem::path game_root_;
    std::filesystem::path overlay_root_;
    std::filesystem::path save_root_;
    std::filesystem::path temp_root_;
    mutable std::mutex mutex_;
    std::unordered_set<std::string> whiteouts_;
    mutable LayerCache game_cache_;
    mutable LayerCache overlay_cache_;
    mutable LayerCache save_cache_;
    mutable LayerCache temp_cache_;
    mutable CacheStats stats_;
};

}  // namespace kestrel::vfs