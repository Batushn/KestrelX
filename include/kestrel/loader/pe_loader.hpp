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

class PeLoader {
public:
    PeLoader(hle::Registry& hle, std::filesystem::path search_dir);
    ~PeLoader();
    PeLoader(const PeLoader&) = delete;
    PeLoader& operator=(const PeLoader&) = delete;

    Result<MappedModule*> load_image(const std::filesystem::path& path);
    Result<int> execute_entry(MappedModule& mod);

    const std::vector<std::unique_ptr<MappedModule>>& modules() const { return modules_; }

private:
    Result<MappedModule*> find_or_load_guest_dll(std::string_view dll_name);
    Result<void> apply_relocations(MappedModule& mod);
    Result<void> resolve_imports(MappedModule& mod);
    Result<void> protect_sections(MappedModule& mod);
    void register_exception_table(MappedModule& mod);
    void run_tls_callbacks(MappedModule& mod);

    hle::Registry& hle_;
    std::filesystem::path search_dir_;
    std::vector<std::unique_ptr<MappedModule>> modules_;
    std::unordered_map<std::string, MappedModule*> by_name_;
};

}  // namespace kestrel::loader
