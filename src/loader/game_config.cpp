#include "kestrel/loader/game_config.hpp"

#include <regex>

namespace kestrel::loader {

namespace {

std::string extract_attr(const std::string& tag, const std::string& attr) {
    const std::regex re(attr + R"re(\s*=\s*"([^"]*)")re", std::regex::icase);
    std::smatch m;
    if (std::regex_search(tag, m, re)) {
        return m[1].str();
    }
    return {};
}

std::string extract_elem_text(const std::string& xml, const std::string& name) {
    const std::regex re("<" + name + R"re([^>]*>([^<]*)</)re" + name + ">", std::regex::icase);
    std::smatch m;
    if (std::regex_search(xml, m, re)) {
        return m[1].str();
    }
    return {};
}

}  // namespace

Result<GameConfig> parse_game_config_xml(std::string_view xml_view) {
    const std::string xml(xml_view);
    if (xml.find("<Game") == std::string::npos && xml.find("<game") == std::string::npos) {
        return fail("MicrosoftGame.config is missing <Game> root element");
    }

    GameConfig cfg;

    // <Identity Name="..." Publisher="..." Version="..." />
    {
        const std::regex id_re(R"re(<Identity\s[^>]*>)re", std::regex::icase);
        std::smatch m;
        if (std::regex_search(xml, m, id_re)) {
            cfg.identity_name = extract_attr(m[0].str(), "Name");
            cfg.publisher = extract_attr(m[0].str(), "Publisher");
            cfg.version = extract_attr(m[0].str(), "Version");
        }
    }

    // <TitleId>...</TitleId>
    cfg.title_id_hex = extract_elem_text(xml, "TitleId");
    if (!cfg.title_id_hex.empty()) {
        try {
            cfg.title_id = static_cast<uint32_t>(std::stoul(cfg.title_id_hex, nullptr, 16));
        } catch (...) {
            return fail("Invalid hex TitleId in MicrosoftGame.config: " + cfg.title_id_hex);
        }
    }

    auto parse_mb = [](const std::string& s) -> uint64_t {
        if (s.empty()) return 0;
        try { return std::stoull(s); } catch (...) { return 0; }
    };
    cfg.anaconda_title_memory_mb = parse_mb(extract_elem_text(xml, "XboxAnacondaTitleMemory"));
    cfg.lockhart_title_memory_mb = parse_mb(extract_elem_text(xml, "XboxLockhartTitleMemory"));

    // <Executable Name="..." Id="..." TargetDeviceFamily="..." />
    {
        const std::regex exe_re(R"re(<Executable\s[^>]*>)re", std::regex::icase);
        for (auto it = std::sregex_iterator(xml.begin(), xml.end(), exe_re);
             it != std::sregex_iterator(); ++it) {
            const std::string tag = (*it)[0].str();
            ExecutableDecl decl;
            decl.name = extract_attr(tag, "Name");
            decl.id = extract_attr(tag, "Id");
            decl.target_device_family = extract_attr(tag, "TargetDeviceFamily");
            if (decl.target_device_family.empty()) {
                decl.target_device_family = extract_attr(tag, "TargetDeviceFamilyForDLC");
            }
            if (!decl.name.empty()) {
                cfg.executables.push_back(std::move(decl));
            }
        }
    }

    if (cfg.executables.empty()) {
        return fail("MicrosoftGame.config declares no <Executable> entries");
    }
    return cfg;
}

Result<GameConfig> load_game_config(const std::filesystem::path& path) {
    auto bytes = read_file_bytes(path);
    if (!bytes) return std::unexpected(bytes.error());
    if (bytes->size() > (16u << 20)) {
        return fail("MicrosoftGame.config exceeds 16 MiB safety limit");
    }
    std::string_view sv(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    if (sv.size() >= 3 && static_cast<unsigned char>(sv[0]) == 0xEF &&
        static_cast<unsigned char>(sv[1]) == 0xBB && static_cast<unsigned char>(sv[2]) == 0xBF) {
        sv.remove_prefix(3);
    }
    return parse_game_config_xml(sv);
}

}  // namespace kestrel::loader
