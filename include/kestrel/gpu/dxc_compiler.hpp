#pragma once

#include "kestrel/common/types.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace kestrel::gpu {

struct DxcCompileOutput {
    std::vector<uint8_t> dxil;
    std::string warnings;
    std::string dxc_path;
};

class DxcCompiler {
public:
    DxcCompiler();
    ~DxcCompiler();
    DxcCompiler(const DxcCompiler&) = delete;
    DxcCompiler& operator=(const DxcCompiler&) = delete;

    Result<void> initialize();
    Result<DxcCompileOutput> compile_hlsl(
        std::string_view hlsl_source,
        std::string_view entry_point,
        std::string_view target_profile) const;

    const std::filesystem::path& loaded_dll_path() const { return dll_path_; }

private:
    void* dll_handle_ = nullptr;
    void* create_instance_fn_ = nullptr;
    std::filesystem::path dll_path_;
};

}  // namespace kestrel::gpu
