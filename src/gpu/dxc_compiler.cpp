#include "kestrel/gpu/dxc_compiler.hpp"

#include <Windows.h>
#include <objidl.h>
#include <oleauto.h>
#include <unknwn.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>

using Microsoft::WRL::ComPtr;

namespace kestrel::gpu {

namespace {

std::wstring to_wide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::vector<std::filesystem::path> candidate_dxc_dirs() {
    std::vector<std::filesystem::path> dirs;
    if (const char* env = std::getenv("KESTREL_DXC_DIR"); env && *env) {
        dirs.emplace_back(env);
    }
    wchar_t exe_buf[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe_buf, MAX_PATH) > 0) {
        dirs.push_back(std::filesystem::path(exe_buf).parent_path());
    }
    const std::filesystem::path kits_bin(R"(C:\Program Files (x86)\Windows Kits\10\bin)");
    std::error_code ec;
    if (std::filesystem::exists(kits_bin, ec)) {
        std::vector<std::filesystem::path> vers;
        for (const auto& entry : std::filesystem::directory_iterator(kits_bin, ec)) {
            if (entry.is_directory()) {
                auto x64 = entry.path() / "x64";
                if (std::filesystem::exists(x64 / "dxcompiler.dll", ec)) {
                    vers.push_back(x64);
                }
            }
        }
        std::sort(vers.begin(), vers.end(), std::greater<>());
        for (auto& v : vers) dirs.push_back(std::move(v));
    }
    return dirs;
}

}  // namespace

DxcCompiler::DxcCompiler() = default;

DxcCompiler::~DxcCompiler() {
    if (dll_handle_) {
        FreeLibrary(static_cast<HMODULE>(dll_handle_));
        dll_handle_ = nullptr;
    }
}

Result<void> DxcCompiler::initialize() {
    if (dll_handle_ && create_instance_fn_) return {};

    for (const auto& dir : candidate_dxc_dirs()) {
        auto dll = dir / "dxcompiler.dll";
        std::error_code ec;
        if (!std::filesystem::exists(dll, ec)) continue;
        // Preload dxil.dll from the same directory if present so DXC signs DXIL containers.
        auto dxil = dir / "dxil.dll";
        if (std::filesystem::exists(dxil, ec)) {
            LoadLibraryW(dxil.c_str());
        }
        HMODULE mod = LoadLibraryW(dll.c_str());
        if (mod) {
            auto fn = GetProcAddress(mod, "DxcCreateInstance");
            if (fn) {
                dll_handle_ = mod;
                create_instance_fn_ = reinterpret_cast<void*>(fn);
                dll_path_ = dll;
                return {};
            }
            FreeLibrary(mod);
        }
    }

    HMODULE mod = LoadLibraryW(L"dxcompiler.dll");
    if (mod) {
        auto fn = GetProcAddress(mod, "DxcCreateInstance");
        if (fn) {
            dll_handle_ = mod;
            create_instance_fn_ = reinterpret_cast<void*>(fn);
            dll_path_ = "dxcompiler.dll";
            return {};
        }
        FreeLibrary(mod);
    }

    return fail("Unable to locate dxcompiler.dll (set KESTREL_DXC_DIR or install Windows 11 SDK)");
}

Result<DxcCompileOutput> DxcCompiler::compile_hlsl(
    std::string_view hlsl_source,
    std::string_view entry_point,
    std::string_view target_profile) const {
    if (!create_instance_fn_) {
        return fail("DxcCompiler not initialized");
    }

    auto create_instance = reinterpret_cast<DxcCreateInstanceProc>(create_instance_fn_);
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    HRESULT hr = create_instance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
    if (FAILED(hr)) return fail(std::format("DxcCreateInstance(IDxcUtils) failed: {:#x}", static_cast<uint32_t>(hr)));
    hr = create_instance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
    if (FAILED(hr)) return fail(std::format("DxcCreateInstance(IDxcCompiler3) failed: {:#x}", static_cast<uint32_t>(hr)));

    const std::wstring w_entry = to_wide(entry_point);
    const std::wstring w_target = to_wide(target_profile);
    LPCWSTR args[] = {
        L"-E", w_entry.c_str(),
        L"-T", w_target.c_str(),
        L"-HV", L"2021",
        L"-O3",
    };

    DxcBuffer src_buf{};
    src_buf.Ptr = hlsl_source.data();
    src_buf.Size = hlsl_source.size();
    src_buf.Encoding = DXC_CP_UTF8;

    ComPtr<IDxcResult> result;
    hr = compiler->Compile(&src_buf, args, static_cast<UINT32>(std::size(args)), nullptr, IID_PPV_ARGS(&result));
    if (FAILED(hr) || !result) {
        return fail(std::format("IDxcCompiler3::Compile failed: {:#x}", static_cast<uint32_t>(hr)));
    }

    ComPtr<IDxcBlobUtf8> errs;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errs), nullptr);
    std::string err_text;
    if (errs && errs->GetStringLength() > 0) {
        err_text.assign(errs->GetStringPointer(), errs->GetStringLength());
    }

    HRESULT status = S_OK;
    result->GetStatus(&status);
    if (FAILED(status)) {
        return fail(std::format("DXC compilation failed ({:#x}):\n{}", static_cast<uint32_t>(status), err_text));
    }

    ComPtr<IDxcBlob> obj;
    hr = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr);
    if (FAILED(hr) || !obj || obj->GetBufferSize() == 0) {
        return fail("DXC returned empty DXIL object");
    }

    DxcCompileOutput out{};
    out.dxil.resize(obj->GetBufferSize());
    std::memcpy(out.dxil.data(), obj->GetBufferPointer(), obj->GetBufferSize());
    out.warnings = std::move(err_text);
    out.dxc_path = dll_path_.string();
    return out;
}

}  // namespace kestrel::gpu
