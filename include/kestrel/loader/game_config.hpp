#pragma once

#include "kestrel/common/types.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kestrel::loader {

struct ExecutableDecl {
    std::string name;
    std::string id;
    std::string target_device_family;
};

struct GameConfig {
    std::string identity_name;
    std::string publisher;
    std::string version;
    std::string title_id_hex;
    uint32_t title_id = 0;
    uint64_t anaconda_title_memory_mb = 0;
    uint64_t lockhart_title_memory_mb = 0;
    std::vector<ExecutableDecl> executables;
};

Result<GameConfig> parse_game_config_xml(std::string_view xml);
Result<GameConfig> load_game_config(const std::filesystem::path& path);

}  // namespace kestrel::loader
