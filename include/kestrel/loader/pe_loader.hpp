#pragma once

#include "kestrel/common/types.hpp"
#include "kestrel/hle/registry.hpp"
#include "kestrel/loader/pe_image.hpp"

#include <Windows.h>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace kestrel::loader {

struct MappedModule {
    std::string name;
    uint8_t* base = nullptr;
    uint32_t size_of_image = 0;
    uint32_t entry_rva = 0;
    bool is_dll = false;
    PRUNTIME_FUNCTION pdata = nullptr;
    PeImage image;
    uint32_t relocs_applied = 0;
    uint32_t hle_imports = 0;
    uint32_t guest_imports = 0;
    uint32_t host_imports = 0;
    uint32_t stub_imports = 0;
    uint32_t tls_callbacks_run = 0;

    void* find_export_by_name(std::string_view sym) const;
    void* find_export_by_ordinal(uint16_t ord) const;
};

struct AgilitySdkConfig {
    uint32_t sdk_version = 0;
    std::string sdk_path;
    bool found = false;
};

class PeLoader {
public:
    PeLoader(hle::Registry& hle, std::filesystem::path search_dir);
    ~PeLoader();
    PeLoader(const PeLoader&) = delete;
    PeLoader& operator=(const PeLoader&) = delete;

    Result<MappedModule*> load_image(const std::filesystem::path& path);
    Result<MappedModule*> find_or_load_guest_dll(std::string_view dll_name);
    Result<int> execute_entry(MappedModule& mod);
    Result<void*> resolve_export_by_name(MappedModule& mod, std::string_view sym);
    Result<void*> resolve_export_by_ordinal(MappedModule& mod, uint16_t ord);

    // Dynamic GDK & Agility SDK DLL redirection
    void add_dll_redirection(std::string_view from_dll, std::string_view to_dll);
    std::string get_redirected_dll(std::string_view dll_name) const;
    void add_search_directory(const std::filesystem::path& dir);
    AgilitySdkConfig query_agility_sdk(const MappedModule& mod) const;
    void apply_agility_sdk_redirection(const MappedModule& mod);

    const std::vector<std::unique_ptr<MappedModule>>& modules() const { return modules_; }

private:
    static constexpr int kMaxForwarderDepth = 8;

    Result<void*> resolve_export_symbol(MappedModule& mod, const PeExportSymbol& exp, int depth);
    Result<void*> resolve_guest_export_by_name(MappedModule& mod, std::string_view sym, int depth);
    Result<void*> resolve_guest_export_by_ordinal(MappedModule& mod, uint16_t ord, int depth);
    Result<void*> resolve_forwarder(std::string_view forwarder, int depth);
    Result<void> apply_relocations(MappedModule& mod);
    Result<void> resolve_imports(MappedModule& mod);
    Result<void> protect_sections(MappedModule& mod);
    void register_exception_table(MappedModule& mod);
    void run_tls_callbacks(MappedModule& mod);
    void rollback_module(const std::string& key, MappedModule* target);

    hle::Registry& hle_;
    std::filesystem::path search_dir_;
    std::vector<std::filesystem::path> extra_search_dirs_;
    std::unordered_map<std::string, std::string> dll_redirections_;
    std::vector<std::unique_ptr<MappedModule>> modules_;
    std::unordered_map<std::string, MappedModule*> by_name_;
};

}  // namespace kestrel::loader
