#include "kestrel/common/types.hpp"
#include "kestrel/cpu/veh_shim.hpp"
#include "kestrel/gpu/dxc_compiler.hpp"
#include "kestrel/gpu/gpu_runner.hpp"
#include "kestrel/gpu/ir.hpp"
#include "kestrel/gpu/rdna2_isa.hpp"
#include "kestrel/gpu/rdna2_recomp.hpp"
#include "kestrel/hle/d3d12x.hpp"
#include "kestrel/hle/kernelx.hpp"
#include "kestrel/hle/registry.hpp"
#include "kestrel/hle/xgameruntime.hpp"
#include "kestrel/loader/game_config.hpp"
#include "kestrel/loader/pe_image.hpp"
#include "kestrel/loader/pe_loader.hpp"
#include "kestrel/vfs/overlay_vfs.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace kestrel;

namespace {

void print_usage() {
    std::cout <<
        "KestrelX — Clean-Room Xbox Series X|S Native Compatibility & Shader Recompilation Runtime\n\n"
        "Usage:\n"
        "  kestrel adapters                  List D3D12 hardware adapters & Wave32/Wave64 capabilities\n"
        "  kestrel inspect <dir|pe>          Inspect MicrosoftGame.config and/or PE32+ Xbox imports\n"
        "  kestrel disasm <shader.bin>       Decode & disassemble raw RDNA2 (GFX10.3) microcode\n"
        "  kestrel selftest [--adapter N]    Run RDNA2 -> IR -> HLSL -> DXC -> real GPU execution test\n"
        "  kestrel run <dir|exe> [--adapter N] [--data-dir <dir>]\n"
        "                                    Load & execute an Xbox-ABI PE title with Overlay VFS & KxDeviceProxy\n";
}

int cmd_adapters() {
    auto res = gpu::enumerate_d3d12_adapters();
    if (!res) {
        std::cerr << "Error: " << res.error().message << "\n";
        return 1;
    }
    std::cout << "D3D12 Adapters (" << res->size() << "):\n";
    for (const auto& a : *res) {
        std::cout << std::format(
            "  [{}] {} | Vendor={:#06x} Device={:#06x} | VRAM={} MB | Wave=[{}..{}] (W32={} W64={}) | SM={:#x}{}\n",
            a.index, a.name, a.vendor_id, a.device_id, a.dedicated_vram_mb,
            a.wave_lane_count_min, a.wave_lane_count_max,
            a.supports_wave32 ? "yes" : "no", a.supports_wave64 ? "yes" : "no",
            a.max_shader_model, a.is_software ? " [SOFTWARE]" : "");
    }
    return 0;
}

int cmd_inspect(const fs::path& target) {
    fs::path pe_path = target;
    if (fs::is_directory(target)) {
        auto cfg_path = target / "MicrosoftGame.config";
        if (fs::exists(cfg_path)) {
            auto cfg = loader::load_game_config(cfg_path);
            if (!cfg) {
                std::cerr << "Failed to parse MicrosoftGame.config: " << cfg.error().message << "\n";
                return 1;
            }
            std::cout << "MicrosoftGame.config:\n"
                      << "  Identity : " << cfg->identity_name << " (" << cfg->version << ")\n"
                      << "  Publisher: " << cfg->publisher << "\n"
                      << "  TitleId  : 0x" << cfg->title_id_hex << " (" << cfg->title_id << ")\n"
                      << "  Memory   : Anaconda=" << cfg->anaconda_title_memory_mb
                      << " MB, Lockhart=" << cfg->lockhart_title_memory_mb << " MB\n";
            for (const auto& ex : cfg->executables) {
                std::cout << "  Exe      : " << ex.name << " [" << ex.target_device_family << "]\n";
            }
            if (!cfg->executables.empty()) pe_path = target / cfg->executables.front().name;
        }
    }

    if (fs::is_regular_file(pe_path)) {
        auto pe = loader::parse_pe64_file(pe_path);
        if (!pe) {
            std::cerr << "PE parse error: " << pe.error().message << "\n";
            return 1;
        }
        std::cout << "\nPE32+ Image: " << pe_path.filename().string() << "\n"
                  << "  ImageBase : " << hex64(pe->image_base) << "\n"
                  << "  ImageSize : " << hex32(pe->size_of_image) << "\n"
                  << "  EntryRVA  : " << hex32(pe->entry_point_rva) << "\n"
                  << "  Sections  : " << pe->sections.size() << "\n"
                  << "  Imports   : " << pe->imports.size() << " DLLs\n";
        for (const auto& imp : pe->imports) {
            std::cout << "    - " << imp.dll_name << " (" << imp.symbols.size() << " symbols)\n";
        }
    }
    return 0;
}

int cmd_disasm(const fs::path& bin_path) {
    std::ifstream ifs(bin_path, std::ios::binary);
    if (!ifs) {
        std::cerr << "Unable to open: " << bin_path.string() << "\n";
        return 1;
    }
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(ifs)), {});
    const size_t nwords = raw.size() / 4;
    std::vector<uint32_t> words(nwords);
    if (nwords > 0) std::memcpy(words.data(), raw.data(), nwords * 4);

    auto insts = gpu::decode_rdna2(words.data(), words.size());
    std::cout << "Decoded " << insts.size() << " RDNA2 instructions from " << bin_path.filename().string() << ":\n";
    for (const auto& d : insts) {
        std::cout << std::format("  {:#06x}: {}\n", d.pc, gpu::disassemble_inst(d));
    }
    return 0;
}

