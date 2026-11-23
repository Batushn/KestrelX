#pragma once

#include "kestrel/common/types.hpp"

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace kestrel::vfs {

enum class MountKind {
    GameReadOnly,    // GAME:\, G:\, D:\ -> read base, write to overlay
    PersistentSave,  // S:\ -> persistent local storage
    TempScratch,     // T:\ -> title scratch volume
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

    const std::filesystem::path& game_root() const { return game_root_; }
    const std::filesystem::path& overlay_root() const { return overlay_root_; }
    const std::filesystem::path& save_root() const { return save_root_; }
    const std::filesystem::path& temp_root() const { return temp_root_; }

private:
    struct ParsedPath {
        MountKind mount;
        std::filesystem::path rel;
        std::string canonical_key;
    };

    Result<ParsedPath> parse_console_path(std::string_view console_path) const;

    std::filesystem::path game_root_;
    std::filesystem::path overlay_root_;
    std::filesystem::path save_root_;
    std::filesystem::path temp_root_;
    mutable std::mutex mutex_;
    std::unordered_set<std::string> whiteouts_;
};

}  // namespace kestrel::vfs