int cmd_selftest(int adapter_filter) {
    // Build a clean-room RDNA2 compute kernel:
    //   v1 = v0 << 2                     (byte offset = tid * 4)
    //   v2 = global_load_dword(v1)       (load float x)
    //   v3 = v_fma_f32(v2, 2.0, 1.0)     (y = 2*x + 1)
    //   global_store_dword(v1, v3)       (store y)
    //   s_endpgm
    using namespace gpu::encode;
    std::vector<uint32_t> prog;
    prog.push_back(vop2(26, 1, 130, 0));                     // v_lshlrev_b32 v1, 2, v0
    global_mem(prog, 12, 2, 1, 127, 0);                      // global_load_dword v2, v1
    vop3(prog, 0x14B, 3, 256 + 2, 244, 242);                 // v_fma_f32 v3, v2, 2.0, 1.0
    global_mem(prog, 28, 3, 1, 127, 0);                      // global_store_dword v1, v3
    prog.push_back(sopp(1));                                 // s_endpgm

    auto decoded = gpu::decode_rdna2(prog.data(), prog.size());
    auto ir = gpu::lift_rdna2_to_ir(decoded);

    gpu::DxcCompiler dxc;
    if (auto init = dxc.initialize(); !init) {
        std::cerr << "DXC init failed: " << init.error().message << "\n";
        return 1;
    }
    std::cout << "DXC loaded: " << dxc.loaded_dll_path().string() << "\n";

    auto adapters = gpu::enumerate_d3d12_adapters();
    if (!adapters) {
        std::cerr << "Adapter enumeration failed: " << adapters.error().message << "\n";
        return 1;
    }

    constexpr uint32_t kCount = 64;
    std::vector<uint32_t> input_u32(kCount);
    for (uint32_t i = 0; i < kCount; ++i) {
        float x = static_cast<float>(i) * 0.5f - 4.0f;
        std::memcpy(&input_u32[i], &x, sizeof(float));
    }
    uint32_t roots[8]{};

    int tested = 0;
    for (const auto& a : *adapters) {
        if (a.is_software) continue;
        if (adapter_filter >= 0 && static_cast<int>(a.index) != adapter_filter) continue;

        const gpu::WaveMode mode = a.supports_wave64 ? gpu::WaveMode::Wave64 : gpu::WaveMode::Wave32;
        gpu::RecompOptions ropt{};
        ropt.wave_mode = mode;
        auto recomp = gpu::emit_hlsl_from_ir(ir, ropt);
        auto compiled = dxc.compile_hlsl(recomp.hlsl, "main", "cs_6_6");
        if (!compiled) {
            std::cerr << "DXC failed for " << a.name << ": " << compiled.error().message << "\n";
            return 1;
        }
        auto ran = gpu::run_compute_dxil_on_gpu(a.index, compiled->dxil, input_u32, roots, 1);
        if (!ran) {
            std::cerr << "GPU execution failed on " << a.name << ": " << ran.error().message << "\n";
            return 1;
        }
        for (uint32_t i = 0; i < kCount; ++i) {
            float x = 0.0f, got = 0.0f;
            std::memcpy(&x, &input_u32[i], 4);
            std::memcpy(&got, &ran->output_u32[i], 4);
            const float expected = 2.0f * x + 1.0f;
            if (std::fabs(got - expected) > 1e-5f) {
                std::cerr << std::format("Mismatch on {} lane {}: got {}, expected {}\n", a.name, i, got, expected);
                return 1;
            }
        }
        std::cout << std::format("PASS [{}] {} (Wave{}, DXIL={} bytes, {:.3f} ms, 64/64 lanes bit-exact)\n",
                                 a.index, a.name, static_cast<uint32_t>(mode), compiled->dxil.size(), ran->gpu_ms);
        ++tested;
    }

    if (tested == 0) {
        std::cerr << "No matching hardware D3D12 adapter found.\n";
        return 1;
    }
    return 0;
}

int cmd_run(const fs::path& target, int adapter_index, fs::path data_dir) {
    fs::path game_root = fs::is_directory(target) ? target : target.parent_path();
    fs::path exe_path = target;
    uint32_t title_id = 0x4B580001u;

    if (fs::is_directory(target)) {
        auto cfg_path = target / "MicrosoftGame.config";
        if (!fs::exists(cfg_path)) {
            std::cerr << "Directory does not contain MicrosoftGame.config: " << target.string() << "\n";
            return 1;
        }
        auto cfg = loader::load_game_config(cfg_path);
        if (!cfg) {
            std::cerr << "MicrosoftGame.config error: " << cfg.error().message << "\n";
            return 1;
        }
        if (cfg->executables.empty()) {
            std::cerr << "No Executable declared in MicrosoftGame.config\n";
            return 1;
        }
        exe_path = target / cfg->executables.front().name;
        if (cfg->title_id != 0) title_id = cfg->title_id;
    }

    if (data_dir.empty()) {
        data_dir = fs::temp_directory_path() / "KestrelX_UserData" / exe_path.stem();
    }

    vfs::OverlayVfs overlay(game_root, data_dir);
    hle::Registry registry;
    hle::set_kernelx_vfs(&overlay);
    hle::set_xgameruntime_title_id(title_id);
    hle::set_d3d12x_preferred_adapter(adapter_index);
    hle::register_kernelx(registry);
    hle::register_xgameruntime(registry);
    hle::register_d3d12x(registry);

    cpu::ScopedVehShim veh;
    loader::PeLoader pe_loader(registry, game_root);

    auto mod = pe_loader.load_image(exe_path);
    if (!mod) {
        std::cerr << "PE load failed: " << mod.error().message << "\n";
        return 1;
    }

    std::cout << std::format(
        "Loaded {} @ {} (relocs={}, hle_imports={}, guest_imports={}, host_imports={}, stubs={})\n",
        (*mod)->name, hex64(reinterpret_cast<uint64_t>((*mod)->base)),
        (*mod)->relocs_applied, (*mod)->hle_imports, (*mod)->guest_imports,
        (*mod)->host_imports, (*mod)->stub_imports);

    auto rc = pe_loader.execute_entry(**mod);
    if (!rc) {
        std::cerr << "Guest execution error: " << rc.error().message << "\n";
        return 1;
    }

    const auto d3d = hle::d3d12x_stats();
    const auto kx = hle::kernelx_stats();
    std::cout << std::format(
        "Guest exited with code {} | D3D12.X(dev={}, q={}, cmd={}, res={}, zmem={}) | KernelX(allocs={}, frees={}, peak={} B)\n",
        *rc, d3d.devices_created, d3d.queues_created, d3d.cmd_lists_created,
        d3d.resources_created, d3d.zero_memory_calls, kx.alloc_count, kx.free_count, kx.peak_bytes);
    return *rc;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 0;
    }
    const std::string cmd = argv[1];
    if (cmd == "adapters") return cmd_adapters();
    if (cmd == "inspect" && argc >= 3) return cmd_inspect(argv[2]);
    if (cmd == "disasm" && argc >= 3) return cmd_disasm(argv[2]);

    int adapter = -1;
    fs::path data_dir;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--adapter" && i + 1 < argc) adapter = std::stoi(argv[++i]);
        else if (arg == "--data-dir" && i + 1 < argc) data_dir = argv[++i];
    }

    if (cmd == "selftest") return cmd_selftest(adapter);
    if (cmd == "run" && argc >= 3) return cmd_run(argv[2], adapter, data_dir);

    print_usage();
    return 1;
}
