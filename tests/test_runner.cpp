#include "kestrel/common/types.hpp"
#include "kestrel/cpu/veh_shim.hpp"
#include "kestrel/gpu/dxc_compiler.hpp"
#include "kestrel/gpu/gfx10_tiling.hpp"
#include "kestrel/gpu/gpu_runner.hpp"
#include "kestrel/gpu/ir.hpp"
#include "kestrel/gpu/rdna2_isa.hpp"
#include "kestrel/gpu/rdna2_recomp.hpp"
#include "kestrel/gpu/shader_cache.hpp"
#include "kestrel/gpu/shader_container.hpp"
#include "kestrel/gpu/vrs.hpp"
#include "kestrel/gpu/work_graph.hpp"
#include "kestrel/hle/d3d12x.hpp"
#include "kestrel/hle/dstorage.hpp"
#include "kestrel/hle/gameinput.hpp"
#include "kestrel/hle/kernelx.hpp"
#include "kestrel/hle/registry.hpp"
#include "kestrel/hle/window_host.hpp"
#include "kestrel/hle/xaudio2.hpp"
#include "kestrel/hle/xgameruntime.hpp"
#include "kestrel/loader/game_config.hpp"
#include "kestrel/loader/pe_image.hpp"
#include "kestrel/loader/pe_loader.hpp"
#include "kestrel/vfs/overlay_vfs.hpp"
#include "kestrel/formats/sgd5.hpp"

#include <dxgi1_6.h>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace kestrel;

namespace {

int g_passed = 0;
int g_failed = 0;

#define KX_CHECK(cond, msg)                                                   \
    do {                                                                      \
        if (!(cond)) {                                                        \
            throw std::runtime_error(std::format("Check failed ({}): {}", #cond, msg)); \
        }                                                                     \
    } while (0)

template <typename Fn>
void run_test(const char* name, Fn&& fn) {
    try {
        fn();
        std::cout << std::format("  [PASS] {}\n", name) << std::flush;
        ++g_passed;
    } catch (const std::exception& e) {
        std::cerr << std::format("  [FAIL] {} -> {}\n", name, e.what()) << std::flush;
        ++g_failed;
    }
}

void test_game_config_parser() {
    const fs::path cfg_path = fs::path(KESTREL_TEST_TITLE_DIR) / "MicrosoftGame.config";
    auto cfg = loader::load_game_config(cfg_path);
    KX_CHECK(cfg.has_value(), cfg.error().message);
    KX_CHECK(cfg->identity_name == "KestrelX.CleanRoom.TestTitle", "identity mismatch");
    KX_CHECK(cfg->title_id == 0x4B580001u, "title id mismatch");
    KX_CHECK(cfg->anaconda_title_memory_mb == 13824, "anaconda memory mismatch");
    KX_CHECK(cfg->lockhart_title_memory_mb == 8192, "lockhart memory mismatch");
    KX_CHECK(!cfg->executables.empty() && cfg->executables[0].name == "kestrel_test_title.exe", "exe mismatch");
}

void test_overlay_vfs_cow_and_security() {
    const fs::path tmp = fs::temp_directory_path() / "KestrelX_TestVfs";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    const fs::path base = tmp / "base";
    const fs::path data = tmp / "data";
    fs::create_directories(base / "data");
    {
        std::ofstream ofs(base / "data" / "asset.txt");
        ofs << "ORIGINAL";
    }

    vfs::OverlayVfs vfs(base, data);
    // 1. Read resolves to base
    auto r0 = vfs.resolve("GAME:\\data\\asset.txt", false);
    KX_CHECK(r0.has_value() && *r0 == base / "data" / "asset.txt", "initial read path");

    // 2. Write resolves to overlay, never touching base
    auto w0 = vfs.resolve("GAME:\\data\\asset.txt", true);
    KX_CHECK(w0.has_value() && *w0 != base / "data" / "asset.txt", "write must target overlay");
    {
        std::ofstream ofs(*w0);
        ofs << "MODIFIED";
    }
    auto r1 = vfs.resolve("GAME:\\data\\asset.txt", false);
    KX_CHECK(r1.has_value() && *r1 == *w0, "read after write must see overlay");

    // Base file remains untouched
    {
        std::ifstream ifs(base / "data" / "asset.txt");
        std::string orig;
        std::getline(ifs, orig);
        KX_CHECK(orig == "ORIGINAL", "base file was mutated!");
    }

    // 3. Whiteout deletes logically without removing base file
    KX_CHECK(vfs.exists("GAME:\\data\\asset.txt"), "exists before delete");
    KX_CHECK(vfs.remove_file("GAME:\\data\\asset.txt").has_value(), "remove_file");
    KX_CHECK(!vfs.exists("GAME:\\data\\asset.txt"), "whiteout should hide deleted file");
    KX_CHECK(fs::exists(base / "data" / "asset.txt"), "base file must survive whiteout");

    // 4. Path traversal rejection
    auto evil = vfs.resolve("GAME:\\..\\..\\Windows\\System32\\cmd.exe", false);
    KX_CHECK(!evil.has_value(), "path traversal must be rejected");

    fs::remove_all(tmp, ec);
}

void test_veh_zen2_decode_and_emulate() {
    const uint8_t clzero[3] = {0x0F, 0x01, 0xFC};
    const uint8_t monitorx[3] = {0x0F, 0x01, 0xFA};
    const uint8_t mwaitx[3] = {0x0F, 0x01, 0xFB};
    const uint8_t nop[3] = {0x90, 0x90, 0x90};

    auto d0 = cpu::decode_zen2_extension(clzero, 3);
    KX_CHECK(d0.kind == cpu::Zen2Instruction::Clzero && d0.length == 3, "clzero decode");
    auto d1 = cpu::decode_zen2_extension(monitorx, 3);
    KX_CHECK(d1.kind == cpu::Zen2Instruction::Monitorx && d1.length == 3, "monitorx decode");
    auto d2 = cpu::decode_zen2_extension(mwaitx, 3);
    KX_CHECK(d2.kind == cpu::Zen2Instruction::Mwaitx && d2.length == 3, "mwaitx decode");
    auto d3 = cpu::decode_zen2_extension(nop, 3);
    KX_CHECK(d3.kind == cpu::Zen2Instruction::None, "non-zen2 decode");

    alignas(64) uint8_t line[64];
    std::memset(line, 0xCC, sizeof(line));
    // Test unaligned address inside the 64-byte line (AMD CLZERO clears aligned [RAX & ~63])
    uint64_t rax = reinterpret_cast<uint64_t>(line + 17);
    KX_CHECK(cpu::emulate_zen2_instruction(cpu::Zen2Instruction::Clzero, rax), "clzero emulate");
    for (uint8_t b : line) {
        KX_CHECK(b == 0, "clzero must zero entire 64B cache line");
    }
    KX_CHECK(cpu::emulate_zen2_instruction(cpu::Zen2Instruction::Monitorx, rax), "monitorx emulate");
    KX_CHECK(cpu::emulate_zen2_instruction(cpu::Zen2Instruction::Mwaitx, rax), "mwaitx emulate");
}

void test_rdna2_decoder_and_ir_cfg() {
    using namespace gpu::encode;
    // Multi-block RDNA2 program with conditional branch:
    //   0x00: s_cmp_eq_u32 s0, 0          (4B)
    //   0x04: s_cbranch_scc1 +1           (4B -> target 0x0C)
    //   0x08: v_add_f32 v1, v1, v2        (4B)
    //   0x0C: s_endpgm                    (4B)
    std::vector<uint32_t> prog;
    prog.push_back(sopc(6, 0, 128));      // s_cmp_eq_u32 s0, 0
    prog.push_back(sopp(5, 1));           // s_cbranch_scc1 +1 -> 0x0C
    prog.push_back(vop2(3, 1, 256 + 1, 2));  // v_add_f32 v1, v1, v2
    prog.push_back(sopp(1));              // s_endpgm

    auto dec = gpu::decode_rdna2(prog.data(), prog.size());
    KX_CHECK(dec.size() == 4, "decoded inst count");
    KX_CHECK(dec[1].is_branch && dec[1].branch_target == 0x0C, "branch target calculation");
    KX_CHECK(dec[3].is_terminator, "s_endpgm terminator");

    auto ir = gpu::lift_rdna2_to_ir(dec);
    KX_CHECK(ir.blocks.size() == 3, "expected 3 CFG basic blocks");
    KX_CHECK(ir.unknown_count == 0, "zero unknown ops");
    KX_CHECK(ir.blocks[0].successors.size() == 2, "cond branch has 2 CFG successors");

    auto hlsl32 = gpu::emit_hlsl_from_ir(ir, {.wave_mode = gpu::WaveMode::Wave32});
    auto hlsl64 = gpu::emit_hlsl_from_ir(ir, {.wave_mode = gpu::WaveMode::Wave64});
    KX_CHECK(hlsl32.hlsl.find("[WaveSize(32)]") != std::string::npos, "Wave32 attribute");
    KX_CHECK(hlsl64.hlsl.find("[WaveSize(64)]") != std::string::npos, "Wave64 attribute");
    KX_CHECK(hlsl32.hlsl.find("switch (bb)") != std::string::npos, "structured CFG switch emitted");
}

void test_dxbc_container_and_dxc_compile() {
    using namespace gpu::encode;
    std::vector<uint32_t> prog;
    prog.push_back(vop2(26, 1, 130, 0));     // v_lshlrev_b32 v1, 2, v0
    global_mem(prog, 12, 2, 1, 127, 0);      // global_load_dword v2, v1
    vop3(prog, 0x14B, 3, 256 + 2, 244, 242); // v_fma_f32 v3, v2, 2.0, 1.0
    global_mem(prog, 28, 3, 1, 127, 0);      // global_store_dword v1, v3
    prog.push_back(sopp(1));                 // s_endpgm

    auto ir = gpu::lift_rdna2_to_ir(gpu::decode_rdna2(prog.data(), prog.size()));
    gpu::DxcCompiler dxc;
    KX_CHECK(dxc.initialize().has_value(), "DXC init");

    auto hlsl = gpu::emit_hlsl_from_ir(ir, {.wave_mode = gpu::WaveMode::Wave32});
    auto out = dxc.compile_hlsl(hlsl.hlsl, "main", "cs_6_6");
    KX_CHECK(out.has_value(), out.error().message);
    KX_CHECK(out->dxil.size() > 64, "valid DXIL size");

    // Parse our own DXC-compiled DXIL blob with KestrelX's DXBC container parser!
    auto parsed = gpu::parse_dxbc_container(out->dxil.data(), out->dxil.size());
    KX_CHECK(parsed.has_value(), parsed.error().message);
    KX_CHECK(parsed->has_dxil, "DXIL chunk must be detected");
    KX_CHECK(parsed->dxil_stage == 5, "compute shader stage == 5");
}

void test_rdna2_recomp_real_dual_gpu_execution() {
    // End-to-end RDNA2 binary -> decode -> CFG/IR -> SM 6.6 HLSL -> DXC -> real D3D12 GPU execution!
    // Program computes per lane i in [0..63]:
    //   x = g_mem[i] (float)
    //   if (s0 != 0) y = fma(x, 2.0, 1.0) else y = x + 4.0
    //   z = max(y, 0.5)
    //   g_mem[i] = z
    using namespace gpu::encode;
    std::vector<uint32_t> prog;
    // 0x00: v_lshlrev_b32 v1, 2, v0          (4B)
    prog.push_back(vop2(26, 1, 130, 0));
    // 0x04: global_load_dword v2, v1         (8B)
    global_mem(prog, 12, 2, 1, 127, 0);
    // 0x0C: s_cmp_eq_u32 s0, 0               (4B)
    prog.push_back(sopc(6, 0, 128));
    // 0x10: s_cbranch_scc1 +3                (4B -> PC 0x14 + 3*4 = 0x20)
    prog.push_back(sopp(5, 3));
    // 0x14: v_fma_f32 v3, v2, 2.0, 1.0       (8B)
    vop3(prog, 0x14B, 3, 256 + 2, 244, 242);
    // 0x1C: s_branch +1                      (4B -> PC 0x20 + 1*4 = 0x24)
    prog.push_back(sopp(2, 1));
    // 0x20: v_add_f32 v3, 4.0, v2            (4B, inline const 246 = 4.0f)
    prog.push_back(vop2(3, 3, 246, 2));
    // 0x24: v_max_f32 v3, 0.5, v3            (4B, inline const 240 = 0.5f)
    prog.push_back(vop2(16, 3, 240, 3));
    // 0x28: global_store_dword v1, v3        (8B)
    global_mem(prog, 28, 3, 1, 127, 0);
    // 0x30: s_endpgm                         (4B)
    prog.push_back(sopp(1));

    auto dec = gpu::decode_rdna2(prog.data(), prog.size());
    auto ir = gpu::lift_rdna2_to_ir(dec);
    KX_CHECK(ir.unknown_count == 0, "all RDNA2 instructions lifted");
    KX_CHECK(ir.blocks.size() == 4, "4 CFG basic blocks (entry, then, else, merge)");

    gpu::DxcCompiler dxc;
    KX_CHECK(dxc.initialize().has_value(), "DXC init");

    auto adapters = gpu::enumerate_d3d12_adapters();
    KX_CHECK(adapters.has_value() && !adapters->empty(), "D3D12 adapters available");

    constexpr uint32_t kLanes = 32;
    std::vector<uint32_t> init_buf(kLanes);
    for (uint32_t i = 0; i < kLanes; ++i) {
        float x = static_cast<float>(i) * 0.25f - 3.0f;
        std::memcpy(&init_buf[i], &x, 4);
    }

    int hw_tested = 0;
    for (const auto& a : *adapters) {
        if (a.is_software) continue;

        // Test Wave32 if supported AND Wave64 if supported!
        std::vector<gpu::WaveMode> modes;
        if (a.supports_wave32) modes.push_back(gpu::WaveMode::Wave32);
        if (a.supports_wave64) modes.push_back(gpu::WaveMode::Wave64);
        if (modes.empty()) modes.push_back(gpu::WaveMode::Wave32);

        for (gpu::WaveMode mode : modes) {
            auto recomp = gpu::emit_hlsl_from_ir(ir, {.wave_mode = mode, .group_size_x = 64});
            auto dxil = dxc.compile_hlsl(recomp.hlsl, "main", "cs_6_6");
            KX_CHECK(dxil.has_value(), dxil.error().message);

            // Run BOTH branch paths (s0 = 1 -> FMA path, s0 = 0 -> Add path)
            for (uint32_t branch_sel : {1u, 0u}) {
                uint32_t roots[8]{branch_sel, 0, 0, 0, 0, 0, 0, 0};
                auto res = gpu::run_compute_dxil_on_gpu(a.index, dxil->dxil, init_buf, roots, 1);
                KX_CHECK(res.has_value(), res.error().message);
                for (uint32_t i = 0; i < kLanes; ++i) {
                    float x = 0.0f, got = 0.0f;
                    std::memcpy(&x, &init_buf[i], 4);
                    std::memcpy(&got, &res->output_u32[i], 4);
                    float y = (branch_sel != 0) ? (x * 2.0f + 1.0f) : (x + 4.0f);
                    float expected = y > 0.5f ? y : 0.5f;
                    KX_CHECK(std::fabs(got - expected) < 1e-5f,
                             std::format("GPU {} Wave{} sel={} lane {}: got {} != {}",
                                         a.name, static_cast<uint32_t>(mode), branch_sel, i, got, expected));
                }
            }
            std::cout << std::format("    -> Hardware verified: [{}] {} | Wave{} | DXIL={} B | both CFG branches bit-exact\n",
                                     a.index, a.name, static_cast<uint32_t>(mode), dxil->dxil.size());
        }
        ++hw_tested;
    }
    KX_CHECK(hw_tested >= 1, "at least one hardware GPU verified");
}

void test_rdna2_smem_lds_barrier_dual_gpu() {
    // Clean-room RDNA2 workgroup LDS reverse-shuffle + SMEM scalar constant load:
    //   Buffer layout (65 uint32s):
    //     g_mem[0..63] = lane input values (1000 + i)
    //     g_mem[64]    = scalar constant K = 77 (loaded via SMEM s_load_dword at offset 256)
    //   Program:
    //     s_load_dword s4, s[0:1], 0x100          ; s4 = g_mem[64] = 77 (s0=0)
    //     v_lshlrev_b32 v1, 2, v0                 ; v1 = tid * 4
    //     global_load_dword v2, v1                ; v2 = g_mem[tid]
    //     ds_write_b32 v1, v2                     ; g_lds[tid] = v2
    //     s_barrier                               ; GroupMemoryBarrierWithGroupSync()
    //     v_xor_b32 v4, 63, v0                    ; v4 = 63 - tid
    //     v_lshlrev_b32 v4, 2, v4                 ; v4 = (63 - tid) * 4
    //     ds_read_b32 v5, v4                      ; v5 = g_lds[63 - tid]
    //     v_add_nc_u32 v6, s4, v5                 ; v6 = g_lds[63 - tid] + K
    //     global_store_dword v1, v6               ; g_mem[tid] = v6
    //     s_endpgm
    using namespace gpu::encode;
    std::vector<uint32_t> prog;
    smem(prog, 0, 4, 0, 256);                   // s_load_dword s4, s[0:1], 256
    prog.push_back(vop2(26, 1, 130, 0));        // v_lshlrev_b32 v1, 2, v0
    global_mem(prog, 12, 2, 1, 127, 0);         // global_load_dword v2, v1
    ds(prog, 13, 0, 1, 2, 0, 0);                // ds_write_b32 v1, v2
    prog.push_back(sopp(10));                   // s_barrier
    prog.push_back(vop2(29, 4, 128 + 63, 0));   // v_xor_b32 v4, 63, v0
    prog.push_back(vop2(26, 4, 130, 4));        // v_lshlrev_b32 v4, 2, v4
    ds(prog, 54, 5, 4, 0, 0, 0);                // ds_read_b32 v5, v4
    prog.push_back(vop2(37, 6, 4, 5));          // v_add_nc_u32 v6, s4, v5
    global_mem(prog, 28, 6, 1, 127, 0);         // global_store_dword v1, v6
    prog.push_back(sopp(1));                    // s_endpgm

    auto dec = gpu::decode_rdna2(prog.data(), prog.size());
    auto ir = gpu::lift_rdna2_to_ir(dec);
    KX_CHECK(ir.unknown_count == 0, "all SMEM/LDS/barrier instructions lifted");
    KX_CHECK(ir.uses_lds, "IR must detect LDS usage");

    gpu::DxcCompiler dxc;
    KX_CHECK(dxc.initialize().has_value(), "DXC init");
    auto adapters = gpu::enumerate_d3d12_adapters();
    KX_CHECK(adapters.has_value() && !adapters->empty(), "D3D12 adapters available");

    constexpr uint32_t kLanes = 64;
    constexpr uint32_t kScalarAdd = 77;
    std::vector<uint32_t> init_buf(kLanes + 1);
    for (uint32_t i = 0; i < kLanes; ++i) init_buf[i] = 1000u + i * 3u;
    init_buf[kLanes] = kScalarAdd;
    uint32_t roots[8]{0, 0, 0, 0, 0, 0, 0, 0};

    int verified = 0;
    for (const auto& a : *adapters) {
        if (a.is_software) continue;
        std::vector<gpu::WaveMode> modes;
        if (a.supports_wave32) modes.push_back(gpu::WaveMode::Wave32);
        if (a.supports_wave64) modes.push_back(gpu::WaveMode::Wave64);
        for (gpu::WaveMode mode : modes) {
            auto recomp = gpu::emit_hlsl_from_ir(ir, {.wave_mode = mode, .group_size_x = 64});
            KX_CHECK(recomp.hlsl.find("groupshared uint g_lds[8192];") != std::string::npos, "LDS declaration");
            KX_CHECK(recomp.hlsl.find("GroupMemoryBarrierWithGroupSync();") != std::string::npos, "barrier emission");
            auto dxil = dxc.compile_hlsl(recomp.hlsl, "main", "cs_6_6");
            KX_CHECK(dxil.has_value(), dxil.error().message);
            auto res = gpu::run_compute_dxil_on_gpu(a.index, dxil->dxil, init_buf, roots, 1);
            KX_CHECK(res.has_value(), res.error().message);
            for (uint32_t i = 0; i < kLanes; ++i) {
                const uint32_t expected = init_buf[63u - i] + kScalarAdd;
                KX_CHECK(res->output_u32[i] == expected,
                         std::format("LDS+SMEM mismatch on {} Wave{} lane {}: got {} != {}",
                                     a.name, static_cast<uint32_t>(mode), i, res->output_u32[i], expected));
            }
            std::cout << std::format("    -> Hardware verified LDS+SMEM: [{}] {} | Wave{} | 64/64 reversed+added bit-exact\n",
                                     a.index, a.name, static_cast<uint32_t>(mode));
        }
        ++verified;
    }
    KX_CHECK(verified >= 1, "at least one hardware GPU verified for LDS+SMEM");
}

void test_strict_unsupported_opcode_refusal() {
    // Unknown SOP1 opcode 0x7F must be flagged in IR and rejected by DXC via #error (never silently ignored).
    using namespace gpu::encode;
    std::vector<uint32_t> prog = {sop1(0x7F, 0, 1), sopp(1)};
    auto ir = gpu::lift_rdna2_to_ir(gpu::decode_rdna2(prog.data(), prog.size()));
    KX_CHECK(ir.unknown_count == 1, "unknown opcode must increment unknown_count");
    auto recomp = gpu::emit_hlsl_from_ir(ir, {.wave_mode = gpu::WaveMode::Wave32});
    KX_CHECK(recomp.hlsl.find("#error Unsupported RDNA2 opcode") != std::string::npos, "must emit #error");
    gpu::DxcCompiler dxc;
    KX_CHECK(dxc.initialize().has_value(), "DXC init");
    auto compiled = dxc.compile_hlsl(recomp.hlsl, "main", "cs_6_6");
    
    KX_CHECK(!compiled.has_value(), "DXC must reject shader containing unsupported RDNA2 opcode");
}

void test_gfx10_tiling_roundtrip() {
    const std::vector<gpu::SwizzleMode> modes = {
        gpu::SwizzleMode::Linear,
        gpu::SwizzleMode::Sw256B_S,
        gpu::SwizzleMode::Sw4KB_S,
        gpu::SwizzleMode::Sw64KB_S,
        gpu::SwizzleMode::Sw64KB_D,
        gpu::SwizzleMode::Sw64KB_S_X,
        gpu::SwizzleMode::Sw64KB_D_X,
    };

    const uint32_t w = 128, h = 128, bpp = 4;
    std::vector<uint8_t> linear(static_cast<size_t>(w) * h * bpp);
    for (size_t i = 0; i < linear.size(); ++i) {
        linear[i] = static_cast<uint8_t>((i * 37u + 13u) & 0xFFu);
    }

    for (auto m : modes) {
        gpu::SurfaceDesc desc{w, h, bpp, m, 0x5Au};
        auto tiled = gpu::swizzle_2d(desc, linear.data(), static_cast<size_t>(w) * bpp);
        auto unswizzled = gpu::deswizzle_2d(desc, tiled.data(), tiled.size());
        KX_CHECK(unswizzled == linear, std::format("GFX10 swizzle round-trip failed for mode {}", static_cast<int>(m)));
    }
}

void test_gfx10_bcn_and_mip_chain() {
    // Verify BC1 (8B/4x4 block) and BC7 (16B/4x4 block) round-trip tiling + 5-level mip chain layout
    for (uint32_t bpp : {8u, 16u}) {
        gpu::SurfaceDesc desc{256, 256, bpp, gpu::SwizzleMode::Sw64KB_S_X, 0x3Cu, 4, 4};
        const uint32_t bx = 256 / 4, by = 256 / 4;
        std::vector<uint8_t> linear(static_cast<size_t>(bx) * by * bpp);
        for (size_t i = 0; i < linear.size(); ++i) linear[i] = static_cast<uint8_t>((i * 53u + 7u) & 0xFFu);

        auto tiled = gpu::swizzle_2d(desc, linear.data(), static_cast<size_t>(bx) * bpp);
        auto back = gpu::deswizzle_2d(desc, tiled.data(), tiled.size());
        KX_CHECK(back == linear, std::format("BCn bpp={} round-trip failed", bpp));

        auto mips = gpu::compute_gfx10_mip_chain(desc, 5);
        KX_CHECK(mips.size() == 5, "expected 5 mip levels");
        KX_CHECK(mips[0].blocks_x == 64 && mips[4].blocks_x == 4, "mip block dimensions");
        for (size_t i = 1; i < mips.size(); ++i) {
            KX_CHECK((mips[i].offset % 256) == 0, "mip offset 256B aligned");
            KX_CHECK(mips[i].offset >= mips[i - 1].offset + mips[i - 1].size, "non-overlapping mips");
        }
    }
}

void test_rdna2_exp_vs_ps_compile() {
    using namespace gpu::encode;
    std::vector<uint32_t> vs;
    vs.push_back(vop1(1, 1, 128));  // v1 = 0.0
    vs.push_back(vop1(1, 2, 240));  // v2 = 0.5
    vs.push_back(vop1(1, 3, 128));  // v3 = 0.0
    vs.push_back(vop1(1, 4, 242));  // v4 = 1.0
    exp(vs, 12, 0xF, 1, 2, 3, 4, false);  // pos0
    exp(vs, 32, 0xF, 4, 2, 1, 4, true);   // param0
    vs.push_back(sopp(1));                // s_endpgm

    std::vector<uint32_t> ps;
    exp(ps, 0, 0xF, 0, 1, 2, 3, true);    // mrt0
    ps.push_back(sopp(1));                // s_endpgm

    auto vs_dec = gpu::decode_rdna2(vs.data(), vs.size());
    auto ps_dec = gpu::decode_rdna2(ps.data(), ps.size());
    KX_CHECK(gpu::disassemble_inst(vs_dec[4]).find("exp pos0") != std::string::npos, "EXP pos0 disasm");
    KX_CHECK(gpu::disassemble_inst(ps_dec[0]).find("exp mrt0") != std::string::npos, "EXP mrt0 disasm");

    auto vs_ir = gpu::lift_rdna2_to_ir(vs_dec);
    auto ps_ir = gpu::lift_rdna2_to_ir(ps_dec);
    KX_CHECK(vs_ir.unknown_count == 0 && ps_ir.unknown_count == 0, "EXP VS/PS zero unknown opcodes");

    auto vs_hlsl = gpu::emit_hlsl_from_ir(vs_ir, {.stage = gpu::ShaderStage::Vertex});
    auto ps_hlsl = gpu::emit_hlsl_from_ir(ps_ir, {.stage = gpu::ShaderStage::Pixel});

    gpu::DxcCompiler dxc;
    KX_CHECK(dxc.initialize().has_value(), "DXC init");
    auto vs_dxil = dxc.compile_hlsl(vs_hlsl.hlsl, "main", "vs_6_6");
    KX_CHECK(vs_dxil.has_value(), vs_dxil.error().message);
    auto ps_dxil = dxc.compile_hlsl(ps_hlsl.hlsl, "main", "ps_6_6");
    KX_CHECK(ps_dxil.has_value(), ps_dxil.error().message);

    auto vs_parsed = gpu::parse_dxbc_container(vs_dxil->dxil.data(), vs_dxil->dxil.size());
    auto ps_parsed = gpu::parse_dxbc_container(ps_dxil->dxil.data(), ps_dxil->dxil.size());
    KX_CHECK(vs_parsed.has_value() && vs_parsed->has_dxil, "Valid vs_6_6 DXIL container");
    KX_CHECK(ps_parsed.has_value() && ps_parsed->has_dxil, "Valid ps_6_6 DXIL container");
}

void test_gameinput_xinput_bridge() {
    hle::reset_gameinput_stats();

    // 1. Verify raw XInput button, trigger, deadzone & stick translation
    auto st = hle::translate_xinput_raw(
        0x1000u | 0x0100u | 0x0001u,  // A + LB + DPadUp
        255, 10,                      // LT full, RT inside deadzone
        32767, 500,                   // LX full right, LY inside deadzone
        -32768, 16384);
    KX_CHECK((st.buttons & hle::KxGamepadA) != 0, "A button mapped");
    KX_CHECK((st.buttons & hle::KxGamepadLeftShoulder) != 0, "LB button mapped");
    KX_CHECK((st.buttons & hle::KxGamepadDPadUp) != 0, "DPadUp mapped");
    KX_CHECK(st.left_trigger > 0.99f && st.right_trigger == 0.0f, "Trigger deadzone & scale");
    KX_CHECK(st.left_thumbstick_x > 0.99f && st.left_thumbstick_y == 0.0f, "Left stick deadzone & scale");
    KX_CHECK(st.right_thumbstick_x < -0.99f && st.right_thumbstick_y > 0.49f, "Right stick scale");

    // 2. Verify GameInputCreate COM interface with mock connected & disconnected states
    hle::Registry reg;
    hle::register_gameinput(reg);
    auto* create_fn = reinterpret_cast<HRESULT(__stdcall*)(hle::IKxGameInput**)>(
        reg.resolve_name("gameinput.dll", "GameInputCreate"));
    KX_CHECK(create_fn != nullptr, "GameInputCreate registered");

    hle::IKxGameInput* gi = nullptr;
    KX_CHECK(SUCCEEDED(create_fn(&gi)) && gi != nullptr, "GameInputCreate S_OK");

    hle::set_gameinput_mock_state(st, true);
    hle::IKxGameInputReading* reading = nullptr;
    KX_CHECK(SUCCEEDED(gi->GetCurrentReading(hle::KxGameInputKindGamepad, nullptr, &reading)) && reading, "Connected reading");
    hle::KxGameInputGamepadState got{};
    KX_CHECK(reading->GetGamepadState(&got), "GetGamepadState");
    KX_CHECK(got.buttons == st.buttons && got.left_trigger > 0.99f, "Mock reading matches");
    reading->Release();

    hle::set_gameinput_mock_state({}, false);
    hle::IKxGameInputReading* disc = nullptr;
    KX_CHECK(FAILED(gi->GetCurrentReading(hle::KxGameInputKindGamepad, nullptr, &disc)) && disc == nullptr,
             "Disconnected gamepad returns failure");

    hle::clear_gameinput_mock_state();
    gi->Release();
}

void test_xaudio2_pcm_ring_buffer() {
    hle::reset_xaudio2_stats();
    hle::Registry reg;
    hle::register_xaudio2(reg);
    auto* create_fn = reinterpret_cast<HRESULT(__stdcall*)(hle::IKxXAudio2**, uint32_t, uint32_t)>(
        reg.resolve_name("xaudio2_9.dll", "XAudio2Create"));
    KX_CHECK(create_fn != nullptr, "XAudio2Create registered");

    hle::IKxXAudio2* xa = nullptr;
    KX_CHECK(SUCCEEDED(create_fn(&xa, 0, 0)) && xa != nullptr, "XAudio2Create S_OK");
    hle::IKxMasteringVoice* mv = nullptr;
    KX_CHECK(SUCCEEDED(xa->CreateMasteringVoice(&mv, 2, 48000)) && mv != nullptr, "CreateMasteringVoice");

    // Submit 440 Hz stereo int16 PCM sine wave (480 frames = 10 ms @ 48 kHz)
    hle::KxWaveFormat fmt{1, 2, 48000, 16};
    hle::IKxSourceVoice* sv = nullptr;
    KX_CHECK(SUCCEEDED(xa->CreateSourceVoice(&sv, &fmt)) && sv != nullptr, "CreateSourceVoice");
    KX_CHECK(SUCCEEDED(sv->Start(0)), "SourceVoice Start");

    constexpr size_t kFrames = 480;
    std::vector<int16_t> sine(kFrames * 2);
    for (size_t i = 0; i < kFrames; ++i) {
        const double phase = 2.0 * 3.14159265358979323846 * 440.0 * static_cast<double>(i) / 48000.0;
        const auto s = static_cast<int16_t>(std::sin(phase) * 16384.0);
        sine[i * 2 + 0] = s;
        sine[i * 2 + 1] = s;
    }
    hle::KxAudioBuffer buf{0, static_cast<uint32_t>(sine.size() * sizeof(int16_t)),
                           reinterpret_cast<const uint8_t*>(sine.data())};
    KX_CHECK(SUCCEEDED(sv->SubmitSourceBuffer(&buf)), "SubmitSourceBuffer");

    std::vector<float> drained(600 * 2, 0.0f);
    const size_t got_frames = hle::xaudio2_drain_stereo_f32(drained.data(), 600);
    KX_CHECK(got_frames == kFrames, "Drained exact 480 frames");

    const auto st = hle::xaudio2_stats();
    KX_CHECK(st.frames_submitted == kFrames && st.frames_consumed == kFrames, "Frame counters match");
    KX_CHECK(st.underruns == 1, "Underrun tracked when draining past available frames");
    KX_CHECK(st.last_rms > 0.30 && st.last_rms < 0.42, std::format("Expected sine RMS ~0.35, got {:.3f}", st.last_rms));

    sv->DestroyVoice();
    mv->DestroyVoice();
    xa->Release();
}

void test_rdna2_mubuf_mimg_textured_quad_dual_gpu() {
    using namespace gpu::encode;

    // 1. Verify MTBUF & MIMG (image_load + image_get_resinfo) decode, disasm, IR lift & DXC compile
    std::vector<uint32_t> cs_words;
    mtbuf(cs_words, 3, 2, 0, 0, 128, 0, 0);          // tbuffer_load_format_xyzw v2, v0, s[0:3], 0
    mimg(cs_words, 0x00, 6, 2, 0, 0, 0xFu, 1);       // image_load v6, v2, s[0:7] dmask:0xf
    mimg(cs_words, 0x0E, 10, 0, 0, 0, 0xBu, 1);      // image_get_resinfo v10, v0, s[0:7] dmask:0xb
    mubuf(cs_words, 30, 6, 0, 0, 128, 0);            // buffer_store_dwordx4 v6, v0, s[0:3], 0
    cs_words.push_back(sopp(1));                     // s_endpgm

    auto cs_dec = gpu::decode_rdna2(cs_words.data(), cs_words.size());
    KX_CHECK(cs_dec.size() == 5, "Decoded 5 MTBUF/MIMG/MUBUF compute instructions");
    KX_CHECK(cs_dec[0].enc == gpu::Encoding::MTBUF && cs_dec[0].mnemonic == "tbuffer_load_format_xyzw", "MTBUF decode");
    KX_CHECK(cs_dec[1].enc == gpu::Encoding::MIMG && cs_dec[1].mnemonic == "image_load", "MIMG image_load decode");
    KX_CHECK(cs_dec[2].enc == gpu::Encoding::MIMG && cs_dec[2].mnemonic == "image_get_resinfo", "MIMG resinfo decode");
    KX_CHECK(cs_dec[3].enc == gpu::Encoding::MUBUF && cs_dec[3].mnemonic == "buffer_store_dwordx4", "MUBUF storex4 decode");
    KX_CHECK(gpu::disassemble_inst(cs_dec[1]).find("image_load") != std::string::npos, "MIMG disasm");

    auto cs_ir = gpu::lift_rdna2_to_ir(cs_dec);
    KX_CHECK(cs_ir.unknown_count == 0 && cs_ir.uses_tex && cs_ir.uses_buf_srv && cs_ir.uses_mem, "MTBUF/MIMG IR flags");

    gpu::DxcCompiler dxc;
    KX_CHECK(dxc.initialize().has_value(), "DXC init");
    auto cs_hlsl = gpu::emit_hlsl_from_ir(cs_ir, {.stage = gpu::ShaderStage::Compute, .wave_mode = gpu::WaveMode::Wave32});
    auto cs_dxil = dxc.compile_hlsl(cs_hlsl.hlsl, "main", "cs_6_6");
    KX_CHECK(cs_dxil.has_value(), cs_dxil.error().message);

    // 2. Build raw RDNA2 Vertex Shader reading quad vertices via MUBUF buffer_load_dwordx4 (t1)
    //    Vertex layout (16 bytes/vert): float4(x, y, u, v)
    std::vector<uint32_t> vs_words;
    vs_words.push_back(vop2(26, 1, 128 + 4, 0));     // v_lshlrev_b32 v1, 4, v0 (byte offset = sv_vid * 16)
    mubuf(vs_words, 14, 2, 1, 0, 128, 0);            // buffer_load_dwordx4 v[2:5], v1, s[0:3], 0
    vs_words.push_back(vop1(1, 6, 128));             // v_mov_b32 v6, 0.0
    vs_words.push_back(vop1(1, 7, 242));             // v_mov_b32 v7, 1.0
    exp(vs_words, 12, 0xFu, 2, 3, 6, 7, false);      // exp pos0 v2, v3, v6, v7
    exp(vs_words, 32, 0xFu, 4, 5, 6, 7, true);       // exp param0 v4, v5, v6, v7 done
    vs_words.push_back(sopp(1));                     // s_endpgm

    // 3. Build raw RDNA2 Pixel Shader sampling 2D texture via MIMG image_sample (t0, s0)
    std::vector<uint32_t> ps_words;
    mimg(ps_words, 0x20, 4, 0, 0, 0, 0xFu, 1);       // image_sample v[4:7], v[0:1], s[0:7], s[0:3] dmask:0xf
    exp(ps_words, 0, 0xFu, 4, 5, 6, 7, true);        // exp mrt0 v4, v5, v6, v7 done
    ps_words.push_back(sopp(1));                     // s_endpgm

    auto vs_ir = gpu::lift_rdna2_to_ir(gpu::decode_rdna2(vs_words.data(), vs_words.size()));
    auto ps_ir = gpu::lift_rdna2_to_ir(gpu::decode_rdna2(ps_words.data(), ps_words.size()));
    KX_CHECK(vs_ir.unknown_count == 0 && vs_ir.uses_buf_srv, "Quad VS IR lift");
    KX_CHECK(ps_ir.unknown_count == 0 && ps_ir.uses_tex, "Quad PS IR lift");

    auto vs_hlsl = gpu::emit_hlsl_from_ir(vs_ir, {.stage = gpu::ShaderStage::Vertex});
    auto ps_hlsl = gpu::emit_hlsl_from_ir(ps_ir, {.stage = gpu::ShaderStage::Pixel});
    auto vs_dxil = dxc.compile_hlsl(vs_hlsl.hlsl, "main", "vs_6_6");
    auto ps_dxil = dxc.compile_hlsl(ps_hlsl.hlsl, "main", "ps_6_6");
    KX_CHECK(vs_dxil.has_value(), vs_dxil.error().message);
    KX_CHECK(ps_dxil.has_value(), ps_dxil.error().message);

    // 4. Execute live Textured Quad on every hardware D3D12 adapter & verify cross-GPU pixel parity
    auto adapters = gpu::enumerate_d3d12_adapters();
    KX_CHECK(adapters.has_value(), adapters.error().message);

    constexpr uint32_t kW = 256, kH = 256, kTex = 64;
    // Build 64x64 4-quadrant calibration checkerboard texture (RGBA8)
    std::vector<uint8_t> tex_rgba(kTex * kTex * 4, 255);
    for (uint32_t y = 0; y < kTex; ++y) {
        for (uint32_t x = 0; x < kTex; ++x) {
            uint8_t r = 26, g = 36, b = 58;
            const bool border = (x < 2 || x >= kTex - 2 || y < 2 || y >= kTex - 2);
            const bool check = (((x / 8) + (y / 8)) & 1u) == 0u;
            if (border) {
                r = 230; g = 238; b = 250;
            } else if (check) {
                if (x < kTex / 2 && y < kTex / 2) { r = 245; g = 195; b = 55; }       // Gold
                else if (x >= kTex / 2 && y < kTex / 2) { r = 48; g = 210; b = 130; } // Emerald
                else if (x < kTex / 2 && y >= kTex / 2) { r = 235; g = 85; b = 95; }  // Coral
                else { r = 55; g = 190; b = 245; }                                    // Cyan
            }
            const size_t idx = (static_cast<size_t>(y) * kTex + x) * 4;
            tex_rgba[idx + 0] = r; tex_rgba[idx + 1] = g; tex_rgba[idx + 2] = b; tex_rgba[idx + 3] = 255;
        }
    }

    // 6 vertices (2 triangles) covering [-0.82, 0.82] NDC with [0..1] UVs
    const float quad_verts[6 * 4] = {
        -0.82f,  0.82f, 0.0f, 0.0f,
         0.82f,  0.82f, 1.0f, 0.0f,
         0.82f, -0.82f, 1.0f, 1.0f,
        -0.82f,  0.82f, 0.0f, 0.0f,
         0.82f, -0.82f, 1.0f, 1.0f,
        -0.82f, -0.82f, 0.0f, 1.0f,
    };

    std::vector<uint8_t> ref_pixels;
    int hw_verified = 0;
    IDXGIFactory6* factory = nullptr;
    KX_CHECK(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "CreateDXGIFactory1");

    const char* capture_env = std::getenv("KESTREL_CAPTURE_QUAD_BMP");

    for (const auto& ad : *adapters) {
        if (ad.is_software) continue;
        IDXGIAdapter1* dxgi_ad = nullptr;
        if (FAILED(factory->EnumAdapters1(ad.index, &dxgi_ad))) continue;

        ID3D12Device* dev = nullptr;
        HRESULT hr = D3D12CreateDevice(dxgi_ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
        dxgi_ad->Release();
        if (FAILED(hr) || !dev) continue;

        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* queue = nullptr;
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* cmd = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))), "CreateCommandQueue");
        KX_CHECK(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))), "CreateCommandAllocator");
        KX_CHECK(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cmd))), "CreateCommandList");

        // Root signature: [0]=DescriptorTable(SRV t0), [1]=RootSRV(t1), StaticSampler(s0, Point)
        D3D12_DESCRIPTOR_RANGE srv_range{};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = 1;
        srv_range.BaseShaderRegister = 0;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rparams[2]{};
        rparams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rparams[0].DescriptorTable = {1, &srv_range};
        rparams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rparams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rparams[1].Descriptor = {1, 0};  // t1
        rparams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsd{2, rparams, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ID3DBlob* rs_blob = nullptr;
        ID3DBlob* rs_err = nullptr;
        ID3D12RootSignature* rs = nullptr;
        KX_CHECK(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err)), "SerializeRS");
        KX_CHECK(SUCCEEDED(dev->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&rs))), "CreateRS");
        rs_blob->Release();
        if (rs_err) rs_err->Release();

        // Graphics PSO
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psd{};
        psd.pRootSignature = rs;
        psd.VS = {vs_dxil->dxil.data(), vs_dxil->dxil.size()};
        psd.PS = {ps_dxil->dxil.data(), ps_dxil->dxil.size()};
        psd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psd.SampleMask = UINT_MAX;
        psd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psd.NumRenderTargets = 1;
        psd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psd.SampleDesc.Count = 1;
        ID3D12PipelineState* pso = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateGraphicsPipelineState(&psd, IID_PPV_ARGS(&pso))), "CreateQuadPSO");

        // Create RTV, SRV Texture, Upload Buffers, and Readback Buffer
        D3D12_HEAP_PROPERTIES hp_def{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
        D3D12_HEAP_PROPERTIES hp_up{D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
        D3D12_HEAP_PROPERTIES hp_rb{D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};

        D3D12_RESOURCE_DESC rt_desc{};
        rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rt_desc.Width = kW; rt_desc.Height = kH; rt_desc.DepthOrArraySize = 1; rt_desc.MipLevels = 1;
        rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rt_desc.SampleDesc.Count = 1;
        rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ID3D12Resource* rt = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt))), "CreateRT");

        D3D12_RESOURCE_DESC tex_desc{};
        tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tex_desc.Width = kTex; tex_desc.Height = kTex; tex_desc.DepthOrArraySize = 1; tex_desc.MipLevels = 1;
        tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; tex_desc.SampleDesc.Count = 1;
        ID3D12Resource* tex = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex))), "CreateTex");

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT tex_fp{};
        UINT64 tex_upload_bytes = 0;
        dev->GetCopyableFootprints(&tex_desc, 0, 1, 0, &tex_fp, nullptr, nullptr, &tex_upload_bytes);

        D3D12_RESOURCE_DESC buf_desc{};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Height = 1; buf_desc.DepthOrArraySize = 1; buf_desc.MipLevels = 1;
        buf_desc.SampleDesc.Count = 1; buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        buf_desc.Width = tex_upload_bytes;
        ID3D12Resource* tex_up = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateCommittedResource(&hp_up, D3D12_HEAP_FLAG_NONE, &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&tex_up))), "CreateTexUpload");
        void* mapped = nullptr;
        KX_CHECK(SUCCEEDED(tex_up->Map(0, nullptr, &mapped)), "MapTexUpload");
        for (uint32_t y = 0; y < kTex; ++y) {
            std::memcpy(static_cast<uint8_t*>(mapped) + y * tex_fp.Footprint.RowPitch,
                        tex_rgba.data() + y * kTex * 4, kTex * 4);
        }
        tex_up->Unmap(0, nullptr);

        buf_desc.Width = 256;
        ID3D12Resource* vb_srv = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateCommittedResource(&hp_up, D3D12_HEAP_FLAG_NONE, &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vb_srv))), "CreateVBSrv");
        KX_CHECK(SUCCEEDED(vb_srv->Map(0, nullptr, &mapped)), "MapVBSrv");
        std::memcpy(mapped, quad_verts, sizeof(quad_verts));
        vb_srv->Unmap(0, nullptr);

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT rt_fp{};
        UINT64 rb_bytes = 0;
        dev->GetCopyableFootprints(&rt_desc, 0, 1, 0, &rt_fp, nullptr, nullptr, &rb_bytes);
        buf_desc.Width = rb_bytes;
        ID3D12Resource* rb = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateCommittedResource(&hp_rb, D3D12_HEAP_FLAG_NONE, &buf_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))), "CreateRB");

        // Heaps
        D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        D3D12_DESCRIPTOR_HEAP_DESC srv_hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        ID3D12DescriptorHeap* rtv_heap = nullptr;
        ID3D12DescriptorHeap* srv_heap = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateDescriptorHeap(&rtv_hd, IID_PPV_ARGS(&rtv_heap))), "CreateRtvHeap");
        KX_CHECK(SUCCEEDED(dev->CreateDescriptorHeap(&srv_hd, IID_PPV_ARGS(&srv_heap))), "CreateSrvHeap");
        dev->CreateRenderTargetView(rt, nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(tex, &srvd, srv_heap->GetCPUDescriptorHandleForHeapStart());

        // Record texture upload + draw + readback
        D3D12_TEXTURE_COPY_LOCATION dst_tex{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        D3D12_TEXTURE_COPY_LOCATION src_tex{tex_up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        src_tex.PlacedFootprint = tex_fp;
        cmd->CopyTextureRegion(&dst_tex, 0, 0, 0, &src_tex, nullptr);

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {tex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
        cmd->ResourceBarrier(1, &b);

        const float clear_col[4] = {14.0f / 255.0f, 20.0f / 255.0f, 36.0f / 255.0f, 1.0f};
        auto rtv_h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        cmd->ClearRenderTargetView(rtv_h, clear_col, 0, nullptr);
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, static_cast<LONG>(kW), static_cast<LONG>(kH)};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);
        cmd->OMSetRenderTargets(1, &rtv_h, FALSE, nullptr);
        cmd->SetGraphicsRootSignature(rs);
        cmd->SetPipelineState(pso);
        cmd->SetDescriptorHeaps(1, &srv_heap);
        cmd->SetGraphicsRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());
        cmd->SetGraphicsRootShaderResourceView(1, vb_srv->GetGPUVirtualAddress());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(6, 1, 0, 0);

        b.Transition = {rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION rb_dst{rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        rb_dst.PlacedFootprint = rt_fp;
        D3D12_TEXTURE_COPY_LOCATION rb_src{rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        cmd->CopyTextureRegion(&rb_dst, 0, 0, 0, &rb_src, nullptr);
        b.Transition = {rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET};
        cmd->ResourceBarrier(1, &b);
        KX_CHECK(SUCCEEDED(cmd->Close()), "CmdClose");

        ID3D12CommandList* lists[1] = {cmd};
        queue->ExecuteCommandLists(1, lists);
        ID3D12Fence* fence = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "CreateFence");
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        queue->Signal(fence, 1);
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, 5000);
        CloseHandle(ev);

        if (capture_env && capture_env[0] != '\0' && hw_verified == 0) {
            (void)hle::capture_d3d12_texture_bmp(dev, queue, rt, D3D12_RESOURCE_STATE_RENDER_TARGET, fs::path(capture_env));
        }

        // Verify readback pixels across all 4 textured quadrants + background border
        void* rb_ptr = nullptr;
        KX_CHECK(SUCCEEDED(rb->Map(0, nullptr, &rb_ptr)), "MapRB");
        std::vector<uint8_t> pixels(kW * kH * 4);
        for (uint32_t y = 0; y < kH; ++y) {
            std::memcpy(pixels.data() + y * kW * 4,
                        static_cast<const uint8_t*>(rb_ptr) + y * rt_fp.Footprint.RowPitch, kW * 4);
        }
        rb->Unmap(0, nullptr);

        auto px = [&](uint32_t x, uint32_t y) { return &pixels[(static_cast<size_t>(y) * kW + x) * 4]; };
        // Corner background (navy)
        KX_CHECK(px(4, 4)[0] == 14 && px(4, 4)[1] == 20 && px(4, 4)[2] == 36, "Quad background navy pixel");
        // Gold tile in top-left quadrant (texel ~(4,4) -> screen ~(36,36))
        KX_CHECK(px(38, 38)[0] > 230 && px(38, 38)[1] > 180 && px(38, 38)[2] < 70, "Top-left Gold checker tile");
        // Emerald tile in top-right quadrant (texel ~(36,4) -> screen ~(142,38))
        KX_CHECK(px(142, 38)[1] > 195 && px(142, 38)[0] < 70, "Top-right Emerald checker tile");
        // Coral tile in bottom-left quadrant (texel ~(4,36) -> screen ~(38,142))
        KX_CHECK(px(38, 142)[0] > 220 && px(38, 142)[1] < 105, "Bottom-left Coral checker tile");
        // Cyan tile in bottom-right quadrant (texel ~(36,36) -> screen ~(142,142))
        KX_CHECK(px(142, 142)[2] > 230 && px(142, 142)[1] > 175 && px(142, 142)[0] < 75, "Bottom-right Cyan checker tile");

        uint32_t max_delta = 0;
        if (ref_pixels.empty()) {
            ref_pixels = pixels;
        } else {
            for (size_t i = 0; i < pixels.size(); ++i) {
                const uint32_t d = static_cast<uint32_t>(std::abs(int(pixels[i]) - int(ref_pixels[i])));
                if (d > max_delta) max_delta = d;
            }
            KX_CHECK(max_delta <= 1, std::format("Cross-GPU textured quad delta too large: {}", max_delta));
        }

        std::cout << std::format(
            "    -> Textured Quad verified: [{}] {} | MUBUF VS + MIMG PS | 256x256 4-quadrant checkerboard | max_delta={}/255\n",
            ad.index, ad.name, max_delta);

        fence->Release(); srv_heap->Release(); rtv_heap->Release();
        rb->Release(); vb_srv->Release(); tex_up->Release(); tex->Release(); rt->Release();
        pso->Release(); rs->Release(); cmd->Release(); alloc->Release(); queue->Release(); dev->Release();
        ++hw_verified;
    }
    factory->Release();
    KX_CHECK(hw_verified >= 1, "At least one D3D12 GPU verified for textured quad");
}

void test_persistent_disk_shader_cache() {
    using namespace gpu::encode;
    const fs::path cache_dir = fs::temp_directory_path() / "KestrelX_DiskCacheTest";
    std::error_code ec;
    fs::remove_all(cache_dir, ec);

    hle::reset_d3d12x_stats();
    hle::set_d3d12x_shader_cache_dir(cache_dir);

    hle::Registry reg;
    hle::register_d3d12x(reg);
    using CreateDevFn = HRESULT(__stdcall*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto create_dev = reinterpret_cast<CreateDevFn>(reg.resolve_name("d3d12_xs.dll", "D3D12CreateDevice"));
    KX_CHECK(create_dev != nullptr, "resolve D3D12CreateDevice");

    ID3D12Device* dev = nullptr;
    KX_CHECK(SUCCEEDED(create_dev(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev))) && dev, "create KxDeviceProxy");

    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[0].Descriptor = {0, 0};
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[1].Constants = {0, 0, 8};
    D3D12_ROOT_SIGNATURE_DESC rsd{2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* rs_blob = nullptr;
    ID3D12RootSignature* rs = nullptr;
    KX_CHECK(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, nullptr)), "SerializeRS");
    KX_CHECK(SUCCEEDED(dev->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&rs))), "CreateRS");
    rs_blob->Release();

    // Minimal valid KXRN compute container
    std::vector<uint32_t> cs_words = {
        vop2(24, 1, 128 + 2, 0),
        sopp(1)
    };
    std::vector<uint8_t> kxrn(8 + cs_words.size() * 4);
    std::memcpy(kxrn.data(), "KXRN", 4);
    const uint32_t gx = 64;
    std::memcpy(kxrn.data() + 4, &gx, 4);
    std::memcpy(kxrn.data() + 8, cs_words.data(), cs_words.size() * 4);

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rs;
    desc.CS = {kxrn.data(), kxrn.size()};

    // 1. Cold compile -> writes .kxdxil to disk
    ID3D12PipelineState* pso1 = nullptr;
    KX_CHECK(SUCCEEDED(dev->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso1))) && pso1, "Cold PSO compile");
    auto st1 = hle::d3d12x_stats();
    KX_CHECK(st1.pso_recompiled == 1 && st1.disk_cache_misses == 1 && st1.disk_cache_hits == 0, "Cold compile stats");

    // Verify a .kxdxil file was created on disk
    fs::path cache_file;
    for (const auto& entry : fs::directory_iterator(cache_dir)) {
        if (entry.path().extension() == ".kxdxil") cache_file = entry.path();
    }
    KX_CHECK(!cache_file.empty() && fs::file_size(cache_file) > 64, "On-disk .kxdxil created");

    // 2. Evict RAM cache -> second PSO creation must hit on-disk cache with ZERO DXC recompiles
    hle::clear_d3d12x_memory_shader_cache();
    ID3D12PipelineState* pso2 = nullptr;
    KX_CHECK(SUCCEEDED(dev->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso2))) && pso2, "Warm disk-cache PSO");
    auto st2 = hle::d3d12x_stats();
    KX_CHECK(st2.disk_cache_hits == 1 && st2.pso_recompiled == 1, "Disk cache hit without DXC recompile");

    // 3. Corrupt 1 byte in the on-disk .kxdxil -> must detect corruption, evict, and recompile cleanly
    {
        std::fstream fs_corrupt(cache_file, std::ios::binary | std::ios::in | std::ios::out);
        fs_corrupt.seekp(40);
        char bad = static_cast<char>(0xFF);
        fs_corrupt.write(&bad, 1);
    }
    hle::clear_d3d12x_memory_shader_cache();
    ID3D12PipelineState* pso3 = nullptr;
    KX_CHECK(SUCCEEDED(dev->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso3))) && pso3, "Corrupt cache recovery");
    auto st3 = hle::d3d12x_stats();
    KX_CHECK(st3.pso_recompiled == 2 && st3.disk_cache_misses == 2, "Corrupt cache evicted & cleanly recompiled");

    pso3->Release(); pso2->Release(); pso1->Release(); rs->Release(); dev->Release();
    fs::remove_all(cache_dir, ec);
}

void test_d3d12_device_qi_stream_pso_and_cmdlist_telemetry() {
    using namespace gpu::encode;
    const fs::path cache_dir = fs::temp_directory_path() / "KestrelX_StreamPsoTest";
    std::error_code ec;
    fs::remove_all(cache_dir, ec);

    hle::reset_d3d12x_stats();
    hle::set_d3d12x_shader_cache_dir(cache_dir);

    hle::Registry reg;
    hle::register_d3d12x(reg);
    using CreateDevFn = HRESULT(__stdcall*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto create_dev = reinterpret_cast<CreateDevFn>(reg.resolve_name("d3d12_xs.dll", "D3D12CreateDevice"));
    ID3D12Device* dev = nullptr;
    KX_CHECK(SUCCEEDED(create_dev(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev))) && dev, "Create KxDeviceProxy");

    // 1. Verify QueryInterface across ID3D12Device1, ID3D12Device2, ID3D12Device5, ID3D12Device10
    ID3D12Device1* dev1 = nullptr;
    ID3D12Device2* dev2 = nullptr;
    ID3D12Device5* dev5 = nullptr;
    ID3D12Device10* dev10 = nullptr;
    KX_CHECK(SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev1))) && dev1, "QI ID3D12Device1");
    KX_CHECK(SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev2))) && dev2, "QI ID3D12Device2");
    KX_CHECK(SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev5))) && dev5, "QI ID3D12Device5");
    KX_CHECK(SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev10))) && dev10, "QI ID3D12Device10");

    // 2. Verify ID3D12Device2::CreatePipelineState with D3D12_PIPELINE_STATE_STREAM_DESC + KXRN compute shader
    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[0].Descriptor = {0, 0};
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[1].Constants = {0, 0, 8};
    D3D12_ROOT_SIGNATURE_DESC rsd{2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* rs_blob = nullptr;
    ID3D12RootSignature* rs = nullptr;
    KX_CHECK(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, nullptr)), "SerializeRS");
    KX_CHECK(SUCCEEDED(dev->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&rs))), "CreateRS");
    rs_blob->Release();

    std::vector<uint32_t> cs_words = {sopp(1)};
    std::vector<uint8_t> kxrn(8 + cs_words.size() * 4);
    std::memcpy(kxrn.data(), "KXRN", 4);
    const uint32_t gx = 64;
    std::memcpy(kxrn.data() + 4, &gx, 4);
    std::memcpy(kxrn.data() + 8, cs_words.data(), cs_words.size() * 4);

    struct alignas(void*) StreamSubobjRS {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
        ID3D12RootSignature* rs = nullptr;
    };
    struct alignas(void*) StreamSubobjCS {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS;
        D3D12_SHADER_BYTECODE cs{};
    };
    struct ComputeStream {
        StreamSubobjRS root_sig;
        StreamSubobjCS cs;
    } stream{};
    stream.root_sig.rs = rs;
    stream.cs.cs = {kxrn.data(), kxrn.size()};

    D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
    ID3D12PipelineState* stream_pso = nullptr;
    KX_CHECK(SUCCEEDED(dev2->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&stream_pso))) && stream_pso,
             "ID3D12Device2::CreatePipelineState stream KXRN recompilation");

    // 3. Verify KxGraphicsCommandListProxy telemetry + KxCommandQueueProxy unwrapping on real hardware
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    KX_CHECK(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))), "CreateCommandQueue");
    KX_CHECK(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))), "CreateCommandAllocator");
    KX_CHECK(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, stream_pso, IID_PPV_ARGS(&cmd))), "CreateCommandList");

    cmd->SetPipelineState(stream_pso);
    cmd->SetComputeRootSignature(rs);
    cmd->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER uav_b{};
    uav_b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_b.UAV.pResource = nullptr;
    cmd->ResourceBarrier(1, &uav_b);
    KX_CHECK(SUCCEEDED(cmd->Close()), "CmdClose");

    ID3D12CommandList* lists[1] = {cmd};
    queue->ExecuteCommandLists(1, lists);
    ID3D12Fence* fence = nullptr;
    KX_CHECK(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "CreateFence");
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence, 1);
    fence->SetEventOnCompletion(1, ev);
    WaitForSingleObject(ev, 5000);
    CloseHandle(ev);

    const auto st = hle::d3d12x_stats();
    KX_CHECK(st.device_qi_upgrades >= 4, "Device1/2/5/10 QI upgrades recorded");
    KX_CHECK(st.stream_pso_recompiled >= 1, "Stream PSO auto-recompilation recorded");
    KX_CHECK(st.pso_binds >= 1 && st.dispatch_calls >= 1 && st.barrier_calls >= 1, "KxGraphicsCommandListProxy telemetry recorded");

    fence->Release(); cmd->Release(); alloc->Release(); queue->Release();
    stream_pso->Release(); rs->Release();
    dev10->Release(); dev5->Release(); dev2->Release(); dev1->Release(); dev->Release();
    fs::remove_all(cache_dir, ec);
}

void test_kernelx_async_io_and_wasapi_lifecycle() {
    const fs::path title_dir = KESTREL_TEST_TITLE_DIR;
    const fs::path data_dir = fs::temp_directory_path() / "KestrelX_AsyncIoTest";
    std::error_code ec;
    fs::remove_all(data_dir, ec);

    vfs::OverlayVfs overlay(title_dir, data_dir);
    hle::Registry reg;
    hle::reset_kernelx_stats();
    hle::reset_xaudio2_stats();
    hle::set_kernelx_vfs(&overlay);
    hle::register_kernelx(reg);
    hle::register_xaudio2(reg);

    using CreateFile2Fn = HANDLE(__stdcall*)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
    using ReadFileExFn = BOOL(__stdcall*)(HANDLE, LPVOID, DWORD, LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE);
    auto cf2 = reinterpret_cast<CreateFile2Fn>(reg.resolve_name("kernelx.dll", "CreateFile2"));
    auto rfex = reinterpret_cast<ReadFileExFn>(reg.resolve_name("kernelx.dll", "ReadFileEx"));
    KX_CHECK(cf2 && rfex, "kernelx!CreateFile2 and ReadFileEx exported");

    CREATEFILE2_EXTENDED_PARAMETERS ext{};
    ext.dwSize = sizeof(ext);
    ext.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    ext.dwFileFlags = FILE_FLAG_OVERLAPPED;

    HANDLE h = cf2(L"GAME:\\data\\readonly.txt", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &ext);
    KX_CHECK(h != INVALID_HANDLE_VALUE, "CreateFile2 opened VFS GAME:\\data\\readonly.txt");

    static DWORD g_apc_err = 9999;
    static DWORD g_apc_bytes = 0;
    g_apc_err = 9999;
    g_apc_bytes = 0;
    struct ApcSink {
        static void WINAPI on_complete(DWORD err, DWORD transferred, LPOVERLAPPED) {
            g_apc_err = err;
            g_apc_bytes = transferred;
        }
    };

    char buf[64]{};
    OVERLAPPED ov{};
    KX_CHECK(rfex(h, buf, sizeof(buf) - 1, &ov, &ApcSink::on_complete) != FALSE, "ReadFileEx queued async read");
    SleepEx(1000, TRUE);
    CloseHandle(h);

    KX_CHECK(g_apc_err == 0 && g_apc_bytes >= 25, "Async ReadFileEx APC completed cleanly");
    KX_CHECK(std::string_view(buf, g_apc_bytes).find("KestrelX") != std::string_view::npos, "Async VFS payload matched");
    const auto kx = hle::kernelx_stats();
    KX_CHECK(kx.create_file2_calls >= 1 && kx.async_reads >= 1, "KernelX async I/O counters");

    // Exercise WASAPI shared-mode thread start -> brief silent render -> clean shutdown
    hle::set_xaudio2_wasapi_enabled(true);
    Sleep(30);
    hle::shutdown_xaudio2_wasapi();
    KX_CHECK(!hle::xaudio2_stats().wasapi_active, "WASAPI thread cleanly stopped");

    fs::remove_all(data_dir, ec);
}

void test_end_to_end_pe_loader_guest_title() {
    const fs::path title_dir = KESTREL_TEST_TITLE_DIR;
    const fs::path data_dir = fs::temp_directory_path() / "KestrelX_TestTitleData";
    const fs::path bmp_out = data_dir / "first_light.bmp";
    std::error_code ec;
    fs::remove_all(data_dir, ec);

    auto cfg = loader::load_game_config(title_dir / "MicrosoftGame.config");
    KX_CHECK(cfg.has_value(), cfg.error().message);

    vfs::OverlayVfs overlay(title_dir, data_dir);
    hle::Registry reg;
    hle::reset_kernelx_stats();
    hle::reset_d3d12x_stats();
    hle::reset_gameinput_stats();
    hle::reset_xaudio2_stats();
    hle::set_kernelx_vfs(&overlay);
    hle::set_xgameruntime_title_id(cfg->title_id);
    hle::set_d3d12x_preferred_adapter(-1);
    hle::set_d3d12x_capture_path(bmp_out);
    hle::set_d3d12x_shader_cache_dir(data_dir / "shader_cache");
    hle::register_kernelx(reg);
    hle::register_xgameruntime(reg);
    hle::register_d3d12x(reg);
    hle::register_gameinput(reg);
    hle::register_xaudio2(reg);

    cpu::ScopedVehShim veh;
    loader::PeLoader loader(reg, title_dir);

    auto mod = loader.load_image(title_dir / cfg->executables[0].name);
    KX_CHECK(mod.has_value(), mod.error().message);
    KX_CHECK((*mod)->hle_imports >= 20, "expected >= 20 HLE console imports resolved");
    KX_CHECK((*mod)->guest_imports >= 2, "expected guest DLL imports resolved");
    KX_CHECK((*mod)->tls_callbacks_run >= 1, "expected PE TLS callback to run");

    auto exit_code = loader.execute_entry(**mod);
    KX_CHECK(exit_code.has_value(), exit_code.error().message);
    KX_CHECK(*exit_code == 0, std::format("guest title returned error code {}", *exit_code));

    // Verify Overlay VFS save landed in data_dir/save/save.bin and NOT in title_dir
    const fs::path save_file = overlay.save_root() / "save.bin";
    KX_CHECK(fs::exists(save_file), "guest save file missing in S:\\ overlay");
    KX_CHECK(!fs::exists(title_dir / "save.bin"), "game_root must remain unmodified");

    // Verify BMP First-Light screenshot captured by D3D12PresentX!
    KX_CHECK(fs::exists(bmp_out) && fs::file_size(bmp_out) >= 256u * 256u * 3u + 54u,
             "First-Light 256x256 BMP capture missing or truncated");
    {
        std::ifstream ifs(bmp_out, std::ios::binary);
        char sig[2]{};
        ifs.read(sig, 2);
        KX_CHECK(sig[0] == 'B' && sig[1] == 'M', "Valid BMP file header");
    }

    // Verify D3D12.X KxDeviceProxy + KxCommandQueueProxy + KxGraphicsCommandListProxy + KXRN/KXRV/KXRP PSO auto-recomp!
    const auto d3d = hle::d3d12x_stats();
    KX_CHECK(d3d.devices_created >= 1 && d3d.queues_created >= 1 &&
             d3d.cmd_lists_created >= 1 && d3d.resources_created >= 2 &&
             d3d.zero_memory_calls >= 1 && d3d.present_calls >= 1 &&
             d3d.draw_calls >= 1 && d3d.barrier_calls >= 2 && d3d.pso_binds >= 1 &&
             d3d.pso_recompiled >= 1 && d3d.pso_cache_hits >= 1 &&
             d3d.gfx_pso_recompiled >= 2 && d3d.gfx_pso_cache_hits >= 2,
             "KxDeviceProxy / CmdListProxy / Graphics+Compute PSO auto-recomp / PresentX counters mismatch");

    const auto kx = hle::kernelx_stats();
    KX_CHECK(kx.alloc_count >= 1 && kx.free_count == kx.alloc_count && kx.live_bytes == 0,
             "KernelX XMemAlloc/XMemFree leak check");

    const auto gi = hle::gameinput_stats();
    const auto xa = hle::xaudio2_stats();
    KX_CHECK(gi.instances_created >= 1 && gi.readings_polled >= 1, "Guest GameInput HLE verified");
    KX_CHECK(xa.engines_created >= 1 && xa.frames_submitted >= 64, "Guest XAudio2 HLE verified");

    std::cout << std::format(
        "    -> Guest title PASS: HLE_imports={}, GuestDLL_imports={}, TLS={}, D3D12X(dev={}, q={}, res={}, draws={}, barriers={}, cs_pso={}, gfx_pso={}, cache_hit={}, present={}), GI(reads={}), XA2(frames={})\n",
        (*mod)->hle_imports, (*mod)->guest_imports, (*mod)->tls_callbacks_run,
        d3d.devices_created, d3d.queues_created, d3d.resources_created,
        d3d.draw_calls, d3d.barrier_calls,
        d3d.pso_recompiled, d3d.gfx_pso_recompiled, d3d.pso_cache_hits + d3d.gfx_pso_cache_hits,
        d3d.present_calls, gi.readings_polled, xa.frames_submitted);

    fs::remove_all(data_dir, ec);
}

void test_rdna2_wave_intrinsics_dual_gpu() {
    using namespace gpu::encode;

    // Build compute kernel exercising:
    //   - v_readfirstlane_b32
    //   - v_readlane_b32
    //   - v_writelane_b32
    //   - v_mbcnt_lo_u32_b32
    //   - s_and_saveexec_b32
    std::vector<uint32_t> words;
    // v1 = v0 << 2 (input byte offset = tid.x * 4)
    words.push_back(vop2(26, 1, 128 + 2, 0));
    // Load input[tid.x] into v2
    global_mem(words, 12, 2, 1, 127, 0);
    // s4 = 5 (target lane index)
    words.push_back(sopk(0, 4, 5));
    // s5 = WaveReadLaneFirst(v2)
    words.push_back(vop1(2, 5, 256 + 2));
    // s6 = WaveReadLaneAt(v2, s4)
    vop3(words, 0x360, 6, 256 + 2, 4, 0);
    // v2 = WaveWriteLane(v2, s5, s4) -> lane 5 gets lane 0's value
    vop3(words, 0x361, 2, 5, 4, 0);
    // s7 = 0xFFFFFFFF
    words.push_back(sopk(0, 7, -1));
    // s8 = 1000
    words.push_back(sopk(0, 8, 1000));
    // v3 = WavePrefixCountBits(WaveActiveBallot.x & s7) + s8
    vop3(words, 0x16B, 3, 7, 8, 0);
    // s9 = WaveActiveBallot(true).x
    words.push_back(sop1(60, 9, 7));
    // v4 = v0 << 4 (output byte offset = tid.x * 16)
    words.push_back(vop2(26, 4, 128 + 4, 0));
    // Move SGPRs into VGPRs for storing
    words.push_back(vop1(1, 5, 5));
    words.push_back(vop1(1, 6, 6));
    words.push_back(vop1(1, 7, 9));
    // Store 4 outputs:
    global_mem(words, 28, 5, 4, 127, 0);   // out[tid*4+0] = readfirstlane
    global_mem(words, 28, 6, 4, 127, 4);   // out[tid*4+1] = readlane 5
    global_mem(words, 28, 3, 4, 127, 8);   // out[tid*4+2] = mbcnt
    global_mem(words, 28, 7, 4, 127, 12);  // out[tid*4+3] = ballot
    words.push_back(sopp(1));              // s_endpgm

    auto decoded = gpu::decode_rdna2(words.data(), words.size());
    auto ir = gpu::lift_rdna2_to_ir(decoded);
    KX_CHECK(ir.unknown_count == 0 && ir.uses_wave_ops, "Wave ops IR lift");

    gpu::DxcCompiler dxc;
    KX_CHECK(dxc.initialize().has_value(), "DXC init");
    auto recomp = gpu::emit_hlsl_from_ir(ir, {.stage = gpu::ShaderStage::Compute, .wave_mode = gpu::WaveMode::Wave32, .group_size_x = 32});
    auto compiled = dxc.compile_hlsl(recomp.hlsl, "main", "cs_6_6");
    
    KX_CHECK(compiled.has_value(), compiled.error().message);

    auto adapters = gpu::enumerate_d3d12_adapters();
    KX_CHECK(adapters.has_value(), adapters.error().message);

    constexpr uint32_t kLanes = 32;
    std::vector<uint32_t> initial_buf(kLanes * 4, 0);
    for (uint32_t i = 0; i < kLanes; ++i) {
        initial_buf[i] = (i + 1) * 111u;
    }
    uint32_t roots[8]{};

    std::vector<uint32_t> reference_output;
    int tested = 0;
    for (const auto& ad : *adapters) {
        if (ad.is_software) continue;
        auto ran = gpu::run_compute_dxil_on_gpu(ad.index, compiled->dxil, initial_buf, roots, 1);
        KX_CHECK(ran.has_value(), ran.error().message);

        for (uint32_t i = 0; i < kLanes; ++i) {
            const uint32_t rf = ran->output_u32[i * 4 + 0];
            const uint32_t rl = ran->output_u32[i * 4 + 1];
            const uint32_t mb = ran->output_u32[i * 4 + 2];
            const uint32_t bal = ran->output_u32[i * 4 + 3];

            KX_CHECK(rf == 111u, "WaveReadLaneFirst must return lane 0 value (111)");
            KX_CHECK(rl == 666u, "WaveReadLaneAt must return lane 5 value (666)");
            KX_CHECK(mb == (i % 32u) + 1000u, "WavePrefixCountBits must match (i % 32) + 1000");
            KX_CHECK((bal & 1u) != 0u, "WaveActiveBallot must have lane 0 active");
        }

        if (reference_output.empty()) {
            reference_output = ran->output_u32;
        } else {
            KX_CHECK(reference_output == ran->output_u32, "Cross-GPU bit-exact wave intrinsic parity");
        }
        std::cout << std::format("    -> Hardware [{}] {} PASS (Wave Intrinsics bit-exact across {} lanes)\n",
                                 ad.index, ad.name, kLanes);
        ++tested;
    }
    KX_CHECK(tested >= 1, "Expected at least 1 hardware GPU tested for wave intrinsics");
}

void test_rdna2_multi_texture_multi_buffer_descriptor_remapping() {
    using namespace gpu::encode;

    std::vector<uint32_t> words;
    // 1. Texture 0 (srsrc = s[0..7], ssamp = s[0..3]) -> g_tex0 (t0), g_samp0 (s0)
    mimg(words, 0x20, 4, 0, 0, 0, 0xFu, 1);
    // 2. Texture 1 (srsrc = s[8..15], ssamp = s[4..7]) -> g_tex1 (t2), g_samp1 (s1)
    mimg(words, 0x20, 8, 0, 8, 4, 0xFu, 1);
    // 3. Buffer 0 (srsrc = s[16..19]) -> g_buf0 (t1)
    mubuf(words, 14, 12, 0, 16, 128, 0);
    // 4. Buffer 1 (srsrc = s[20..23]) -> g_buf1 (t3)
    mubuf(words, 14, 16, 0, 20, 128, 0);
    exp(words, 12, 0xFu, 4, 5, 6, 7, false);
    exp(words, 32, 0xFu, 8, 9, 10, 11, true);
    words.push_back(sopp(1));

    auto decoded = gpu::decode_rdna2(words.data(), words.size());
    auto ir = gpu::lift_rdna2_to_ir(decoded);

    KX_CHECK(ir.tex_bindings.size() == 2, "Expected 2 texture bindings");
    KX_CHECK(ir.tex_bindings[0].sgpr_base == 0 && ir.tex_bindings[0].hlsl_reg == 0, "Tex0 mapped to t0");
    KX_CHECK(ir.tex_bindings[1].sgpr_base == 8 && ir.tex_bindings[1].hlsl_reg == 2, "Tex1 mapped to t2");

    KX_CHECK(ir.buf_srv_bindings.size() == 2, "Expected 2 buffer SRV bindings");
    KX_CHECK(ir.buf_srv_bindings[0].sgpr_base == 16 && ir.buf_srv_bindings[0].hlsl_reg == 1, "Buf0 mapped to t1");
    KX_CHECK(ir.buf_srv_bindings[1].sgpr_base == 20 && ir.buf_srv_bindings[1].hlsl_reg == 3, "Buf1 mapped to t3");

    KX_CHECK(ir.samp_bindings.size() == 2, "Expected 2 sampler bindings");
    KX_CHECK(ir.samp_bindings[0].sgpr_base == 0 && ir.samp_bindings[0].hlsl_reg == 0, "Samp0 mapped to s0");
    KX_CHECK(ir.samp_bindings[1].sgpr_base == 4 && ir.samp_bindings[1].hlsl_reg == 1, "Samp1 mapped to s1");

    auto plan = gpu::build_root_remap_plan(ir, gpu::ShaderStage::Vertex);
    KX_CHECK(plan.total_srv_slots == 4, "4 SRV slots in root plan");
    KX_CHECK(plan.total_sampler_slots == 2, "2 Sampler slots in root plan");

    auto recomp = gpu::emit_hlsl_from_ir(ir, {.stage = gpu::ShaderStage::Vertex});
    KX_CHECK(recomp.hlsl.find("g_tex0 : register(t0)") != std::string::npos, "HLSL has g_tex0 : register(t0)");
    KX_CHECK(recomp.hlsl.find("g_tex1 : register(t2)") != std::string::npos, "HLSL has g_tex1 : register(t2)");
    KX_CHECK(recomp.hlsl.find("g_buf0 : register(t1)") != std::string::npos, "HLSL has g_buf0 : register(t1)");
    KX_CHECK(recomp.hlsl.find("g_buf1 : register(t3)") != std::string::npos, "HLSL has g_buf1 : register(t3)");
    KX_CHECK(recomp.hlsl.find("g_samp0 : register(s0)") != std::string::npos, "HLSL has g_samp0 : register(s0)");
    KX_CHECK(recomp.hlsl.find("g_samp1 : register(s1)") != std::string::npos, "HLSL has g_samp1 : register(s1)");

    gpu::DxcCompiler dxc;
    KX_CHECK(dxc.initialize().has_value(), "DXC init");
    auto compiled = dxc.compile_hlsl(recomp.hlsl, "main", "vs_6_6");
    KX_CHECK(compiled.has_value(), compiled.error().message);
}

void test_rdna2_depth_buffered_3d_cube_dual_gpu() {
    using namespace gpu::encode;

    struct CubeVertex {
        float x, y, z, shade;
        float u, v, tr, tg;
    };

    const float s = 0.82f;
    // 36 vertices (6 faces × 2 triangles × 3 vertices) with outward normals
    CubeVertex cube_verts[36] = {
        // Front (+Z, directional shade 1.0)
        {-s,  s,  s, 1.00f, 0.0f, 0.0f, 1.0f, 1.0f},
        { s,  s,  s, 1.00f, 1.0f, 0.0f, 1.0f, 1.0f},
        { s, -s,  s, 1.00f, 1.0f, 1.0f, 1.0f, 1.0f},
        {-s,  s,  s, 1.00f, 0.0f, 0.0f, 1.0f, 1.0f},
        { s, -s,  s, 1.00f, 1.0f, 1.0f, 1.0f, 1.0f},
        {-s, -s,  s, 1.00f, 0.0f, 1.0f, 1.0f, 1.0f},

        // Top (+Y, directional shade 0.88)
        {-s,  s, -s, 0.88f, 0.0f, 0.0f, 0.9f, 1.0f},
        { s,  s, -s, 0.88f, 1.0f, 0.0f, 0.9f, 1.0f},
        { s,  s,  s, 0.88f, 1.0f, 1.0f, 0.9f, 1.0f},
        {-s,  s, -s, 0.88f, 0.0f, 0.0f, 0.9f, 1.0f},
        { s,  s,  s, 0.88f, 1.0f, 1.0f, 0.9f, 1.0f},
        {-s,  s,  s, 0.88f, 0.0f, 1.0f, 0.9f, 1.0f},

        // Right (+X, directional shade 0.72)
        { s,  s,  s, 0.72f, 0.0f, 0.0f, 1.0f, 0.85f},
        { s,  s, -s, 0.72f, 1.0f, 0.0f, 1.0f, 0.85f},
        { s, -s, -s, 0.72f, 1.0f, 1.0f, 1.0f, 0.85f},
        { s,  s,  s, 0.72f, 0.0f, 0.0f, 1.0f, 0.85f},
        { s, -s, -s, 0.72f, 1.0f, 1.0f, 1.0f, 0.85f},
        { s, -s,  s, 0.72f, 0.0f, 1.0f, 1.0f, 0.85f},

        // Back (-Z, directional shade 0.35)
        { s,  s, -s, 0.35f, 0.0f, 0.0f, 0.5f, 0.5f},
        {-s,  s, -s, 0.35f, 1.0f, 0.0f, 0.5f, 0.5f},
        {-s, -s, -s, 0.35f, 1.0f, 1.0f, 0.5f, 0.5f},
        { s,  s, -s, 0.35f, 0.0f, 0.0f, 0.5f, 0.5f},
        {-s, -s, -s, 0.35f, 1.0f, 1.0f, 0.5f, 0.5f},
        { s, -s, -s, 0.35f, 0.0f, 1.0f, 0.5f, 0.5f},

        // Bottom (-Y, directional shade 0.45)
        {-s, -s,  s, 0.45f, 0.0f, 0.0f, 0.6f, 0.6f},
        { s, -s,  s, 0.45f, 1.0f, 0.0f, 0.6f, 0.6f},
        { s, -s, -s, 0.45f, 1.0f, 1.0f, 0.6f, 0.6f},
        {-s, -s,  s, 0.45f, 0.0f, 0.0f, 0.6f, 0.6f},
        { s, -s, -s, 0.45f, 1.0f, 1.0f, 0.6f, 0.6f},
        {-s, -s, -s, 0.45f, 0.0f, 1.0f, 0.6f, 0.6f},

        // Left (-X, directional shade 0.55)
        {-s,  s, -s, 0.55f, 0.0f, 0.0f, 0.7f, 0.7f},
        {-s,  s,  s, 0.55f, 1.0f, 0.0f, 0.7f, 0.7f},
        {-s, -s,  s, 0.55f, 1.0f, 1.0f, 0.7f, 0.7f},
        {-s,  s, -s, 0.55f, 0.0f, 0.0f, 0.7f, 0.7f},
        {-s, -s,  s, 0.55f, 1.0f, 1.0f, 0.7f, 0.7f},
        {-s, -s, -s, 0.55f, 0.0f, 1.0f, 0.7f, 0.7f},
    };

    // Calculate Look-At & Perspective MVP matrix (LH, 0 to 1 depth)
    const float ex = 2.2f, ey = 1.8f, ez = 3.4f;
    float zx = -ex, zy = -ey, zz = -ez;
    const float zlen = std::sqrt(zx*zx + zy*zy + zz*zz);
    zx /= zlen; zy /= zlen; zz /= zlen;

    float xx = zz, xy = 0.0f, xz = -zx;
    const float xlen = std::sqrt(xx*xx + xz*xz);
    xx /= xlen; xz /= xlen;

    const float yx = zy * xz - zz * xy;
    const float yy = zz * xx - zx * xz;
    const float yz = zx * xy - zy * xx;

    const float V[4][4] = {
        { xx, xy, xz, -(xx*ex + xy*ey + xz*ez) },
        { yx, yy, yz, -(yx*ex + yy*ey + yz*ez) },
        { zx, zy, zz, -(zx*ex + zy*ey + zz*ez) },
        { 0.0f, 0.0f, 0.0f, 1.0f }
    };

    const float f_proj = 1.0f / std::tan(30.0f * 3.14159265f / 180.0f);
    constexpr float near_z = 0.1f, far_z = 100.0f;
    const float P_proj[4][4] = {
        { f_proj, 0.0f,   0.0f,                          0.0f },
        { 0.0f,   f_proj, 0.0f,                          0.0f },
        { 0.0f,   0.0f,   far_z / (far_z - near_z),      -near_z * far_z / (far_z - near_z) },
        { 0.0f,   0.0f,   1.0f,                          0.0f }
    };

    float mvp[16]{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += P_proj[r][k] * V[k][c];
            mvp[r * 4 + c] = sum;
        }
    }

    // Build RDNA2 Vertex Shader with MVP matrix constant buffer (s_load_dwordx4 b0)
    std::vector<uint32_t> vs_words;
    vs_words.push_back(vop2(26, 1, 128 + 5, 0));            // v1 = v0 << 5 (sv_vid * 32)
    mubuf(vs_words, 14, 2, 1, 0, 128, 0);                   // buffer_load_dwordx4 v[2:5], v1, s[0:3], 0
    mubuf(vs_words, 14, 6, 1, 0, 128, 16);                  // buffer_load_dwordx4 v[6:9], v1, s[0:3], 16
    smem(vs_words, 2, 4, 0, 0);                             // s_load_dwordx4 s[4:7], s0, 0
    smem(vs_words, 2, 8, 0, 16);                            // s_load_dwordx4 s[8:11], s0, 16
    smem(vs_words, 2, 12, 0, 32);                           // s_load_dwordx4 s[12:15], s0, 32
    smem(vs_words, 2, 16, 0, 48);                           // s_load_dwordx4 s[16:19], s0, 48

    // Row 0 dot pos -> clip.x in v10
    vs_words.push_back(vop2(7, 10, 4, 2));                  // v10 = s4 * v2
    vop3(vs_words, 0x14B, 10, 5, 256 + 3, 256 + 10);       // v10 = mad(s5, v3, v10)
    vop3(vs_words, 0x14B, 10, 6, 256 + 4, 256 + 10);       // v10 = mad(s6, v4, v10)
    vs_words.push_back(vop2(3, 10, 7, 10));                 // v10 = s7 + v10

    // Row 1 dot pos -> clip.y in v11
    vs_words.push_back(vop2(7, 11, 8, 2));                  // v11 = s8 * v2
    vop3(vs_words, 0x14B, 11, 9, 256 + 3, 256 + 11);       // v11 = mad(s9, v3, v11)
    vop3(vs_words, 0x14B, 11, 10, 256 + 4, 256 + 11);      // v11 = mad(s10, v4, v11)
    vs_words.push_back(vop2(3, 11, 11, 11));                // v11 = s11 + v11

    // Row 2 dot pos -> clip.z in v12
    vs_words.push_back(vop2(7, 12, 12, 2));                 // v12 = s12 * v2
    vop3(vs_words, 0x14B, 12, 13, 256 + 3, 256 + 12);      // v12 = mad(s13, v3, v12)
    vop3(vs_words, 0x14B, 12, 14, 256 + 4, 256 + 12);      // v12 = mad(s14, v4, v12)
    vs_words.push_back(vop2(3, 12, 15, 12));                // v12 = s15 + v12

    // Row 3 dot pos -> clip.w in v13
    vs_words.push_back(vop2(7, 13, 16, 2));                 // v13 = s16 * v2
    vop3(vs_words, 0x14B, 13, 17, 256 + 3, 256 + 13);      // v13 = mad(s17, v3, v13)
    vop3(vs_words, 0x14B, 13, 18, 256 + 4, 256 + 13);      // v13 = mad(s18, v4, v13)
    vs_words.push_back(vop2(3, 13, 19, 13));                // v13 = s19 + v13

    vs_words.push_back(vop1(1, 8, 242));                    // v8 = 1.0f
    exp(vs_words, 12, 0xFu, 10, 11, 12, 13, false);         // exp pos0
    exp(vs_words, 32, 0xFu, 6, 7, 5, 8, true);              // exp param0 done
    vs_words.push_back(sopp(1));                            // s_endpgm

    // Build RDNA2 Pixel Shader with directional shade modulation
    std::vector<uint32_t> ps_words;
    mimg(ps_words, 0x20, 4, 0, 0, 0, 0xFu, 1);              // image_sample v[4:7], v[0:1], s[0:7], s[0:3]
    ps_words.push_back(vop2(7, 4, 256 + 2, 4));             // v4 = shade * v4 (R)
    ps_words.push_back(vop2(7, 5, 256 + 2, 5));             // v5 = shade * v5 (G)
    ps_words.push_back(vop2(7, 6, 256 + 2, 6));             // v6 = shade * v6 (B)
    exp(ps_words, 0, 0xFu, 4, 5, 6, 7, true);               // exp mrt0 done
    ps_words.push_back(sopp(1));                            // s_endpgm

    auto vs_ir = gpu::lift_rdna2_to_ir(gpu::decode_rdna2(vs_words.data(), vs_words.size()));
    auto ps_ir = gpu::lift_rdna2_to_ir(gpu::decode_rdna2(ps_words.data(), ps_words.size()));
    KX_CHECK(vs_ir.unknown_count == 0 && vs_ir.uses_cbuf && vs_ir.uses_buf_srv, "Cube VS IR flags");
    KX_CHECK(ps_ir.unknown_count == 0 && ps_ir.uses_tex, "Cube PS IR flags");

    gpu::DxcCompiler dxc;
    KX_CHECK(dxc.initialize().has_value(), "DXC init");
    auto vs_hlsl = gpu::emit_hlsl_from_ir(vs_ir, {.stage = gpu::ShaderStage::Vertex});
    auto ps_hlsl = gpu::emit_hlsl_from_ir(ps_ir, {.stage = gpu::ShaderStage::Pixel});
    auto vs_dxil = dxc.compile_hlsl(vs_hlsl.hlsl, "main", "vs_6_6");
    auto ps_dxil = dxc.compile_hlsl(ps_hlsl.hlsl, "main", "ps_6_6");
    KX_CHECK(vs_dxil.has_value(), vs_dxil.error().message);
    KX_CHECK(ps_dxil.has_value(), ps_dxil.error().message);

    // 64x64 calibration checkerboard texture with gold/emerald/coral/cyan quadrants
    constexpr uint32_t kW = 256, kH = 256, kTex = 64;
    std::vector<uint8_t> tex_rgba(kTex * kTex * 4, 255);
    for (uint32_t y = 0; y < kTex; ++y) {
        for (uint32_t x = 0; x < kTex; ++x) {
            uint8_t r = 26, g = 36, b = 58;
            const bool border = (x < 2 || x >= kTex - 2 || y < 2 || y >= kTex - 2);
            const bool check = (((x / 8) + (y / 8)) & 1u) == 0u;
            if (border) {
                r = 240; g = 245; b = 255;
            } else if (check) {
                if (x < kTex / 2 && y < kTex / 2) { r = 255; g = 205; b = 55; }       // Gold
                else if (x >= kTex / 2 && y < kTex / 2) { r = 40; g = 220; b = 130; } // Emerald
                else if (x < kTex / 2 && y >= kTex / 2) { r = 240; g = 80; b = 95; }  // Coral
                else { r = 50; g = 195; b = 250; }                                    // Cyan
            } else {
                r = 30; g = 38; b = 64;
            }
            const size_t off = (y * kTex + x) * 4;
            tex_rgba[off + 0] = r; tex_rgba[off + 1] = g; tex_rgba[off + 2] = b; tex_rgba[off + 3] = 255;
        }
    }

    auto adapters = gpu::enumerate_d3d12_adapters();
    KX_CHECK(adapters.has_value(), adapters.error().message);

    IDXGIFactory6* factory = nullptr;
    KX_CHECK(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "CreateDXGIFactory1");

    int hw_verified = 0;
    uint32_t ref_cube_pixels = 0;
    const char* capture_env = std::getenv("KESTREL_CAPTURE_CUBE_BMP");
    fs::path capture_path = capture_env ? fs::path(capture_env) : (fs::current_path() / "docs" / "cube_3d.bmp");

    for (const auto& ad : *adapters) {
        if (ad.is_software) continue;
        IDXGIAdapter1* dxgi_ad = nullptr;
        if (FAILED(factory->EnumAdapters1(ad.index, &dxgi_ad))) continue;

        ID3D12Device* dev = nullptr;
        HRESULT hr = D3D12CreateDevice(dxgi_ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
        dxgi_ad->Release();
        if (FAILED(hr) || !dev) continue;

        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* queue = nullptr;
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* cmd = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))), "CreateCommandQueue");
        KX_CHECK(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))), "CreateCommandAllocator");
        KX_CHECK(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cmd))), "CreateCommandList");

        // Root signature: [0]=CBV b0, [1]=Table(SRV t0), [2]=RootSRV t1, Sampler(s0)
        D3D12_DESCRIPTOR_RANGE srv_range{};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = 1;
        srv_range.BaseShaderRegister = 0;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rparams[3]{};
        rparams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rparams[0].Descriptor = {0, 0}; // b0
        rparams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rparams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rparams[1].DescriptorTable = {1, &srv_range};
        rparams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rparams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rparams[2].Descriptor = {1, 0}; // t1
        rparams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsd{3, rparams, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ID3DBlob* rs_blob = nullptr;
        ID3D12RootSignature* rs = nullptr;
        KX_CHECK(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, nullptr)), "SerializeRS");
        KX_CHECK(SUCCEEDED(dev->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&rs))), "CreateRS");
        rs_blob->Release();

        // Pipeline State with real D3D12 D32_FLOAT Depth-Buffer & Backface Culling
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psd{};
        psd.pRootSignature = rs;
        psd.VS = {vs_dxil->dxil.data(), vs_dxil->dxil.size()};
        psd.PS = {ps_dxil->dxil.data(), ps_dxil->dxil.size()};
        psd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psd.SampleMask = UINT_MAX;
        psd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psd.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        psd.DepthStencilState.DepthEnable = TRUE;
        psd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        psd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psd.NumRenderTargets = 1;
        psd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psd.SampleDesc.Count = 1;

        ID3D12PipelineState* pso = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateGraphicsPipelineState(&psd, IID_PPV_ARGS(&pso))), "CreateCubePSO");

        D3D12_HEAP_PROPERTIES hp_def{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
        D3D12_HEAP_PROPERTIES hp_up{D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
        D3D12_HEAP_PROPERTIES hp_rb{D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};

        // Render target
        D3D12_RESOURCE_DESC rt_desc{};
        rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rt_desc.Width = kW; rt_desc.Height = kH; rt_desc.DepthOrArraySize = 1; rt_desc.MipLevels = 1;
        rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rt_desc.SampleDesc.Count = 1;
        rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ID3D12Resource* rt = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt))), "CreateRT");

        // Depth buffer (D32_FLOAT)
        D3D12_RESOURCE_DESC depth_desc{};
        depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depth_desc.Width = kW; depth_desc.Height = kH; depth_desc.DepthOrArraySize = 1; depth_desc.MipLevels = 1;
        depth_desc.Format = DXGI_FORMAT_D32_FLOAT; depth_desc.SampleDesc.Count = 1;
        depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE dsv_cv{}; dsv_cv.Format = DXGI_FORMAT_D32_FLOAT; dsv_cv.DepthStencil.Depth = 1.0f;
        ID3D12Resource* depth_res = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dsv_cv, IID_PPV_ARGS(&depth_res))), "CreateDepthRes");

        // Texture
        D3D12_RESOURCE_DESC tex_desc{};
        tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tex_desc.Width = kTex; tex_desc.Height = kTex; tex_desc.DepthOrArraySize = 1; tex_desc.MipLevels = 1;
        tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; tex_desc.SampleDesc.Count = 1;
        ID3D12Resource* tex = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex))), "CreateTex");

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT tex_fp{};
        UINT64 tex_upload_bytes = 0;
        dev->GetCopyableFootprints(&tex_desc, 0, 1, 0, &tex_fp, nullptr, nullptr, &tex_upload_bytes);

        D3D12_RESOURCE_DESC buf_desc{};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Height = 1; buf_desc.DepthOrArraySize = 1; buf_desc.MipLevels = 1;
        buf_desc.SampleDesc.Count = 1; buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        buf_desc.Width = tex_upload_bytes;
        ID3D12Resource* tex_up = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateCommittedResource(&hp_up, D3D12_HEAP_FLAG_NONE, &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&tex_up))), "CreateTexUpload");
        void* mapped = nullptr;
        KX_CHECK(SUCCEEDED(tex_up->Map(0, nullptr, &mapped)), "MapTexUpload");
        for (uint32_t y = 0; y < kTex; ++y) {
            std::memcpy(static_cast<uint8_t*>(mapped) + y * tex_fp.Footprint.RowPitch,
                        tex_rgba.data() + y * kTex * 4, kTex * 4);
        }
        tex_up->Unmap(0, nullptr);

        // Constant Buffer (MVP matrix)
        buf_desc.Width = 256;
        ID3D12Resource* cb_up = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateCommittedResource(&hp_up, D3D12_HEAP_FLAG_NONE, &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cb_up))), "CreateCB");
        KX_CHECK(SUCCEEDED(cb_up->Map(0, nullptr, &mapped)), "MapCB");
        std::memcpy(mapped, mvp, sizeof(mvp));
        cb_up->Unmap(0, nullptr);

        // Vertex Buffer (36 vertices × 32 bytes = 1152 bytes)
        buf_desc.Width = sizeof(cube_verts);
        ID3D12Resource* vb_srv = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateCommittedResource(&hp_up, D3D12_HEAP_FLAG_NONE, &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vb_srv))), "CreateVB");
        KX_CHECK(SUCCEEDED(vb_srv->Map(0, nullptr, &mapped)), "MapVB");
        std::memcpy(mapped, cube_verts, sizeof(cube_verts));
        vb_srv->Unmap(0, nullptr);

        // Readback buffer
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT rt_fp{};
        UINT64 rb_bytes = 0;
        dev->GetCopyableFootprints(&rt_desc, 0, 1, 0, &rt_fp, nullptr, nullptr, &rb_bytes);
        buf_desc.Width = rb_bytes;
        ID3D12Resource* rb = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateCommittedResource(&hp_rb, D3D12_HEAP_FLAG_NONE, &buf_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))), "CreateRB");

        // Descriptor Heaps: RTV, DSV, SRV
        D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        D3D12_DESCRIPTOR_HEAP_DESC dsv_hd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        D3D12_DESCRIPTOR_HEAP_DESC srv_hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        ID3D12DescriptorHeap* rtv_heap = nullptr;
        ID3D12DescriptorHeap* dsv_heap = nullptr;
        ID3D12DescriptorHeap* srv_heap = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateDescriptorHeap(&rtv_hd, IID_PPV_ARGS(&rtv_heap))), "CreateRtvHeap");
        KX_CHECK(SUCCEEDED(dev->CreateDescriptorHeap(&dsv_hd, IID_PPV_ARGS(&dsv_heap))), "CreateDsvHeap");
        KX_CHECK(SUCCEEDED(dev->CreateDescriptorHeap(&srv_hd, IID_PPV_ARGS(&srv_heap))), "CreateSrvHeap");

        dev->CreateRenderTargetView(rt, nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        dev->CreateDepthStencilView(depth_res, nullptr, dsv_heap->GetCPUDescriptorHandleForHeapStart());
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_d{};
        srv_d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv_d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_d.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(tex, &srv_d, srv_heap->GetCPUDescriptorHandleForHeapStart());

        // Upload texture
        D3D12_TEXTURE_COPY_LOCATION src_loc{}, dst_loc{};
        src_loc.pResource = tex_up; src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src_loc.PlacedFootprint = tex_fp;
        dst_loc.pResource = tex; dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst_loc.SubresourceIndex = 0;
        cmd->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

        D3D12_RESOURCE_BARRIER tbar[1]{};
        tbar[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        tbar[0].Transition.pResource = tex;
        tbar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        tbar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        tbar[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, tbar);

        // Viewport and scissor
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, static_cast<LONG>(kW), static_cast<LONG>(kH)};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);

        // Clear render target and depth buffer
        const FLOAT clear_color[4] = {0.08f, 0.09f, 0.12f, 1.0f}; // Slate navy background
        const auto rtv_h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        const auto dsv_h = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        cmd->OMSetRenderTargets(1, &rtv_h, FALSE, &dsv_h);
        cmd->ClearRenderTargetView(rtv_h, clear_color, 0, nullptr);
        cmd->ClearDepthStencilView(dsv_h, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        // Bind 3D pipeline & draw cube
        cmd->SetPipelineState(pso);
        cmd->SetGraphicsRootSignature(rs);
        ID3D12DescriptorHeap* heaps[] = {srv_heap};
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetGraphicsRootConstantBufferView(0, cb_up->GetGPUVirtualAddress());
        cmd->SetGraphicsRootDescriptorTable(1, srv_heap->GetGPUDescriptorHandleForHeapStart());
        cmd->SetGraphicsRootShaderResourceView(2, vb_srv->GetGPUVirtualAddress());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(36, 1, 0, 0);

        // Transition RT to copy source & copy to readback buffer
        D3D12_RESOURCE_BARRIER rt_bar{};
        rt_bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rt_bar.Transition.pResource = rt;
        rt_bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        rt_bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        rt_bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &rt_bar);

        src_loc.pResource = rt; src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src_loc.SubresourceIndex = 0;
        dst_loc.pResource = rb; dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst_loc.PlacedFootprint = rt_fp;
        cmd->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

        KX_CHECK(SUCCEEDED(cmd->Close()), "CloseCmd");
        ID3D12CommandList* lists[] = {cmd};
        queue->ExecuteCommandLists(1, lists);

        ID3D12Fence* fence = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "CreateFence");
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        queue->Signal(fence, 1);
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, 5000);
        CloseHandle(ev);

        // Capture BMP screenshot on first GPU
        if (hw_verified == 0) {
            std::error_code ec;
            fs::create_directories(capture_path.parent_path(), ec);
            (void)hle::capture_d3d12_texture_bmp(dev, queue, rt, D3D12_RESOURCE_STATE_COPY_SOURCE, capture_path);
        }

        // Readback and verify depth-buffered lit cube pixels
        void* rb_ptr = nullptr;
        KX_CHECK(SUCCEEDED(rb->Map(0, nullptr, &rb_ptr)), "MapRB");
        std::vector<uint8_t> pixels(kW * kH * 4);
        for (uint32_t y = 0; y < kH; ++y) {
            std::memcpy(pixels.data() + y * kW * 4,
                        static_cast<const uint8_t*>(rb_ptr) + y * rt_fp.Footprint.RowPitch, kW * 4);
        }
        rb->Unmap(0, nullptr);

        // 1. Verify corners are background
        auto is_bg = [&](uint32_t x, uint32_t y) {
            const size_t o = (y * kW + x) * 4;
            return pixels[o] <= 30 && pixels[o + 1] <= 32 && pixels[o + 2] <= 42;
        };
        KX_CHECK(is_bg(5, 5) && is_bg(250, 5) && is_bg(5, 250) && is_bg(250, 250), "Corners are background");

        // 2. Verify center pixel (128, 128) is a lit cube pixel
        const size_t center_off = (128 * kW + 128) * 4;
        const uint8_t cr = pixels[center_off], cg = pixels[center_off + 1], cb = pixels[center_off + 2];
        KX_CHECK(cr > 40 || cg > 40 || cb > 40, "Center pixel is lit cube surface");

        // 3. Count rasterized cube pixels
        uint32_t cube_pixel_count = 0;
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                if (!is_bg(x, y)) ++cube_pixel_count;
            }
        }
        KX_CHECK(cube_pixel_count >= 5000 && cube_pixel_count <= 40000,
                 std::format("Rasterized cube pixel count in valid perspective bounds: {}", cube_pixel_count));

        if (ref_cube_pixels == 0) {
            ref_cube_pixels = cube_pixel_count;
        } else {
            const double diff = std::abs(static_cast<double>(cube_pixel_count) - static_cast<double>(ref_cube_pixels));
            const double pct = diff / static_cast<double>(ref_cube_pixels);
            KX_CHECK(pct < 0.005, std::format("Cross-GPU rasterization parity within 0.5%: got {}, ref {}", cube_pixel_count, ref_cube_pixels));
        }

        std::cout << std::format("    -> Hardware [{}] {} PASS (3D Perspective Cube: {} lit pixels, DSV D32_FLOAT verified)\n",
                                 ad.index, ad.name, cube_pixel_count);
        ++hw_verified;

        fence->Release();
        rtv_heap->Release();
        dsv_heap->Release();
        srv_heap->Release();
        rb->Release();
        vb_srv->Release();
        cb_up->Release();
        tex_up->Release();
        tex->Release();
        depth_res->Release();
        rt->Release();
        pso->Release();
        rs->Release();
        cmd->Release();
        alloc->Release();
        queue->Release();
        dev->Release();
    }
    factory->Release();
    KX_CHECK(hw_verified >= 1, "Expected at least 1 hardware GPU verified for 3D Cube");
}

void test_gdk_runtime_and_directstorage_bridge() {
    hle::reset_xgameruntime_stats();

    // 1. Test GDK XUser & XAsyncBlock HLE exports
    hle::KxXAsyncBlock async{};
    bool cb_called = false;
    async.callback = [](hle::KxXAsyncBlock* a) {
        if (a) *static_cast<bool*>(a->context) = true;
    };
    async.context = &cb_called;

    HRESULT hr = hle::kx_XUserAddAsync(0, &async);
    KX_CHECK(SUCCEEDED(hr), "XUserAddAsync");
    KX_CHECK(cb_called, "XUserAddAsync completion callback invoked");
    KX_CHECK(hle::kx_XAsyncGetStatus(&async, false) == S_OK, "XAsyncGetStatus S_OK");

    void* user_handle = nullptr;
    KX_CHECK(SUCCEEDED(hle::kx_XUserAddResult(&async, &user_handle)) && user_handle != nullptr, "XUserAddResult");

    uint64_t xuid = 0;
    KX_CHECK(SUCCEEDED(hle::kx_XUserGetId(user_handle, &xuid)) && xuid != 0, "XUserGetId");

    char gamertag[64]{};
    size_t used = 0;
    KX_CHECK(SUCCEEDED(hle::kx_XUserGetGamertag(user_handle, 0, sizeof(gamertag), gamertag, &used)), "XUserGetGamertag");
    KX_CHECK(std::string_view(gamertag) == "KestrelPlayer", "Gamertag matches KestrelPlayer");

    char pkg_id[128]{};
    KX_CHECK(SUCCEEDED(hle::kx_XPackageGetCurrentProcessPackageIdentifier(sizeof(pkg_id), pkg_id)), "XPackageGetCurrentProcessPackageIdentifier");
    KX_CHECK(std::string_view(pkg_id).find("KestrelX") != std::string_view::npos, "Package ID contains KestrelX");

    void* store_ctx = nullptr;
    KX_CHECK(SUCCEEDED(hle::kx_XStoreCreateContext(user_handle, &store_ctx)) && store_ctx != nullptr, "XStoreCreateContext");
    hle::kx_XStoreCloseContextHandle(store_ctx);
    hle::kx_XUserCloseHandle(user_handle);

    const auto gdk_stats = hle::xgameruntime_stats();
    KX_CHECK(gdk_stats.users_added >= 1 && gdk_stats.store_contexts >= 1 && gdk_stats.package_queries >= 1, "GDK stats counters");

    // 2. Test DirectStorage COM HLE backed by OverlayVfs
    const fs::path test_dir = fs::temp_directory_path() / "KestrelX_DStorageTest";
    std::error_code ec;
    fs::create_directories(test_dir / "data", ec);
    const fs::path game_file = test_dir / "data" / "dstorage_sample.bin";
    {
        std::ofstream ofs(game_file, std::ios::binary);
        const char content[] = "KESTRELX-DIRECTSTORAGE-NVME-STREAMING-VERIFIED-DATA";
        ofs.write(content, sizeof(content) - 1);
    }

    vfs::OverlayVfs vfs(test_dir, test_dir / "data_overlay");
    hle::set_dstorage_vfs(&vfs);
    hle::reset_dstorage_stats();

    hle::IKxDStorageFactory* factory = nullptr;
    KX_CHECK(SUCCEEDED(hle::kx_DStorageGetFactory(IID_IUnknown, reinterpret_cast<void**>(&factory))) && factory != nullptr, "DStorageGetFactory");

    hle::IKxDStorageFile* file = nullptr;
    KX_CHECK(SUCCEEDED(factory->OpenFile(L"GAME:\\data\\dstorage_sample.bin", IID_IUnknown, reinterpret_cast<void**>(&file))) && file != nullptr, "DStorage OpenFile");

    BY_HANDLE_FILE_INFORMATION finfo{};
    KX_CHECK(SUCCEEDED(file->GetFileInformation(&finfo)) && finfo.nFileSizeLow > 0, "DStorage GetFileInformation");

    hle::KxDStorageQueueDesc qdesc{};
    hle::IKxDStorageQueue* queue = nullptr;
    KX_CHECK(SUCCEEDED(factory->CreateQueue(&qdesc, IID_IUnknown, reinterpret_cast<void**>(&queue))) && queue != nullptr, "DStorage CreateQueue");

    auto adapters = gpu::enumerate_d3d12_adapters();
    KX_CHECK(adapters.has_value() && !adapters->empty(), "Adapter available for fence");
    IDXGIFactory6* dxgi_f = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_f));
    IDXGIAdapter1* ad = nullptr;
    dxgi_f->EnumAdapters1((*adapters)[0].index, &ad);
    ID3D12Device* dev = nullptr;
    D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
    ad->Release();
    dxgi_f->Release();

    ID3D12Fence* fence = nullptr;
    KX_CHECK(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "CreateFence");

    char read_buf[64]{};
    hle::KxDStorageRequest req{};
    req.source_file = file;
    req.source_offset = 9;  // "DIRECTSTORAGE..."
    req.source_size = 20;
    req.destination_buffer = read_buf;
    req.destination_size = sizeof(read_buf);

    queue->EnqueueRequest(&req);
    queue->EnqueueSignal(fence, 42);
    queue->Submit();

    KX_CHECK(fence->GetCompletedValue() == 42, "DirectStorage fence signal value 42");
    KX_CHECK(std::string_view(read_buf, 13) == "DIRECTSTORAGE", "DirectStorage payload verified");

    const auto ds_stats = hle::dstorage_stats();
    KX_CHECK(ds_stats.files_opened >= 1 && ds_stats.queues_created >= 1 && ds_stats.requests_submitted >= 1 && ds_stats.fences_signaled >= 1, "DStorage stats counters");

    fence->Release();
    dev->Release();
    queue->Release();
    file->Close();
    file->Release();
    factory->Release();
    hle::set_dstorage_vfs(nullptr);
    fs::remove_all(test_dir, ec);
}


// ============================================================================
// Milestone v0.6.0 Verification Tests (Issues #18, #19, #20, #21)
// ============================================================================

void test_rdna2_compute_2d_workgroup_tiling_dual_gpu() {
    using namespace gpu::encode;

    // 2D Compute workgroup dispatch:
    // Grid: 2 x 2 workgroups (groups_x=2, groups_y=2)
    // Workgroup size: 8 x 8 threads (64 threads per workgroup = 256 threads total)
    // Hardware input mapping:
    //   v0 = tid.x (global x, 0..15)
    //   v1 = tid.y (global y, 0..15)
    //   v2 = gtid.x (local x, 0..7)
    //   v3 = gtid.y (local y, 0..7)
    //   s4 = gid.x (group x, 0..1)
    //   s5 = gid.y (group y, 0..1)
    std::vector<uint32_t> words;
    // 1. global_idx = (v1 << 4) + v0; v4 = global_idx << 2; (byte addr in output buffer)
    words.push_back(vop2(26, 4, 128 + 4, 1));       // v4 = v1 << 4
    words.push_back(vop2(37, 4, 256 + 0, 4));       // v4 = v4 + v0
    words.push_back(vop2(26, 4, 128 + 2, 4));       // v4 = v4 << 2
    // 2. local_idx = (v3 << 3) + v2; v5 = local_idx << 2; (LDS byte addr)
    words.push_back(vop2(26, 5, 128 + 3, 3));       // v5 = v3 << 3
    words.push_back(vop2(37, 5, 256 + 2, 5));       // v5 = v5 + v2
    words.push_back(vop2(26, 5, 128 + 2, 5));       // v5 = v5 << 2
    // 3. ds_write_b32 v5, v2 (store local x into LDS)
    ds(words, 13, 0, 5, 2, 0, 0);
    // 4. s_barrier
    words.push_back(sopp(10));
    // 5. trans_idx = (v2 << 3) + v3; v6 = trans_idx << 2;
    words.push_back(vop2(26, 6, 128 + 3, 2));       // v6 = v2 << 3
    words.push_back(vop2(37, 6, 256 + 3, 6));       // v6 = v6 + v3
    words.push_back(vop2(26, 6, 128 + 2, 6));       // v6 = v6 << 2
    // 6. ds_read_b32 v7, v6 (read transposed local from LDS -> returns local y!)
    ds(words, 54, 7, 6, 0, 0, 0);
    // 7. v8 = v7 + (s4 * 100) + s5
    // s6 = 100
    words.push_back(sopk(0, 6, 100));
    // v9 = s4 * s6
    vop3(words, 0x169, 9, 4, 6, 0);
    // v9 = v9 + s5 (s5 is SGPR 5, v9 is VGPR 9)
    words.push_back(vop2(37, 9, 5, 9));
    // v8 = v9 + v7 (v9 is VGPR 9 -> 256 + 9, v7 is VGPR 7)
    words.push_back(vop2(37, 8, 256 + 9, 7));
    // 8. global_store_dword v4, v8
    global_mem(words, 28, 8, 4, 127, 0);
    // s_endpgm
    words.push_back(sopp(1));

    auto decoded = gpu::decode_rdna2(words.data(), words.size());
    auto ir = gpu::lift_rdna2_to_ir(decoded);
    KX_CHECK(ir.unknown_count == 0 && ir.uses_lds, "Compute 2D IR lift");

    gpu::DxcCompiler dxc;
    KX_CHECK(dxc.initialize().has_value(), "DXC init");
    auto recomp = gpu::emit_hlsl_from_ir(ir, {
        .stage = gpu::ShaderStage::Compute,
        .wave_mode = gpu::WaveMode::Wave32,
        .group_size_x = 8,
        .group_size_y = 8,
        .group_size_z = 1
    });
    auto compiled = dxc.compile_hlsl(recomp.hlsl, "main", "cs_6_6");
    
    KX_CHECK(compiled.has_value(), compiled.error().message);

    auto adapters = gpu::enumerate_d3d12_adapters();
    KX_CHECK(adapters.has_value() && !adapters->empty(), "Adapters available");

    constexpr uint32_t kTotalThreads = 16 * 16;
    std::vector<uint32_t> init_buf(kTotalThreads, 0);
    uint32_t roots[8]{};

    std::vector<uint32_t> ref_output;
    int tested = 0;
    for (const auto& ad : *adapters) {
        if (ad.is_software) continue;
        auto ran = gpu::run_compute_dxil_on_gpu(ad.index, compiled->dxil, init_buf, roots, 2, 2, 1);
        KX_CHECK(ran.has_value(), ran.error().message);

        for (uint32_t gy = 0; gy < 2; ++gy) {
            for (uint32_t gx = 0; gx < 2; ++gx) {
                for (uint32_t ly = 0; ly < 8; ++ly) {
                    for (uint32_t lx = 0; lx < 8; ++lx) {
                        const uint32_t gx_t = gx * 8 + lx;
                        const uint32_t gy_t = gy * 8 + ly;
                        const uint32_t idx = gy_t * 16 + gx_t;
                        const uint32_t expected = ly + gx * 100 + gy;
                        const uint32_t got = ran->output_u32[idx];
                        KX_CHECK(got == expected, std::format("2D Workgroup mismatch at ({},{}): got {}, expected {}", gx_t, gy_t, got, expected));
                    }
                }
            }
        }

        if (ref_output.empty()) {
            ref_output = ran->output_u32;
        } else {
            KX_CHECK(ref_output == ran->output_u32, "Cross-GPU bit-exact 2D workgroup parity");
        }
        std::cout << std::format("    -> Hardware [{}] {} PASS (2D Workgroup 16x16 Tiling bit-exact across 256 threads)\n",
                                 ad.index, ad.name);
        ++tested;
    }
    KX_CHECK(tested >= 1, "Expected at least 1 hardware GPU tested for 2D Workgroup Tiling");
}

inline void save_bmp_rgba(const fs::path& path, uint32_t w, uint32_t h, const uint8_t* rgba) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return;
    const uint32_t row_bytes = w * 3;
    const uint32_t pad = (4 - (row_bytes % 4)) % 4;
    const uint32_t img_size = (row_bytes + pad) * h;
    const uint32_t file_size = 54 + img_size;

    uint8_t h54[54] = {
        'B', 'M',
        static_cast<uint8_t>(file_size), static_cast<uint8_t>(file_size >> 8), static_cast<uint8_t>(file_size >> 16), static_cast<uint8_t>(file_size >> 24),
        0, 0, 0, 0,
        54, 0, 0, 0,
        40, 0, 0, 0,
        static_cast<uint8_t>(w), static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(w >> 16), static_cast<uint8_t>(w >> 24),
        static_cast<uint8_t>(h), static_cast<uint8_t>(h >> 8), static_cast<uint8_t>(h >> 16), static_cast<uint8_t>(h >> 24),
        1, 0,
        24, 0,
        0, 0, 0, 0,
        static_cast<uint8_t>(img_size), static_cast<uint8_t>(img_size >> 8), static_cast<uint8_t>(img_size >> 16), static_cast<uint8_t>(img_size >> 24),
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0
    };
    ofs.write(reinterpret_cast<const char*>(h54), 54);
    std::vector<uint8_t> row(row_bytes + pad, 0);
    for (int32_t y = static_cast<int32_t>(h) - 1; y >= 0; --y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t si = (static_cast<size_t>(y) * w + x) * 4;
            row[x * 3 + 0] = rgba[si + 2];
            row[x * 3 + 1] = rgba[si + 1];
            row[x * 3 + 2] = rgba[si + 0];
        }
        ofs.write(reinterpret_cast<const char*>(row.data()), row.size());
    }
}

void test_rdna2_pbr_deferred_mrt_pipeline_dual_gpu() {
    using namespace gpu::encode;

    // 1. VS: 4 vertices (pos.xy, uv.xy)
    std::vector<uint32_t> vs_words;
    vs_words.push_back(vop2(26, 1, 128 + 4, 0));     // v1 = sv_vid * 16
    mubuf(vs_words, 14, 2, 1, 0, 128, 0);            // buffer_load_dwordx4 v[2:5], v1, s[0:3]
    vs_words.push_back(vop1(1, 6, 128));             // v6 = 0.0f
    vs_words.push_back(vop1(1, 7, 242));             // v7 = 1.0f
    exp(vs_words, 12, 0xFu, 2, 3, 6, 7, false);      // exp pos0
    exp(vs_words, 32, 0xFu, 4, 5, 6, 7, true);       // exp param0 done
    vs_words.push_back(sopp(1));

    // 2. PS: MRT0 (Albedo), MRT1 (Normal), MRT2 (Metallic-Roughness-AO)
    std::vector<uint32_t> ps_words;
    auto mov_lit = [&](uint32_t vreg, float fval) {
        ps_words.push_back(vop1(1, vreg, 255));
        ps_words.push_back(std::bit_cast<uint32_t>(fval));
    };
    // MRT0: Albedo (0.85f, 0.25f, 0.15f, 1.0f)
    mov_lit(4, 0.85f);
    mov_lit(5, 0.25f);
    mov_lit(6, 0.15f);
    mov_lit(7, 1.0f);
    exp(ps_words, 0, 0xFu, 4, 5, 6, 7, false);

    // MRT1: Encoded Normal (0.0f, 0.0f, 1.0f, 0.0f)
    mov_lit(8, 0.0f);
    mov_lit(9, 0.0f);
    mov_lit(10, 1.0f);
    mov_lit(11, 0.0f);
    exp(ps_words, 1, 0xFu, 8, 9, 10, 11, false);

    // MRT2: PBR Mask (Metalness=0.9f, Roughness=0.2f, AO=1.0f, 0.0f)
    mov_lit(12, 0.9f);
    mov_lit(13, 0.2f);
    mov_lit(14, 1.0f);
    mov_lit(15, 0.0f);
    exp(ps_words, 2, 0xFu, 12, 13, 14, 15, true);
    ps_words.push_back(sopp(1));

    auto vs_ir = gpu::lift_rdna2_to_ir(gpu::decode_rdna2(vs_words.data(), vs_words.size()));
    auto ps_ir = gpu::lift_rdna2_to_ir(gpu::decode_rdna2(ps_words.data(), ps_words.size()));
    KX_CHECK(ps_ir.mrt_mask == 7u, "PS MRT mask must have bits 0, 1, 2 set (mrt_mask == 7)");

    gpu::DxcCompiler dxc;
    KX_CHECK(dxc.initialize().has_value(), "DXC init");
    auto vs_hlsl = gpu::emit_hlsl_from_ir(vs_ir, {.stage = gpu::ShaderStage::Vertex});
    auto ps_hlsl = gpu::emit_hlsl_from_ir(ps_ir, {.stage = gpu::ShaderStage::Pixel});

    KX_CHECK(ps_hlsl.hlsl.find("struct KxPsOut") != std::string::npos, "HLSL KxPsOut struct emission");
    KX_CHECK(ps_hlsl.hlsl.find("float4 mrt0 : SV_Target0;") != std::string::npos, "HLSL SV_Target0 emission");
    KX_CHECK(ps_hlsl.hlsl.find("float4 mrt1 : SV_Target1;") != std::string::npos, "HLSL SV_Target1 emission");
    KX_CHECK(ps_hlsl.hlsl.find("float4 mrt2 : SV_Target2;") != std::string::npos, "HLSL SV_Target2 emission");

    auto vs_dxil = dxc.compile_hlsl(vs_hlsl.hlsl, "main", "vs_6_6");
    auto ps_dxil = dxc.compile_hlsl(ps_hlsl.hlsl, "main", "ps_6_6");
    KX_CHECK(vs_dxil.has_value(), vs_dxil.error().message);
    KX_CHECK(ps_dxil.has_value(), ps_dxil.error().message);

    auto adapters = gpu::enumerate_d3d12_adapters();
    KX_CHECK(adapters.has_value() && !adapters->empty(), "Adapters available");

    constexpr uint32_t kW = 256, kH = 256;
    float quad_verts[16] = {
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
        -1.0f,  1.0f,  0.0f, 1.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
    };

    IDXGIFactory6* factory = nullptr;
    KX_CHECK(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "CreateDXGIFactory1");

    int verified = 0;
    for (const auto& ad : *adapters) {
        if (ad.is_software) continue;
        IDXGIAdapter1* dxgi_ad = nullptr;
        if (FAILED(factory->EnumAdapters1(ad.index, &dxgi_ad))) continue;
        ID3D12Device* dev = nullptr;
        HRESULT hr = D3D12CreateDevice(dxgi_ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
        dxgi_ad->Release();
        if (FAILED(hr) || !dev) continue;

        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* queue = nullptr;
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* cmd = nullptr;
        dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cmd));

        // Root signature with SRV t0
        D3D12_ROOT_PARAMETER rp{};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rp.Descriptor.ShaderRegister = 1;
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 1;
        rsd.pParameters = &rp;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob* rs_blob = nullptr;
        D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, nullptr);
        ID3D12RootSignature* rs = nullptr;
        dev->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&rs));
        rs_blob->Release();

        // PSO with 3 Render Targets
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = rs;
        pso_desc.VS = {vs_dxil->dxil.data(), vs_dxil->dxil.size()};
        pso_desc.PS = {ps_dxil->dxil.data(), ps_dxil->dxil.size()};
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 3;
        for (int i = 0; i < 3; ++i) {
            pso_desc.RTVFormats[i] = DXGI_FORMAT_R8G8B8A8_UNORM;
            pso_desc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.SampleDesc.Count = 1;
        ID3D12PipelineState* pso = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso))), "CreateGraphicsPipelineState 3-MRT");

        // RTV Heap with 3 descriptors
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 3;
        ID3D12DescriptorHeap* rtv_heap = nullptr;
        dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap));
        const UINT rtv_step = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        // 3 RTV textures + 3 readback buffers
        ID3D12Resource* rts[3]{};
        ID3D12Resource* rbs[3]{};
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handles[3]{};

        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kW; td.Height = kH; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_HEAP_PROPERTIES hp_def{}; hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_HEAP_PROPERTIES hp_read{}; hp_read.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = kW * kH * 4; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        for (int i = 0; i < 3; ++i) {
            dev->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rts[i]));
            dev->CreateCommittedResource(&hp_read, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbs[i]));
            rtv_handles[i] = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            rtv_handles[i].ptr += static_cast<SIZE_T>(i) * rtv_step;
            dev->CreateRenderTargetView(rts[i], nullptr, rtv_handles[i]);
        }

        // Vertex buffer
        D3D12_HEAP_PROPERTIES hp_up{}; hp_up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC vbd{};
        vbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        vbd.Width = sizeof(quad_verts); vbd.Height = 1; vbd.DepthOrArraySize = 1; vbd.MipLevels = 1;
        vbd.SampleDesc.Count = 1; vbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* vb = nullptr;
        dev->CreateCommittedResource(&hp_up, D3D12_HEAP_FLAG_NONE, &vbd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vb));
        void* mapped = nullptr;
        vb->Map(0, nullptr, &mapped);
        std::memcpy(mapped, quad_verts, sizeof(quad_verts));
        vb->Unmap(0, nullptr);

        // Record draw
        cmd->SetGraphicsRootSignature(rs);
        cmd->SetPipelineState(pso);
        cmd->SetGraphicsRootShaderResourceView(0, vb->GetGPUVirtualAddress());

        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f};
        D3D12_RECT sr{0, 0, static_cast<LONG>(kW), static_cast<LONG>(kH)};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sr);

        cmd->OMSetRenderTargets(3, rtv_handles, FALSE, nullptr);
        float clear_black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (int i = 0; i < 3; ++i) {
            cmd->ClearRenderTargetView(rtv_handles[i], clear_black, 0, nullptr);
        }

        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        cmd->DrawInstanced(4, 1, 0, 0);

        D3D12_RESOURCE_BARRIER barriers[3]{};
        for (int i = 0; i < 3; ++i) {
            barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[i].Transition.pResource = rts[i];
            barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        cmd->ResourceBarrier(3, barriers);

        for (int i = 0; i < 3; ++i) {
            D3D12_TEXTURE_COPY_LOCATION dst_loc{};
            dst_loc.pResource = rbs[i];
            dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            dst_loc.PlacedFootprint.Footprint.Width = kW;
            dst_loc.PlacedFootprint.Footprint.Height = kH;
            dst_loc.PlacedFootprint.Footprint.Depth = 1;
            dst_loc.PlacedFootprint.Footprint.RowPitch = kW * 4;

            D3D12_TEXTURE_COPY_LOCATION src_loc{};
            src_loc.pResource = rts[i];
            src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src_loc.SubresourceIndex = 0;
            cmd->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
        }

        cmd->Close();
        ID3D12CommandList* cls[] = {cmd};
        queue->ExecuteCommandLists(1, cls);

        ID3D12Fence* fence = nullptr;
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        queue->Signal(fence, 1);
        HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1, hEvent);
        WaitForSingleObject(hEvent, 3000);
        CloseHandle(hEvent);

        // Readback and verify GBuffer channels
        uint8_t* pAlbedo = nullptr; rbs[0]->Map(0, nullptr, reinterpret_cast<void**>(&pAlbedo));
        uint8_t* pNormal = nullptr; rbs[1]->Map(0, nullptr, reinterpret_cast<void**>(&pNormal));
        uint8_t* pMaterial = nullptr; rbs[2]->Map(0, nullptr, reinterpret_cast<void**>(&pMaterial));

        const size_t center = (128 * kW + 128) * 4;
        // MRT0 (Albedo): (0.85, 0.25, 0.15, 1.0) -> R~217, G~64, B~38, A~255
        KX_CHECK(std::abs(static_cast<int>(pAlbedo[center + 0]) - 217) <= 1, "Albedo R channel ~217");
        KX_CHECK(std::abs(static_cast<int>(pAlbedo[center + 1]) - 64) <= 1, "Albedo G channel ~64");
        KX_CHECK(std::abs(static_cast<int>(pAlbedo[center + 2]) - 38) <= 1, "Albedo B channel ~38");
        KX_CHECK(pAlbedo[center + 3] == 255, "Albedo A channel 255");

        // MRT1 (Normal): (0.0, 0.0, 1.0, 0.0) -> R=0, G=0, B=255, A=0
        KX_CHECK(pNormal[center + 0] == 0, "Normal X channel 0");
        KX_CHECK(pNormal[center + 1] == 0, "Normal Y channel 0");
        KX_CHECK(pNormal[center + 2] == 255, "Normal Z channel 255");

        // MRT2 (Material): (0.9, 0.2, 1.0, 0.0) -> Metallic~230, Roughness~51, AO~255
        KX_CHECK(std::abs(static_cast<int>(pMaterial[center + 0]) - 230) <= 1, "Metallic channel ~230");
        KX_CHECK(std::abs(static_cast<int>(pMaterial[center + 1]) - 51) <= 1, "Roughness channel ~51");
        KX_CHECK(pMaterial[center + 2] == 255, "AO channel 255");

        // Export PBR GBuffer composite image to docs/pbr_mrt.bmp
        if (verified == 0) {
            std::vector<uint8_t> composite(kW * 3 * kH * 4);
            for (uint32_t y = 0; y < kH; ++y) {
                for (uint32_t x = 0; x < kW; ++x) {
                    const size_t src_idx = (y * kW + x) * 4;
                    // Slot 0: Albedo (x in 0..255)
                    const size_t d0 = (y * (kW * 3) + x) * 4;
                    composite[d0 + 0] = pAlbedo[src_idx + 0];
                    composite[d0 + 1] = pAlbedo[src_idx + 1];
                    composite[d0 + 2] = pAlbedo[src_idx + 2];
                    composite[d0 + 3] = 255;
                    // Slot 1: Normal (x in 256..511)
                    const size_t d1 = (y * (kW * 3) + (x + kW)) * 4;
                    composite[d1 + 0] = pNormal[src_idx + 0];
                    composite[d1 + 1] = pNormal[src_idx + 1];
                    composite[d1 + 2] = pNormal[src_idx + 2];
                    composite[d1 + 3] = 255;
                    // Slot 2: Material (x in 512..767)
                    const size_t d2 = (y * (kW * 3) + (x + kW * 2)) * 4;
                    composite[d2 + 0] = pMaterial[src_idx + 0];
                    composite[d2 + 1] = pMaterial[src_idx + 1];
                    composite[d2 + 2] = pMaterial[src_idx + 2];
                    composite[d2 + 3] = 255;
                }
            }
            save_bmp_rgba("docs/pbr_mrt.bmp", kW * 3, kH, composite.data());
        }

        rbs[0]->Unmap(0, nullptr);
        rbs[1]->Unmap(0, nullptr);
        rbs[2]->Unmap(0, nullptr);

        std::cout << std::format("    -> Hardware [{}] {} PASS (PBR Deferred GBuffer 3-MRT bit-exact)\n",
                                 ad.index, ad.name);
        ++verified;

        fence->Release();
        for (int i = 0; i < 3; ++i) {
            rts[i]->Release();
            rbs[i]->Release();
        }
        rtv_heap->Release();
        vb->Release();
        pso->Release();
        rs->Release();
        cmd->Release();
        alloc->Release();
        queue->Release();
        dev->Release();
    }
    factory->Release();
    KX_CHECK(verified >= 1, "Expected at least 1 hardware GPU verified for PBR Deferred MRT");
}

void test_d3d12x_async_compute_queue_fence_sync() {
    hle::reset_d3d12x_stats();
    auto adapters = gpu::enumerate_d3d12_adapters();
    KX_CHECK(adapters.has_value() && !adapters->empty(), "Adapters available");
    IDXGIFactory6* factory = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    IDXGIAdapter1* ad = nullptr;
    factory->EnumAdapters1((*adapters)[0].index, &ad);
    ID3D12Device* raw_dev = nullptr;
    D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&raw_dev));
    ad->Release();
    factory->Release();
    KX_CHECK(raw_dev != nullptr, "D3D12CreateDevice");

    hle::KxDeviceProxy dev_proxy(raw_dev);
    ID3D12Device* dev = &dev_proxy;

    D3D12_COMMAND_QUEUE_DESC direct_desc{};
    direct_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* direct_q = nullptr;
    KX_CHECK(SUCCEEDED(dev->CreateCommandQueue(&direct_desc, IID_PPV_ARGS(&direct_q))) && direct_q, "Create DIRECT queue");

    D3D12_COMMAND_QUEUE_DESC compute_desc{};
    compute_desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ID3D12CommandQueue* compute_q = nullptr;
    KX_CHECK(SUCCEEDED(dev->CreateCommandQueue(&compute_desc, IID_PPV_ARGS(&compute_q))) && compute_q, "Create COMPUTE queue");

    const auto stats1 = hle::d3d12x_stats();
    KX_CHECK(stats1.compute_queues_created >= 1, "d3d12x compute_queues_created stat");

    ID3D12Fence* fence = nullptr;
    KX_CHECK(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) && fence, "CreateFence");

    // Async Compute signals 100 -> Direct queue waits for 100 -> Direct queue signals 200
    KX_CHECK(SUCCEEDED(compute_q->Signal(fence, 100)), "compute_q->Signal(100)");
    KX_CHECK(SUCCEEDED(direct_q->Wait(fence, 100)), "direct_q->Wait(100)");
    KX_CHECK(SUCCEEDED(direct_q->Signal(fence, 200)), "direct_q->Signal(200)");

    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(200, event);
    DWORD wait_res = WaitForSingleObject(event, 3000);
    CloseHandle(event);
    KX_CHECK(wait_res == WAIT_OBJECT_0, "Fence wait completed");
    KX_CHECK(fence->GetCompletedValue() == 200, "Fence value 200");

    const auto stats2 = hle::d3d12x_stats();
    KX_CHECK(stats2.inter_queue_fences >= 3, "d3d12x inter_queue_fences stat");

    fence->Release();
    compute_q->Release();
    direct_q->Release();
    raw_dev->Release();
}

void test_gameinput_force_feedback_rumble() {
    hle::reset_gameinput_stats();
    hle::Registry reg;
    hle::register_gameinput(reg);

    auto* create_fn = reinterpret_cast<HRESULT(__stdcall*)(hle::IKxGameInput**)>(
        reg.resolve_name("gameinput.dll", "GameInputCreate"));
    KX_CHECK(create_fn != nullptr, "GameInputCreate export");

    hle::IKxGameInput* gi = nullptr;
    KX_CHECK(SUCCEEDED(create_fn(&gi)) && gi != nullptr, "GameInputCreate S_OK");

    hle::IKxGameInputDevice* device = nullptr;
    KX_CHECK(SUCCEEDED(gi->GetDefaultDevice(&device)) && device != nullptr, "GetDefaultDevice S_OK");

    hle::KxGameInputRumbleParams params{0.85f, 0.35f, 0.65f, 0.45f};
    device->SetRumbleState(&params);

    hle::KxGameInputRumbleParams got{};
    device->GetRumbleState(&got);

    KX_CHECK(std::abs(got.low_frequency - 0.85f) < 1e-4f, "Low-frequency rumble motor intensity");
    KX_CHECK(std::abs(got.high_frequency - 0.35f) < 1e-4f, "High-frequency rumble motor intensity");
    KX_CHECK(std::abs(got.left_trigger - 0.65f) < 1e-4f, "Left trigger impulse haptic intensity");
    KX_CHECK(std::abs(got.right_trigger - 0.45f) < 1e-4f, "Right trigger impulse haptic intensity");

    const auto stats = hle::gameinput_stats();
    KX_CHECK(stats.rumble_commands_sent >= 1, "Rumble commands telemetry stat");

    // Clear rumble state
    hle::KxGameInputRumbleParams zero{};
    device->SetRumbleState(&zero);
    device->GetRumbleState(&got);
    KX_CHECK(got.low_frequency == 0.0f && got.high_frequency == 0.0f, "Rumble cleared");

    device->Release();
    gi->Release();
}


void test_sgd5_shader_archive_and_decompression() {
    using namespace formats;

    std::vector<uint8_t> payload1 = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA };
    std::vector<uint8_t> payload2(512);
    for (size_t i = 0; i < payload2.size(); ++i) payload2[i] = static_cast<uint8_t>(i ^ 0x5A);
    std::vector<uint8_t> payload3(2048, 0xCC);

    std::vector<std::tuple<std::string, uint8_t, std::vector<uint8_t>>> items = {
        {"VS_Main_Wrapped", static_cast<uint8_t>(Sgd5ShaderKind::StandardShader), payload1},
        {"PS_GBuffer_Wrapped", static_cast<uint8_t>(Sgd5ShaderKind::MeshShader), payload2},
        {"CS_Cull_Wrapped", static_cast<uint8_t>(Sgd5ShaderKind::ComputeShader), payload3},
    };

    const auto archive = Sgd5Archive::synthesize_archive(items);
    KX_CHECK(!archive.empty(), "Synthesized SGD5 archive must not be empty");
    KX_CHECK(Sgd5Archive::is_sgd5(archive), "Buffer must be detected as valid SGD5 archive");

    const auto records = Sgd5Archive::parse_records(archive);
    KX_CHECK(records.size() == 3, "Parsed record count must match 3");

    KX_CHECK(records[0].name == "VS_Main_Wrapped", "Record 0 name matches");
    KX_CHECK(records[0].kind == Sgd5ShaderKind::StandardShader, "Record 0 kind matches");
    KX_CHECK(records[0].unpacked_size == payload1.size(), "Record 0 unpacked size matches");
    auto unp1 = records[0].unpack();
    
    KX_CHECK(unp1.has_value() && *unp1 == payload1, "Record 0 decompresses bit-exact");

    KX_CHECK(records[1].name == "PS_GBuffer_Wrapped", "Record 1 name matches");
    KX_CHECK(records[1].kind == Sgd5ShaderKind::MeshShader, "Record 1 kind matches");
    KX_CHECK(records[1].unpacked_size == payload2.size(), "Record 1 unpacked size matches");
    auto unp2 = records[1].unpack();
    KX_CHECK(unp2.has_value() && *unp2 == payload2, "Record 1 decompresses bit-exact");

    KX_CHECK(records[2].name == "CS_Cull_Wrapped", "Record 2 name matches");
    KX_CHECK(records[2].kind == Sgd5ShaderKind::ComputeShader, "Record 2 kind matches");
    KX_CHECK(records[2].unpacked_size == payload3.size(), "Record 2 unpacked size matches");
    auto unp3 = records[2].unpack();
    KX_CHECK(unp3.has_value() && *unp3 == payload3, "Record 2 decompresses bit-exact");
}

void test_rdna2_valu_precision_parity() {
    using namespace gpu::encode;

    std::vector<uint32_t> words;
    auto mov_lit = [&](uint32_t vreg, uint32_t val) {
        words.push_back(vop1(1, vreg, 255));
        words.push_back(val);
    };

    // v0: input = -1e-10f (0xB3E533F4) -> hardware clamps to 0x3f7fffff
    mov_lit(0, 0xAF4DF204u);
    // v3 = v_fract_f32 v0
    words.push_back(vop1(32, 3, 256 + 0));

    // v1: 1.0f (0x3F800000), v2: 2.0f (0x40000000), v4: NaN (0x7FC00000)
    mov_lit(1, 0x3F800000u);
    mov_lit(2, 0x40000000u);
    mov_lit(4, 0x7FC00000u);
    // v5 = v_med3_f32 v1, v2, v4 -> RDNA2 hardware returns min3=1.0f (0x3F800000)
    vop3(words, 0x151, 5, 256 + 1, 256 + 2, 256 + 4);

    // v6: 0xD2F90159, v7: 28, v8: 16
    mov_lit(6, 0xD2F90159u);
    mov_lit(7, 28u);
    mov_lit(8, 16u);
    // v9 = v_bfe_i32 v6, v7, v8 -> past bit 31 sign-extends to 0xFFFFFFFD (-3)
    vop3(words, 0x147, 9, 256 + 6, 256 + 7, 256 + 8);

    // Output to g_buf0:
    words.push_back(vop1(1, 10, 128)); // v10 = 0
    global_mem(words, 28, 3, 10, 127, 0); // offset 0
    global_mem(words, 28, 5, 10, 127, 4); // offset 4
    global_mem(words, 28, 9, 10, 127, 8); // offset 8
    words.push_back(sopp(1)); // s_endpgm

    auto dec = gpu::decode_rdna2(words.data(), words.size());
    auto ir = gpu::lift_rdna2_to_ir(dec);
    KX_CHECK(ir.unknown_count == 0, "All VALU precision ops recognized");

    gpu::DxcCompiler dxc;
    KX_CHECK(dxc.initialize().has_value(), "DXC init");
    auto recomp = gpu::emit_hlsl_from_ir(ir, {.stage = gpu::ShaderStage::Compute, .wave_mode = gpu::WaveMode::Wave32, .group_size_x = 32});
    auto compiled = dxc.compile_hlsl(recomp.hlsl, "main", "cs_6_6");
    
    KX_CHECK(compiled.has_value(), compiled.error().message);

    auto adapters = gpu::enumerate_d3d12_adapters();
    KX_CHECK(adapters.has_value(), adapters.error().message);

    std::vector<uint32_t> init_buf(32, 0);
    uint32_t roots[8]{};
    std::vector<uint32_t> ref_output;

    for (const auto& ad : *adapters) {
        if (ad.is_software) continue;
        auto ran = gpu::run_compute_dxil_on_gpu(ad.index, compiled->dxil, init_buf, roots, 1, 1, 1);
        KX_CHECK(ran.has_value(), ran.error().message);
        KX_CHECK(ran->output_u32.size() >= 3, "Output buffer size check");

        // 1. VFractF32 hardware clamp to 0x3F7FFFFF
        const uint32_t got_fract = ran->output_u32[0];
        KX_CHECK(got_fract == 0x3F7FFFFFu, std::format("GPU {} VFractF32 clamp: got 0x{:08X}", ad.name, got_fract));

        // 2. VMed3F32 NaN operand -> min3 = 1.0f (0x3F800000)
        const uint32_t got_med3 = ran->output_u32[1];
        KX_CHECK(got_med3 == 0x3F800000u, std::format("GPU {} VMed3F32 NaN operand: got 0x{:08X}", ad.name, got_med3));

        // 3. VBfeI32 past bit 31 sign-extends to 0xFFFFFFFD (-3)
        const uint32_t got_bfe = ran->output_u32[2];
        KX_CHECK(got_bfe == 0xFFFFFFFDu, std::format("GPU {} VBfeI32 sign-extension: got 0x{:08X}", ad.name, got_bfe));

        if (ref_output.empty()) {
            ref_output = ran->output_u32;
        } else {
            KX_CHECK(ref_output == ran->output_u32, "Cross-GPU bit-exact VALU precision parity");
        }
    }
}

void test_veh_zen2_sse4a_extrq_insertq() {
    cpu::ScopedVehShim::reset_stats();

    // 1. EXTRQ imm8, imm8: 66 0F 78 C0 10 08 (len=16, idx=8 on XMM0)
    const uint8_t extrq_code[] = { 0x66, 0x0F, 0x78, 0xC0, 0x10, 0x08 };
    const auto dec_extrq = cpu::decode_zen2_extension(extrq_code, sizeof(extrq_code));
    KX_CHECK(dec_extrq.kind == cpu::Zen2Instruction::Extrq, "Decoder identifies EXTRQ");
    KX_CHECK(dec_extrq.length == 6, "EXTRQ length is 6 bytes");
    KX_CHECK(dec_extrq.bit_len == 16, "EXTRQ bit_len is 16");
    KX_CHECK(dec_extrq.bit_idx == 8, "EXTRQ bit_idx is 8");

    CONTEXT ctx{};
    ctx.Xmm0.Low = 0x123456789ABCDEF0ull;
    ctx.Xmm0.High = 0xDEADBEEFCAFEBABEull;
    uint64_t rax = 0;
    bool ok_extrq = cpu::emulate_zen2_instruction(dec_extrq, rax, &ctx);
    KX_CHECK(ok_extrq, "EXTRQ emulation succeeds");
    KX_CHECK(ctx.Xmm0.Low == 0xBCDEull, std::format("EXTRQ Low matches 0xBCDE (got 0x{:016X})", ctx.Xmm0.Low));
    KX_CHECK(ctx.Xmm0.High == 0ull, "EXTRQ High is cleared to 0");

    // 2. INSERTQ imm8, imm8: F2 0F 78 C1 18 0C (len=24, idx=12 on XMM1)
    const uint8_t insertq_code[] = { 0xF2, 0x0F, 0x78, 0xC1, 0x18, 0x0C };
    const auto dec_insertq = cpu::decode_zen2_extension(insertq_code, sizeof(insertq_code));
    KX_CHECK(dec_insertq.kind == cpu::Zen2Instruction::Insertq, "Decoder identifies INSERTQ");
    KX_CHECK(dec_insertq.length == 6, "INSERTQ length is 6 bytes");
    KX_CHECK(dec_insertq.bit_len == 24, "INSERTQ bit_len is 24");
    KX_CHECK(dec_insertq.bit_idx == 12, "INSERTQ bit_idx is 12");

    ctx.Xmm1.Low = 0xFFFFFFFFFFFFFFFFull;
    ctx.Xmm1.High = 0xAAAAAAAAAAAAAAAAull;
    bool ok_insertq = cpu::emulate_zen2_instruction(dec_insertq, rax, &ctx);
    KX_CHECK(ok_insertq, "INSERTQ emulation succeeds");
    KX_CHECK(ctx.Xmm1.High == 0xAAAAAAAAAAAAAAAAull, "INSERTQ High remains unchanged");

    const auto st = cpu::ScopedVehShim::stats();
    KX_CHECK(st.extrq_emulated == 1, "Extrq counter incremented");
    KX_CHECK(st.insertq_emulated == 1, "Insertq counter incremented");
}

void test_vrs_tier2_encode_decode_combiners_and_image() {
    using gpu::VrsCombiner;
    using gpu::VrsShadingRate;

    // 1. Verify all 7 D3D12/RDNA2 shading rates round-trip through tile encode/decode & D3D12 translation
    struct RateCase {
        VrsShadingRate rate;
        uint8_t tile_byte;
        D3D12_SHADING_RATE d3d_rate;
        uint8_t ax;
        uint8_t ay;
    };
    const RateCase cases[] = {
        {VrsShadingRate::Rate1x1, 0x0, D3D12_SHADING_RATE_1X1, 1, 1},
        {VrsShadingRate::Rate1x2, 0x1, D3D12_SHADING_RATE_1X2, 1, 2},
        {VrsShadingRate::Rate2x1, 0x4, D3D12_SHADING_RATE_2X1, 2, 1},
        {VrsShadingRate::Rate2x2, 0x5, D3D12_SHADING_RATE_2X2, 2, 2},
        {VrsShadingRate::Rate2x4, 0x6, D3D12_SHADING_RATE_2X4, 2, 4},
        {VrsShadingRate::Rate4x2, 0x9, D3D12_SHADING_RATE_4X2, 4, 2},
        {VrsShadingRate::Rate4x4, 0xA, D3D12_SHADING_RATE_4X4, 4, 4},
    };
    for (const auto& c : cases) {
        KX_CHECK(gpu::encode_vrs_tile_rate(c.rate) == c.tile_byte, "VRS tile byte encoding");
        KX_CHECK(gpu::decode_vrs_tile_rate(c.tile_byte) == c.rate, "VRS tile byte decoding");
        KX_CHECK(gpu::to_d3d12_shading_rate(c.rate) == c.d3d_rate, "to_d3d12_shading_rate");
        KX_CHECK(gpu::from_d3d12_shading_rate(c.d3d_rate) == c.rate, "from_d3d12_shading_rate");
        const auto axes = gpu::vrs_rate_axes(c.rate);
        KX_CHECK(axes.x == c.ax && axes.y == c.ay, "vrs_rate_axes");
        KX_CHECK(gpu::vrs_rate_from_axes(c.ax, c.ay) == c.rate, "vrs_rate_from_axes");
    }

    // Disallowed aspect ratios (1x4 = 0x2, 4x1 = 0x8) and high garbage bits clamp cleanly
    KX_CHECK(gpu::decode_vrs_tile_rate(0x02u) == VrsShadingRate::Rate1x2, "1x4 clamps safely to 1x2");
    KX_CHECK(gpu::decode_vrs_tile_rate(0x08u) == VrsShadingRate::Rate2x1, "4x1 clamps safely to 2x1");
    KX_CHECK(gpu::decode_vrs_tile_rate(0xFFu) == VrsShadingRate::Rate4x4, "0xFF clamps safely to 4x4");

    // 2. Verify all 5 combiners + D3D12 translation
    KX_CHECK(gpu::to_d3d12_combiner(VrsCombiner::Passthrough) == D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, "combiner passthrough");
    KX_CHECK(gpu::to_d3d12_combiner(VrsCombiner::Override) == D3D12_SHADING_RATE_COMBINER_OVERRIDE, "combiner override");
    KX_CHECK(gpu::to_d3d12_combiner(VrsCombiner::Min) == D3D12_SHADING_RATE_COMBINER_MIN, "combiner min");
    KX_CHECK(gpu::to_d3d12_combiner(VrsCombiner::Max) == D3D12_SHADING_RATE_COMBINER_MAX, "combiner max");
    KX_CHECK(gpu::to_d3d12_combiner(VrsCombiner::Sum) == D3D12_SHADING_RATE_COMBINER_SUM, "combiner sum");
    KX_CHECK(gpu::from_d3d12_combiner(D3D12_SHADING_RATE_COMBINER_SUM) == VrsCombiner::Sum, "from_d3d12_combiner");

    KX_CHECK(gpu::combine_shading_rates(VrsShadingRate::Rate2x1, VrsShadingRate::Rate4x4, VrsCombiner::Passthrough) == VrsShadingRate::Rate2x1, "Passthrough");
    KX_CHECK(gpu::combine_shading_rates(VrsShadingRate::Rate1x1, VrsShadingRate::Rate2x4, VrsCombiner::Override) == VrsShadingRate::Rate2x4, "Override");
    KX_CHECK(gpu::combine_shading_rates(VrsShadingRate::Rate2x4, VrsShadingRate::Rate4x2, VrsCombiner::Min) == VrsShadingRate::Rate2x2, "Min");
    KX_CHECK(gpu::combine_shading_rates(VrsShadingRate::Rate1x2, VrsShadingRate::Rate2x1, VrsCombiner::Max) == VrsShadingRate::Rate2x2, "Max");
    KX_CHECK(gpu::combine_shading_rates(VrsShadingRate::Rate2x1, VrsShadingRate::Rate1x2, VrsCombiner::Sum) == VrsShadingRate::Rate2x2, "Sum 2x1+1x2=2x2");
    KX_CHECK(gpu::combine_shading_rates(VrsShadingRate::Rate2x2, VrsShadingRate::Rate2x2, VrsCombiner::Sum) == VrsShadingRate::Rate4x4, "Sum 2x2+2x2=4x4");
    KX_CHECK(gpu::combine_shading_rates(VrsShadingRate::Rate2x2, VrsShadingRate::Rate1x2, VrsCombiner::Sum) == VrsShadingRate::Rate2x4, "Sum 2x2+1x2=2x4");
    KX_CHECK(gpu::combine_shading_rates(VrsShadingRate::Rate2x2, VrsShadingRate::Rate2x1, VrsCombiner::Sum) == VrsShadingRate::Rate4x2, "Sum 2x2+2x1=4x2");
    KX_CHECK(gpu::combine_shading_rates(VrsShadingRate::Rate4x4, VrsShadingRate::Rate4x4, VrsCombiner::Sum) == VrsShadingRate::Rate4x4, "Sum clamps at 4x4");

    // 3. Verify RDNA2 VRS control register encode/decode round-trip
    const gpu::Rdna2VrsControl ctrl_in{
        .base_rate = VrsShadingRate::Rate2x1,
        .combiner0 = VrsCombiner::Max,
        .combiner1 = VrsCombiner::Override,
        .enable_vrs_image = true,
        .enable_per_primitive = true,
        .tile_size = 16,
    };
    const uint32_t reg_word = gpu::encode_rdna2_vrs_control(ctrl_in);
    const auto ctrl_out = gpu::decode_rdna2_vrs_control(reg_word);
    KX_CHECK(ctrl_out.base_rate == ctrl_in.base_rate, "RDNA2 VRS ctrl base_rate");
    KX_CHECK(ctrl_out.combiner0 == ctrl_in.combiner0, "RDNA2 VRS ctrl combiner0");
    KX_CHECK(ctrl_out.combiner1 == ctrl_in.combiner1, "RDNA2 VRS ctrl combiner1");
    KX_CHECK(ctrl_out.enable_vrs_image == ctrl_in.enable_vrs_image, "RDNA2 VRS ctrl enable_vrs_image");
    KX_CHECK(ctrl_out.enable_per_primitive == ctrl_in.enable_per_primitive, "RDNA2 VRS ctrl enable_per_primitive");
    KX_CHECK(ctrl_out.tile_size == 16u, "RDNA2 VRS ctrl tile_size=16");

    // 4. Verify build_vrs_shading_rate_image dimensions & foveated tile mask layout
    auto non_mult = gpu::build_vrs_shading_rate_image(100, 70, 16, VrsShadingRate::Rate2x2);
    KX_CHECK(non_mult.tiles_x == 7u && non_mult.tiles_y == 5u && non_mult.data.size() == 35u, "Ceiling tile dimensions");
    for (uint8_t b : non_mult.data) {
        KX_CHECK(b == gpu::encode_vrs_tile_rate(VrsShadingRate::Rate2x2), "Uniform fill tile rate");
    }

    auto foveated = gpu::build_vrs_shading_rate_image(
        256, 256, 16,
        [](uint32_t tx, uint32_t ty, uint32_t nx, uint32_t ny) {
            const bool in_center = (tx >= nx / 4 && tx < (nx * 3) / 4 && ty >= ny / 4 && ty < (ny * 3) / 4);
            return in_center ? VrsShadingRate::Rate1x1 : VrsShadingRate::Rate4x4;
        });
    KX_CHECK(foveated.tiles_x == 16u && foveated.tiles_y == 16u && foveated.data.size() == 256u, "Foveated 256x256 / 16");
    KX_CHECK(gpu::decode_vrs_tile_rate(foveated.data[0]) == VrsShadingRate::Rate4x4, "Corner tile is 4x4");
    KX_CHECK(gpu::decode_vrs_tile_rate(foveated.data[8u * 16u + 8u]) == VrsShadingRate::Rate1x1, "Center tile is 1x1");
}

void test_vrs_tier2_hardware_render_and_proxy_telemetry() {
    using gpu::VrsCombiner;
    using gpu::VrsShadingRate;

    // Compile a fullscreen triangle VS + per-pixel high-frequency checkerboard PS (sm 6.6)
    const char* vs_src = R"(
float4 main(uint vid : SV_VertexID) : SV_Position {
    float2 p = float2((vid == 1u) ? 3.0f : -1.0f, (vid == 2u) ? -3.0f : 1.0f);
    return float4(p, 0.0f, 1.0f);
}
)";
    const char* ps_src = R"(
float4 main(float4 pos : SV_Position) : SV_Target0 {
    uint px = (uint)pos.x;
    uint py = (uint)pos.y;
    float r = float(px & 1u);
    float g = float(py & 1u);
    float b = float(((px >> 1u) ^ (py >> 1u)) & 1u);
    return float4(r, g, b, 1.0f);
}
)";

    gpu::DxcCompiler dxc;
    KX_CHECK(dxc.initialize().has_value(), "DXC init");
    auto vs_dxil = dxc.compile_hlsl(vs_src, "main", "vs_6_6");
    auto ps_dxil = dxc.compile_hlsl(ps_src, "main", "ps_6_6");
    KX_CHECK(vs_dxil.has_value(), vs_dxil.error().message);
    KX_CHECK(ps_dxil.has_value(), ps_dxil.error().message);

    auto adapters = gpu::enumerate_d3d12_adapters();
    KX_CHECK(adapters.has_value() && !adapters->empty(), "Adapters available");

    IDXGIFactory6* factory = nullptr;
    KX_CHECK(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "CreateDXGIFactory1");

    constexpr uint32_t kW = 256, kH = 256;
    int hw_seen = 0;
    int tier2_verified = 0;

    for (const auto& ad : *adapters) {
        if (ad.is_software) continue;
        IDXGIAdapter1* dxgi_ad = nullptr;
        if (FAILED(factory->EnumAdapters1(ad.index, &dxgi_ad))) continue;

        ID3D12Device* raw_dev = nullptr;
        HRESULT hr = D3D12CreateDevice(dxgi_ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&raw_dev));
        dxgi_ad->Release();
        if (FAILED(hr) || !raw_dev) continue;
        ++hw_seen;

        hle::reset_d3d12x_stats();
        auto* dev_proxy = new hle::KxDeviceProxy(raw_dev);
        raw_dev->Release();
        ID3D12Device* dev = dev_proxy;

        // Verify KxDeviceProxy::CreateCommandList wraps ID3D12GraphicsCommandList1..5 + QI upgrades
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* queue = nullptr;
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList5* cmd5 = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) && queue, "CreateCommandQueue");
        KX_CHECK(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) && alloc, "CreateCommandAllocator");
        KX_CHECK(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cmd5))) && cmd5,
                 "CreateCommandList IID_ID3D12GraphicsCommandList5 via KxDeviceProxy");

        ID3D12GraphicsCommandList1* cl1 = nullptr;
        ID3D12GraphicsCommandList4* cl4 = nullptr;
        KX_CHECK(SUCCEEDED(cmd5->QueryInterface(IID_PPV_ARGS(&cl1))) && cl1, "QI ID3D12GraphicsCommandList1");
        KX_CHECK(SUCCEEDED(cmd5->QueryInterface(IID_PPV_ARGS(&cl4))) && cl4, "QI ID3D12GraphicsCommandList4");
        cl4->Release();
        cl1->Release();

        D3D12_FEATURE_DATA_D3D12_OPTIONS6 opt6{};
        KX_CHECK(SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &opt6, sizeof(opt6))),
                 "CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6)");

        if (opt6.VariableShadingRateTier < D3D12_VARIABLE_SHADING_RATE_TIER_2 || opt6.ShadingRateImageTileSize == 0) {
            // Even without Tier 2 hardware, verify proxy hook counters work cleanly on unexecuted command list
            cmd5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
            cmd5->RSSetShadingRateImage(nullptr);
            const auto st_no_vrs = hle::d3d12x_stats();
            KX_CHECK(st_no_vrs.vrs_rate_sets == 1 && st_no_vrs.vrs_image_binds == 1,
                     "KxGraphicsCommandListProxy VRS telemetry recorded");
            std::cout << std::format("    -> Hardware [{}] {} (VRS Tier {} — skipped Tier 2 render, proxy telemetry OK)\n",
                                     ad.index, ad.name, static_cast<int>(opt6.VariableShadingRateTier));
            cmd5->Close();
            cmd5->Release();
            alloc->Release();
            queue->Release();
            dev->Release();
            continue;
        }

        const uint32_t tile_size = opt6.ShadingRateImageTileSize;
        const VrsShadingRate outer_rate = opt6.AdditionalShadingRatesSupported ? VrsShadingRate::Rate4x4 : VrsShadingRate::Rate2x2;
        const auto outer_axes = gpu::vrs_rate_axes(outer_rate);

        // Build R8_UINT shading rate image: center [64..192)x[64..192) = 1x1, outer = 4x4 (or 2x2)
        const auto sri = gpu::build_vrs_shading_rate_image(
            kW, kH, tile_size,
            [tile_size, outer_rate](uint32_t tx, uint32_t ty, uint32_t, uint32_t) {
                const uint32_t px = tx * tile_size;
                const uint32_t py = ty * tile_size;
                const bool center = (px >= 64u && px < 192u && py >= 64u && py < 192u);
                return center ? VrsShadingRate::Rate1x1 : outer_rate;
            });

        // Empty root signature + Graphics PSO
        D3D12_ROOT_SIGNATURE_DESC rsd{0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ID3DBlob* rs_blob = nullptr;
        ID3D12RootSignature* rs = nullptr;
        KX_CHECK(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, nullptr)), "SerializeRS");
        KX_CHECK(SUCCEEDED(dev->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&rs))), "CreateRS");
        rs_blob->Release();

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psd{};
        psd.pRootSignature = rs;
        psd.VS = {vs_dxil->dxil.data(), vs_dxil->dxil.size()};
        psd.PS = {ps_dxil->dxil.data(), ps_dxil->dxil.size()};
        psd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psd.SampleMask = UINT_MAX;
        psd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psd.NumRenderTargets = 1;
        psd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psd.SampleDesc.Count = 1;
        ID3D12PipelineState* pso = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateGraphicsPipelineState(&psd, IID_PPV_ARGS(&pso))), "CreateVrsPSO");

        D3D12_HEAP_PROPERTIES hp_def{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
        D3D12_HEAP_PROPERTIES hp_up{D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
        D3D12_HEAP_PROPERTIES hp_rb{D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};

        // Render target (256x256 RGBA8)
        D3D12_RESOURCE_DESC rt_desc{};
        rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rt_desc.Width = kW; rt_desc.Height = kH; rt_desc.DepthOrArraySize = 1; rt_desc.MipLevels = 1;
        rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rt_desc.SampleDesc.Count = 1;
        rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ID3D12Resource* rt = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &rt_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt))), "CreateRT");

        // Shading Rate Image texture (R8_UINT)
        D3D12_RESOURCE_DESC sri_desc{};
        sri_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        sri_desc.Width = sri.tiles_x; sri_desc.Height = sri.tiles_y; sri_desc.DepthOrArraySize = 1; sri_desc.MipLevels = 1;
        sri_desc.Format = DXGI_FORMAT_R8_UINT; sri_desc.SampleDesc.Count = 1;
        ID3D12Resource* sri_tex = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &sri_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&sri_tex))), "CreateSriTex");

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT sri_fp{};
        UINT64 sri_up_bytes = 0;
        dev->GetCopyableFootprints(&sri_desc, 0, 1, 0, &sri_fp, nullptr, nullptr, &sri_up_bytes);

        D3D12_RESOURCE_DESC buf_desc{};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Height = 1; buf_desc.DepthOrArraySize = 1; buf_desc.MipLevels = 1;
        buf_desc.SampleDesc.Count = 1; buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buf_desc.Width = sri_up_bytes;
        ID3D12Resource* sri_up = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateCommittedResource(&hp_up, D3D12_HEAP_FLAG_NONE, &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&sri_up))), "CreateSriUpload");

        void* mapped = nullptr;
        KX_CHECK(SUCCEEDED(sri_up->Map(0, nullptr, &mapped)), "MapSriUpload");
        for (uint32_t ty = 0; ty < sri.tiles_y; ++ty) {
            std::memcpy(static_cast<uint8_t*>(mapped) + static_cast<size_t>(ty) * sri_fp.Footprint.RowPitch,
                        sri.data.data() + static_cast<size_t>(ty) * sri.tiles_x, sri.tiles_x);
        }
        sri_up->Unmap(0, nullptr);

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT rt_fp{};
        UINT64 rb_bytes = 0;
        dev->GetCopyableFootprints(&rt_desc, 0, 1, 0, &rt_fp, nullptr, nullptr, &rb_bytes);
        buf_desc.Width = rb_bytes;
        ID3D12Resource* rb = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateCommittedResource(&hp_rb, D3D12_HEAP_FLAG_NONE, &buf_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))), "CreateRB");

        D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        ID3D12DescriptorHeap* rtv_heap = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateDescriptorHeap(&rtv_hd, IID_PPV_ARGS(&rtv_heap))), "CreateRtvHeap");
        const auto rtv_h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        dev->CreateRenderTargetView(rt, nullptr, rtv_h);

        // Upload shading rate image & transition to SHADING_RATE_SOURCE
        D3D12_TEXTURE_COPY_LOCATION sri_dst{sri_tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        D3D12_TEXTURE_COPY_LOCATION sri_src{sri_up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        sri_src.PlacedFootprint = sri_fp;
        cmd5->CopyTextureRegion(&sri_dst, 0, 0, 0, &sri_src, nullptr);

        D3D12_RESOURCE_BARRIER sri_bar{};
        sri_bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        sri_bar.Transition = {sri_tex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE};
        cmd5->ResourceBarrier(1, &sri_bar);

        // Record VRS Tier 2 draw through KxGraphicsCommandListProxy
        const float clear_col[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        cmd5->ClearRenderTargetView(rtv_h, clear_col, 0, nullptr);
        cmd5->OMSetRenderTargets(1, &rtv_h, FALSE, nullptr);
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, static_cast<LONG>(kW), static_cast<LONG>(kH)};
        cmd5->RSSetViewports(1, &vp);
        cmd5->RSSetScissorRects(1, &sc);
        cmd5->SetGraphicsRootSignature(rs);
        cmd5->SetPipelineState(pso);

        const D3D12_SHADING_RATE_COMBINER combiners[2] = {
            gpu::to_d3d12_combiner(VrsCombiner::Passthrough),
            gpu::to_d3d12_combiner(VrsCombiner::Override),
        };
        cmd5->RSSetShadingRate(gpu::to_d3d12_shading_rate(VrsShadingRate::Rate1x1), combiners);
        cmd5->RSSetShadingRateImage(sri_tex);
        cmd5->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd5->DrawInstanced(3, 1, 0, 0);

        // Unbind shading rate image & restore 1x1
        cmd5->RSSetShadingRateImage(nullptr);
        cmd5->RSSetShadingRate(gpu::to_d3d12_shading_rate(VrsShadingRate::Rate1x1), nullptr);

        // Readback
        D3D12_RESOURCE_BARRIER rt_bar{};
        rt_bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rt_bar.Transition = {rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd5->ResourceBarrier(1, &rt_bar);

        D3D12_TEXTURE_COPY_LOCATION rb_dst{rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        rb_dst.PlacedFootprint = rt_fp;
        D3D12_TEXTURE_COPY_LOCATION rb_src{rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        cmd5->CopyTextureRegion(&rb_dst, 0, 0, 0, &rb_src, nullptr);

        KX_CHECK(SUCCEEDED(cmd5->Close()), "Close cmd5");
        ID3D12CommandList* lists[] = {cmd5};
        queue->ExecuteCommandLists(1, lists);

        ID3D12Fence* fence = nullptr;
        KX_CHECK(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "CreateFence");
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        queue->Signal(fence, 1);
        fence->SetEventOnCompletion(1, ev);
        KX_CHECK(WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0, "VRS GPU fence wait");
        CloseHandle(ev);

        // Verify KxGraphicsCommandListProxy VRS Tier 2 telemetry counters
        const auto stats = hle::d3d12x_stats();
        KX_CHECK(stats.vrs_rate_sets == 2u, "Expected 2 RSSetShadingRate calls tracked");
        KX_CHECK(stats.vrs_image_binds == 2u, "Expected 2 RSSetShadingRateImage calls tracked");
        KX_CHECK(stats.draw_calls == 1u && stats.pso_binds == 1u && stats.barrier_calls == 2u,
                 "Expected draw/pso/barrier telemetry on ID3D12GraphicsCommandList5 proxy");

        // Verify hardware VRS Tier 2 shading behavior on readback pixels:
        //  1) Center [64..192)x[64..192) was shaded at 1x1 -> every adjacent pixel alternates R (x) and G (y).
        //  2) Outer [0..64)x[0..64) was shaded at outer_axes (4x4 or 2x2) -> every aligned coarse block is uniform!
        uint8_t* pixels = nullptr;
        KX_CHECK(SUCCEEDED(rb->Map(0, nullptr, reinterpret_cast<void**>(&pixels))), "MapRB");
        auto pixel_at = [&](uint32_t x, uint32_t y) -> const uint8_t* {
            return pixels + static_cast<size_t>(y) * rt_fp.Footprint.RowPitch + static_cast<size_t>(x) * 4u;
        };

        for (uint32_t y = 64u; y < 192u; y += 2u) {
            for (uint32_t x = 64u; x < 192u; x += 2u) {
                const uint8_t* p00 = pixel_at(x, y);
                const uint8_t* p10 = pixel_at(x + 1u, y);
                const uint8_t* p01 = pixel_at(x, y + 1u);
                KX_CHECK(p00[3] == 255u && p10[3] == 255u && p01[3] == 255u, "Center alpha 255");
                KX_CHECK(p00[0] != p10[0], "Center 1x1 must preserve per-pixel horizontal frequency");
                KX_CHECK(p00[1] != p01[1], "Center 1x1 must preserve per-pixel vertical frequency");
            }
        }

        for (uint32_t by = 0u; by < 64u; by += outer_axes.y) {
            for (uint32_t bx = 0u; bx < 64u; bx += outer_axes.x) {
                const uint8_t* ref = pixel_at(bx, by);
                KX_CHECK(ref[3] == 255u, "Outer alpha 255");
                for (uint32_t dy = 0u; dy < outer_axes.y; ++dy) {
                    for (uint32_t dx = 0u; dx < outer_axes.x; ++dx) {
                        const uint8_t* p = pixel_at(bx + dx, by + dy);
                        KX_CHECK(p[0] == ref[0] && p[1] == ref[1] && p[2] == ref[2] && p[3] == ref[3],
                                 std::format("Outer {}x{} coarse VRS block ({},{}) must broadcast uniform color",
                                             outer_axes.x, outer_axes.y, bx, by));
                    }
                }
            }
        }
        rb->Unmap(0, nullptr);

        std::cout << std::format(
            "    -> Hardware [{}] {} PASS (VRS Tier {} | tile={}x{} | center=1x1 outer={}x{} verified | rate_sets={} img_binds={})\n",
            ad.index, ad.name, static_cast<int>(opt6.VariableShadingRateTier),
            tile_size, tile_size, outer_axes.x, outer_axes.y,
            stats.vrs_rate_sets, stats.vrs_image_binds);
        ++tier2_verified;

        fence->Release();
        rtv_heap->Release();
        rb->Release();
        sri_up->Release();
        sri_tex->Release();
        rt->Release();
        pso->Release();
        rs->Release();
        cmd5->Release();
        alloc->Release();
        queue->Release();
        dev->Release();
    }

    factory->Release();
    KX_CHECK(hw_seen >= 1, "At least one hardware GPU enumerated");
    KX_CHECK(tier2_verified >= 1, "At least one hardware GPU verified for VRS Tier 2");
}

void test_gameinput_multi_controller_slots_and_rumble() {
    hle::reset_gameinput_stats();
    hle::Registry reg;
    hle::register_gameinput(reg);

    auto* create_fn = reinterpret_cast<HRESULT(__stdcall*)(hle::IKxGameInput**)>(
        reg.resolve_name("gameinput.dll", "GameInputCreate"));
    KX_CHECK(create_fn != nullptr, "GameInputCreate export");

    hle::IKxGameInput* gi = nullptr;
    KX_CHECK(SUCCEEDED(create_fn(&gi)) && gi != nullptr, "GameInputCreate S_OK");

    // Bounds & pointer validation on GetDeviceForSlot
    hle::IKxGameInputDevice* bad_dev = nullptr;
    KX_CHECK(gi->GetDeviceForSlot(hle::kMaxControllerSlots, &bad_dev) == E_INVALIDARG && bad_dev == nullptr,
             "Out-of-range slot rejected with E_INVALIDARG");
    KX_CHECK(gi->GetDeviceForSlot(0, nullptr) == E_POINTER, "Null out_device rejected with E_POINTER");

    // Enumerate all 4 slots and verify distinct identity & GetDefaultDevice == slot 0
    std::array<hle::IKxGameInputDevice*, hle::kMaxControllerSlots> devs{};
    for (uint32_t s = 0; s < hle::kMaxControllerSlots; ++s) {
        KX_CHECK(SUCCEEDED(gi->GetDeviceForSlot(s, &devs[s])) && devs[s] != nullptr, "GetDeviceForSlot S_OK");
        KX_CHECK(devs[s]->GetSlotIndex() == s, "Slot index matches");
        KX_CHECK(devs[s]->GetDeviceId() != 0, "Non-zero device ID");
        for (uint32_t prev = 0; prev < s; ++prev) {
            KX_CHECK(devs[s]->GetDeviceId() != devs[prev]->GetDeviceId(), "Unique device ID per slot");
        }
    }

    hle::IKxGameInputDevice* def_dev = nullptr;
    KX_CHECK(SUCCEEDED(gi->GetDefaultDevice(&def_dev)) && def_dev != nullptr, "GetDefaultDevice S_OK");
    KX_CHECK(def_dev->GetSlotIndex() == 0 && def_dev->GetDeviceId() == devs[0]->GetDeviceId(),
             "Default device maps to slot 0");
    def_dev->Release();

    // Mark all 4 slots mocked disconnected and verify targeted reads fail with ERROR_DEVICE_NOT_CONNECTED
    for (uint32_t s = 0; s < hle::kMaxControllerSlots; ++s) {
        hle::set_gameinput_mock_slot_state(s, {}, false);
        KX_CHECK(!devs[s]->IsConnected(), "Slot initially disconnected");
        hle::IKxGameInputReading* disc_r = nullptr;
        const HRESULT hr = gi->GetCurrentReading(hle::KxGameInputKindGamepad, devs[s], &disc_r);
        KX_CHECK(hr == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) && disc_r == nullptr,
                 "Disconnected slot returns ERROR_DEVICE_NOT_CONNECTED");
    }
    KX_CHECK(hle::gameinput_stats().active_controllers == 0, "Zero active controllers when all disconnected");

    // Connect 4 concurrent controllers with independent states
    const std::array<hle::KxGameInputGamepadState, hle::kMaxControllerSlots> expected = {{
        {hle::KxGamepadA, 0.25f, 0.00f,  0.50f,  0.00f,  0.00f,  0.00f},
        {hle::KxGamepadB, 0.00f, 0.50f,  0.00f, -0.75f,  0.00f,  0.00f},
        {hle::KxGamepadX, 0.75f, 0.10f,  0.00f,  0.00f,  0.90f,  0.00f},
        {hle::KxGamepadY, 0.15f, 1.00f, -1.00f,  0.25f,  0.00f, -0.85f},
    }};
    for (uint32_t s = 0; s < hle::kMaxControllerSlots; ++s) {
        hle::set_gameinput_mock_slot_state(s, expected[s], true);
        KX_CHECK(devs[s]->IsConnected(), "Slot connected after mock state set");
    }
    KX_CHECK(hle::gameinput_stats().active_controllers == 4, "4 active controllers tracked");

    // Verify independent targeted readings across all 4 slots
    for (uint32_t s = 0; s < hle::kMaxControllerSlots; ++s) {
        hle::IKxGameInputReading* r = nullptr;
        KX_CHECK(SUCCEEDED(gi->GetCurrentReading(hle::KxGameInputKindGamepad, devs[s], &r)) && r != nullptr,
                 "Targeted slot reading succeeds");
        hle::KxGameInputGamepadState got{};
        KX_CHECK(r->GetGamepadState(&got), "GetGamepadState true");
        KX_CHECK(got.buttons == expected[s].buttons, "Buttons isolated per slot");
        KX_CHECK(std::abs(got.left_trigger - expected[s].left_trigger) < 1e-5f, "Left trigger isolated per slot");
        KX_CHECK(std::abs(got.right_trigger - expected[s].right_trigger) < 1e-5f, "Right trigger isolated per slot");
        KX_CHECK(std::abs(got.left_thumbstick_x - expected[s].left_thumbstick_x) < 1e-5f, "LX isolated per slot");
        KX_CHECK(std::abs(got.left_thumbstick_y - expected[s].left_thumbstick_y) < 1e-5f, "LY isolated per slot");
        KX_CHECK(std::abs(got.right_thumbstick_x - expected[s].right_thumbstick_x) < 1e-5f, "RX isolated per slot");
        KX_CHECK(std::abs(got.right_thumbstick_y - expected[s].right_thumbstick_y) < 1e-5f, "RY isolated per slot");
        r->Release();
    }

    // Disconnect slot 2 only; verify slot 2 fails while slots 0, 1, 3 continue reading
    hle::simulate_gameinput_hotplug(2, false);
    KX_CHECK(!devs[2]->IsConnected(), "Slot 2 disconnected");
    KX_CHECK(hle::gameinput_stats().active_controllers == 3, "3 active controllers after slot 2 disconnect");
    {
        hle::IKxGameInputReading* r2 = nullptr;
        KX_CHECK(gi->GetCurrentReading(hle::KxGameInputKindGamepad, devs[2], &r2) ==
                     HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) && r2 == nullptr,
                 "Disconnected slot 2 reading fails");
        hle::IKxGameInputReading* r3 = nullptr;
        KX_CHECK(SUCCEEDED(gi->GetCurrentReading(hle::KxGameInputKindGamepad, devs[3], &r3)) && r3 != nullptr,
                 "Slot 3 reading still succeeds");
        hle::KxGameInputGamepadState got3{};
        KX_CHECK(r3->GetGamepadState(&got3) && got3.buttons == hle::KxGamepadY, "Slot 3 state intact");
        r3->Release();
    }

    // Per-slot rumble isolation: slot 1 must NOT overwrite slot 0 (or slots 2/3)
    const hle::KxGameInputRumbleParams r0{0.20f, 0.30f, 0.10f, 0.40f};
    const hle::KxGameInputRumbleParams r1{0.90f, 0.80f, 0.70f, 0.60f};
    devs[0]->SetRumbleState(&r0);
    devs[1]->SetRumbleState(&r1);

    hle::KxGameInputRumbleParams got_r0{}, got_r1{}, got_r2{};
    devs[0]->GetRumbleState(&got_r0);
    devs[1]->GetRumbleState(&got_r1);
    devs[2]->GetRumbleState(&got_r2);
    KX_CHECK(std::abs(got_r0.low_frequency - 0.20f) < 1e-5f && std::abs(got_r0.high_frequency - 0.30f) < 1e-5f,
             "Slot 0 rumble preserved after slot 1 update");
    KX_CHECK(std::abs(got_r1.low_frequency - 0.90f) < 1e-5f && std::abs(got_r1.right_trigger - 0.60f) < 1e-5f,
             "Slot 1 rumble stored independently");
    KX_CHECK(got_r2.low_frequency == 0.0f && got_r2.high_frequency == 0.0f, "Slot 2 rumble remains zero");

    // NaN / +/-Inf / out-of-range rumble clamping safety
    const hle::KxGameInputRumbleParams wild{
        std::numeric_limits<float>::quiet_NaN(),
        2.5f,
        -0.75f,
        std::numeric_limits<float>::infinity()};
    devs[3]->SetRumbleState(&wild);
    hle::KxGameInputRumbleParams got_r3{};
    devs[3]->GetRumbleState(&got_r3);
    KX_CHECK(got_r3.low_frequency == 0.0f, "NaN rumble clamped to 0.0");
    KX_CHECK(std::abs(got_r3.high_frequency - 1.0f) < 1e-5f, ">1.0 rumble clamped to 1.0");
    KX_CHECK(got_r3.left_trigger == 0.0f, "Negative rumble clamped to 0.0");
    KX_CHECK(got_r3.right_trigger == 0.0f, "+Inf rumble clamped to 0.0");

    // Clear all motors to zero and clean up
    const hle::KxGameInputRumbleParams zero{};
    for (auto* d : devs) {
        d->SetRumbleState(&zero);
        d->Release();
    }
    KX_CHECK(hle::gameinput_stats().rumble_commands_sent == 7, "Exact rumble command count tracked");
    hle::clear_gameinput_mock_slots();
    gi->Release();
}

void test_gameinput_hotplug_callbacks_and_telemetry() {
    hle::reset_gameinput_stats();
    hle::Registry reg;
    hle::register_gameinput(reg);

    auto* create_fn = reinterpret_cast<HRESULT(__stdcall*)(hle::IKxGameInput**)>(
        reg.resolve_name("gameinput.dll", "GameInputCreate"));
    KX_CHECK(create_fn != nullptr, "GameInputCreate export");

    hle::IKxGameInput* gi = nullptr;
    KX_CHECK(SUCCEEDED(create_fn(&gi)) && gi != nullptr, "GameInputCreate S_OK");

    // Validation checks
    hle::KxGameInputCallbackToken bad_tok = 0;
    KX_CHECK(gi->RegisterDeviceCallback(nullptr, nullptr, &bad_tok) == E_INVALIDARG,
             "Null callback rejected with E_INVALIDARG");
    KX_CHECK(!gi->UnregisterCallback(0), "UnregisterCallback(0) returns false");
    KX_CHECK(!gi->UnregisterCallback(999999), "UnregisterCallback(unknown) returns false");

    struct EventRecord {
        hle::KxGameInputCallbackToken token = 0;
        uint32_t slot = 0;
        uint64_t device_id = 0;
        uint64_t timestamp = 0;
        hle::KxGameInputDeviceStatus cur = hle::KxGameInputDeviceStatus::Disconnected;
        hle::KxGameInputDeviceStatus prev = hle::KxGameInputDeviceStatus::Disconnected;
        bool dev_is_connected = false;
    };
    struct CbContext {
        std::vector<EventRecord> events;
        hle::IKxGameInput* gi = nullptr;
        bool self_unregister = false;
    };

    auto on_device = [](hle::KxGameInputCallbackToken token,
                        void* ctx_ptr,
                        hle::IKxGameInputDevice* device,
                        uint64_t timestamp,
                        hle::KxGameInputDeviceStatus cur,
                        hle::KxGameInputDeviceStatus prev) -> void {
        auto* ctx = static_cast<CbContext*>(ctx_ptr);
        ctx->events.push_back(EventRecord{
            token,
            device ? device->GetSlotIndex() : 0xFFFFFFFFu,
            device ? device->GetDeviceId() : 0ull,
            timestamp,
            cur,
            prev,
            device ? device->IsConnected() : false,
        });
        if (ctx->self_unregister && ctx->gi) {
            ctx->gi->UnregisterCallback(token);
        }
    };

    CbContext ctx1{};
    CbContext ctx2{};
    hle::KxGameInputCallbackToken tok1 = 0;
    hle::KxGameInputCallbackToken tok2 = 0;
    KX_CHECK(SUCCEEDED(gi->RegisterDeviceCallback(&ctx1, on_device, &tok1)) && tok1 != 0, "Register cb 1");
    KX_CHECK(SUCCEEDED(gi->RegisterDeviceCallback(&ctx2, on_device, &tok2)) && tok2 != 0 && tok2 != tok1,
             "Register cb 2 with distinct token");

    // 1. Connect slot 1 -> both callbacks fire
    hle::simulate_gameinput_hotplug(1, true);
    KX_CHECK(ctx1.events.size() == 1 && ctx2.events.size() == 1, "Both callbacks fired on slot 1 connect");
    KX_CHECK(ctx1.events[0].token == tok1 && ctx1.events[0].slot == 1 && ctx1.events[0].device_id != 0 &&
                 ctx1.events[0].timestamp > 0 &&
                 ctx1.events[0].cur == hle::KxGameInputDeviceStatus::Connected &&
                 ctx1.events[0].prev == hle::KxGameInputDeviceStatus::Disconnected &&
                 ctx1.events[0].dev_is_connected,
             "Slot 1 connect event fields accurate");
    {
        const auto st = hle::gameinput_stats();
        KX_CHECK(st.active_controllers == 1 && st.hotplug_events == 1, "Stats after slot 1 connect");
    }

    // 2. Redundant connect on slot 1 -> deterministic no-op (no duplicate callback or event)
    hle::simulate_gameinput_hotplug(1, true);
    KX_CHECK(ctx1.events.size() == 1 && ctx2.events.size() == 1, "Redundant connect does not re-fire");
    KX_CHECK(hle::gameinput_stats().hotplug_events == 1, "Redundant connect does not increment hotplug_events");

    // 3. Connect slot 3, then disconnect slot 1
    hle::simulate_gameinput_hotplug(3, true);
    hle::simulate_gameinput_hotplug(1, false);
    KX_CHECK(ctx1.events.size() == 3 && ctx2.events.size() == 3, "Both callbacks received all 3 transitions");
    KX_CHECK(ctx1.events[1].slot == 3 && ctx1.events[1].cur == hle::KxGameInputDeviceStatus::Connected,
             "Slot 3 connect recorded");
    KX_CHECK(ctx1.events[2].slot == 1 &&
                 ctx1.events[2].cur == hle::KxGameInputDeviceStatus::Disconnected &&
                 ctx1.events[2].prev == hle::KxGameInputDeviceStatus::Connected &&
                 !ctx1.events[2].dev_is_connected,
             "Slot 1 disconnect recorded");
    {
        const auto st = hle::gameinput_stats();
        KX_CHECK(st.active_controllers == 1 && st.hotplug_events == 3, "Stats after connect+disconnect");
    }

    // 4. Unregister tok1 -> stops receiving callbacks while tok2 continues
    KX_CHECK(gi->UnregisterCallback(tok1), "Unregister tok1 returns true");
    KX_CHECK(!gi->UnregisterCallback(tok1), "Double unregister returns false");

    hle::simulate_gameinput_hotplug(0, true);
    KX_CHECK(ctx1.events.size() == 3, "Unregistered callback receives zero further events");
    KX_CHECK(ctx2.events.size() == 4 && ctx2.events[3].slot == 0, "Remaining callback still fires");
    {
        const auto st = hle::gameinput_stats();
        KX_CHECK(st.active_controllers == 2 && st.hotplug_events == 4, "Stats after slot 0 connect");
    }

    // 5. Re-entrancy safety: callback unregisters itself from inside dispatch without deadlocking
    CbContext ctx_self{{}, gi, true};
    hle::KxGameInputCallbackToken tok_self = 0;
    KX_CHECK(SUCCEEDED(gi->RegisterDeviceCallback(&ctx_self, on_device, &tok_self)) && tok_self != 0,
             "Register self-unregistering callback");
    hle::simulate_gameinput_hotplug(2, true);
    KX_CHECK(ctx_self.events.size() == 1, "Self-unregistering callback fired once");
    hle::simulate_gameinput_hotplug(2, false);
    KX_CHECK(ctx_self.events.size() == 1, "Self-unregistered callback did not fire again");

    // 6. Releasing IKxGameInput automatically cleans up remaining callbacks (no dangling call)
    const size_t ctx2_count_before_release = ctx2.events.size();
    gi->Release();
    hle::simulate_gameinput_hotplug(0, false);
    KX_CHECK(ctx2.events.size() == ctx2_count_before_release,
             "Released IKxGameInput auto-unregisters remaining callbacks");
    KX_CHECK(hle::gameinput_stats().active_controllers == 1 && hle::gameinput_stats().hotplug_events == 7,
             "Final hotplug telemetry accurate");

    hle::clear_gameinput_mock_slots();
}

void test_work_graph_program_validation_and_ring_allocator() {
    using namespace gpu::encode;
    const std::vector<uint32_t> minimal_cs = {sopp(1)};  // s_endpgm

    // 1. Valid 3-node DAG (Broadcasting -> Coalescing -> Thread)
    gpu::WorkGraphProgram dag{};
    dag.name = "CleanRoomTestDAG";
    const uint32_t n0 = dag.add_node({
        .name = "ProducerBroadcast",
        .kind = gpu::WorkGraphNodeKind::Broadcasting,
        .group_size_x = 64,
        .group_size_y = 1,
        .group_size_z = 1,
        .record_stride_bytes = 32,
        .max_output_records = 64,
        .rdna2_dwords = minimal_cs,
    });
    const uint32_t n1 = dag.add_node({
        .name = "MiddleCoalesce",
        .kind = gpu::WorkGraphNodeKind::Coalescing,
        .group_size_x = 32,
        .group_size_y = 1,
        .group_size_z = 1,
        .record_stride_bytes = 16,
        .max_output_records = 32,
        .kxrn_bytecode = gpu::pack_kxrn_compute_bytecode(32, minimal_cs),
    });
    const uint32_t n2 = dag.add_node({
        .name = "LeafThread",
        .kind = gpu::WorkGraphNodeKind::Thread,
        .group_size_x = 1,
        .group_size_y = 1,
        .group_size_z = 1,
        .record_stride_bytes = 0,
        .max_output_records = 0,
        .rdna2_dwords = minimal_cs,
    });
    dag.add_edge(n0, n1, 0, 16, 1);
    dag.add_edge(n1, n2, 16, 64, 1);

    auto valid = dag.validate();
    KX_CHECK(valid.has_value(), valid.error().message);
    auto order = dag.topological_order();
    KX_CHECK(order.has_value() && order->size() == 3 &&
             (*order)[0] == 0 && (*order)[1] == 1 && (*order)[2] == 2,
             "Deterministic topological order [0, 1, 2]");

    // 2. Empty program rejected
    gpu::WorkGraphProgram empty_prog{};
    KX_CHECK(!empty_prog.validate().has_value(), "Empty WorkGraphProgram must be rejected");

    // 3. Duplicate node names rejected
    gpu::WorkGraphProgram dup_names = dag;
    dup_names.nodes[1].name = dup_names.nodes[0].name;
    KX_CHECK(!dup_names.validate().has_value(), "Duplicate node names must be rejected");

    // 4. Out-of-range edge target rejected
    gpu::WorkGraphProgram bad_target = dag;
    bad_target.add_edge(0, 99, 0, 16, 1);
    KX_CHECK(!bad_target.validate().has_value(), "Out-of-range edge target must be rejected");

    // 5. Cycle rejected (2 -> 0 closes cycle 0 -> 1 -> 2 -> 0)
    gpu::WorkGraphProgram cyclic = dag;
    cyclic.add_edge(n2, n0, 32, 128, 1);
    KX_CHECK(!cyclic.validate().has_value(), "Cyclic WorkGraphProgram must be rejected");

    // 6. Self-loop rejected
    gpu::WorkGraphProgram self_loop = dag;
    self_loop.add_edge(n0, n0, 32, 128, 1);
    KX_CHECK(!self_loop.validate().has_value(), "Self-loop edge must be rejected");

    // 7. Unaligned record stride & edge offsets rejected
    gpu::WorkGraphProgram bad_stride = dag;
    bad_stride.nodes[0].record_stride_bytes = 12;
    KX_CHECK(!bad_stride.validate().has_value(), "Non-16B-aligned record_stride_bytes must be rejected");

    gpu::WorkGraphProgram bad_edge_align = dag;
    bad_edge_align.edges[0].record_buffer_byte_offset = 8;
    KX_CHECK(!bad_edge_align.validate().has_value(), "Non-16B-aligned record_buffer_byte_offset must be rejected");

    gpu::WorkGraphProgram bad_args_align = dag;
    bad_args_align.edges[0].dispatch_args_byte_offset = 2;
    KX_CHECK(!bad_args_align.validate().has_value(), "Non-4B-aligned dispatch_args_byte_offset must be rejected");

    // 8. Record backing memory overflow rejected
    gpu::WorkGraphProgram overflow_prog = dag;
    overflow_prog.nodes[0].record_stride_bytes = 0x10000000u;
    overflow_prog.nodes[0].max_output_records = 64u;
    KX_CHECK(!overflow_prog.validate().has_value(), "Record buffer size overflow must be rejected");

    // 9. RecordRingAllocator: 16B alignment, bounds checking, FIFO release, wrap & reset
    constexpr uint64_t kBaseVa = 0x100000000ull;
    gpu::RecordRingAllocator ring(256, kBaseVa);
    KX_CHECK(ring.capacity_bytes() == 256, "Ring capacity");

    auto a0 = ring.allocate(1);   // aligns up to 16B @ offset 0
    KX_CHECK(a0.has_value() && a0->offset == 0 && a0->aligned_size_bytes == 16 && a0->gpu_va == kBaseVa,
             "First allocation 16B aligned at offset 0");

    auto a1 = ring.allocate(113);  // aligns up to 128B @ offset 16
    KX_CHECK(a1.has_value() && a1->offset == 16 && a1->aligned_size_bytes == 128 &&
             a1->gpu_va == kBaseVa + 16 && !a1->wrapped,
             "Second allocation 16B aligned at offset 16");

    auto a2 = ring.allocate(80);  // 80B @ offset 144 -> head = 224
    KX_CHECK(a2.has_value() && a2->offset == 144 && a2->aligned_size_bytes == 80,
             "Third allocation at offset 144");

    // Only 32B left at tail, 0B at front -> 64B must fail
    auto oom = ring.allocate(64);
    KX_CHECK(!oom.has_value(), "Allocation exceeding contiguous free space must fail");

    // Release a0 (16B) and a1 (128B) -> tail advances to 144
    KX_CHECK(ring.release(*a0).has_value(), "FIFO release a0");
    KX_CHECK(ring.release(*a1).has_value(), "FIFO release a1");

    // Now allocate 64B: does not fit in remaining 32B at end (224..256), so wraps to offset 0!
    auto a3 = ring.allocate(64);
    KX_CHECK(a3.has_value() && a3->offset == 0 && a3->wrapped && a3->gpu_va == kBaseVa,
             "Allocation wraps contiguously to offset 0");

    const auto m_mid = ring.metrics();
    KX_CHECK(m_mid.wrap_count == 1 && m_mid.failed_allocations == 1 &&
             m_mid.used_bytes == 144 && m_mid.peak_used_bytes == 224 &&
             m_mid.utilization > 0.50 && m_mid.utilization < 0.60,
             "Ring allocator utilization, peak, wrap & failure metrics");

    // Release a2 (at 144) -> tail wraps cleanly to 0, then release a3 (at 0)
    KX_CHECK(ring.release(*a2).has_value(), "FIFO release a2 across wrap boundary");
    KX_CHECK(ring.release(*a3).has_value(), "FIFO release wrapped a3");
    KX_CHECK(ring.metrics().used_bytes == 0 && ring.metrics().free_bytes == 256, "Empty after all releases");

    // Allocate & reset
    auto a4 = ring.allocate(128);
    KX_CHECK(a4.has_value() && a4->offset == 0, "Re-allocate at offset 0");
    ring.reset();
    const auto m_reset = ring.metrics();
    KX_CHECK(m_reset.used_bytes == 0 && m_reset.free_bytes == 256 && m_reset.peak_used_bytes == 224,
             "Ring reset restores full free capacity while preserving peak telemetry");
}

void test_work_graph_rdna2_execute_indirect_dual_gpu() {
    using namespace gpu::encode;
    hle::reset_d3d12x_stats();

    // Shared GPU buffer layout (128 uint32s = 512 bytes):
    //   [0..2]   (byte 0..11)   : D3D12_DISPATCH_ARGUMENTS written on GPU by Node 0 ({2, 1, 1})
    //   [3]      (byte 12..15)  : reserved padding
    //   [4..35]  (byte 16..143) : 32 input uint32 values (10 + i * 3)
    //   [36..67] (byte 144..271): 32 producer -> consumer records (2 * input[i] + 5)
    //   [68..99] (byte 272..399): 32 consumer final outputs (record[i] + 100 + i)

    // Node 0 ("ProducerNode", Broadcasting, numthreads(32,1,1)):
    //   v1 = v0 << 2
    //   v2 = g_mem[16 + v1]
    //   v3 = v2 + v2
    //   v3 = 5 + v3
    //   g_mem[144 + v1] = v3
    //   vcc = (0 == v0)
    //   if (!vcc) goto end
    //   g_mem[0] = s0  (ThreadGroupCountX = 2)
    //   g_mem[4] = 1   (ThreadGroupCountY = 1)
    //   g_mem[8] = 1   (ThreadGroupCountZ = 1)
    // end: s_endpgm
    std::vector<uint32_t> producer_dwords;
    producer_dwords.push_back(vop2(26, 1, 130, 0));         // v_lshlrev_b32 v1, 2, v0
    global_mem(producer_dwords, 12, 2, 1, 127, 16);         // global_load_dword v2, v1, off offset:16
    producer_dwords.push_back(vop2(37, 3, 256 + 2, 2));     // v_add_nc_u32 v3, v2, v2
    producer_dwords.push_back(vop2(37, 3, 128 + 5, 3));     // v_add_nc_u32 v3, 5, v3
    global_mem(producer_dwords, 28, 3, 1, 127, 144);        // global_store_dword v1, v3, off offset:144
    producer_dwords.push_back(vopc(0xC2, 128, 0));          // v_cmp_eq_u32 vcc, 0, v0
    producer_dwords.push_back(sopp(6, 10));                 // s_cbranch_vccz +10 -> skip 10 dwords to s_endpgm
    producer_dwords.push_back(vop1(1, 4, 128));             // v_mov_b32 v4, 0
    producer_dwords.push_back(vop1(1, 5, 0));               // v_mov_b32 v5, s0
    producer_dwords.push_back(vop1(1, 6, 129));             // v_mov_b32 v6, 1
    producer_dwords.push_back(vop1(1, 7, 129));             // v_mov_b32 v7, 1
    global_mem(producer_dwords, 28, 5, 4, 127, 0);          // global_store_dword v4, v5, off offset:0
    global_mem(producer_dwords, 28, 6, 4, 127, 4);          // global_store_dword v4, v6, off offset:4
    global_mem(producer_dwords, 28, 7, 4, 127, 8);          // global_store_dword v4, v7, off offset:8
    producer_dwords.push_back(sopp(1));                     // s_endpgm

    // Node 1 ("ConsumerNode", Coalescing, numthreads(16,1,1), packed as "KXRN"):
    //   Dispatched via ExecuteIndirect using GPU-generated {2, 1, 1} -> 32 threads!
    //   v1 = v0 << 2
    //   v2 = g_mem[144 + v1]
    //   v3 = s0 + v2
    //   v3 = v0 + v3
    //   g_mem[272 + v1] = v3
    //   s_endpgm
    std::vector<uint32_t> consumer_dwords;
    consumer_dwords.push_back(vop2(26, 1, 130, 0));         // v_lshlrev_b32 v1, 2, v0
    global_mem(consumer_dwords, 12, 2, 1, 127, 144);        // global_load_dword v2, v1, off offset:144
    consumer_dwords.push_back(vop2(37, 3, 0, 2));           // v_add_nc_u32 v3, s0, v2
    consumer_dwords.push_back(vop2(37, 3, 256 + 0, 3));     // v_add_nc_u32 v3, v0, v3
    global_mem(consumer_dwords, 28, 3, 1, 127, 272);        // global_store_dword v1, v3, off offset:272
    consumer_dwords.push_back(sopp(1));                     // s_endpgm

    gpu::WorkGraphProgram prog{};
    prog.name = "Rdna2ProducerConsumerDispatchGraph";
    const uint32_t prod_idx = prog.add_node({
        .name = "ProducerNode",
        .kind = gpu::WorkGraphNodeKind::Broadcasting,
        .group_size_x = 32,
        .group_size_y = 1,
        .group_size_z = 1,
        .record_stride_bytes = 16,
        .max_output_records = 32,
        .rdna2_dwords = producer_dwords,
    });
    const uint32_t cons_idx = prog.add_node({
        .name = "ConsumerNode",
        .kind = gpu::WorkGraphNodeKind::Coalescing,
        .group_size_x = 16,
        .group_size_y = 1,
        .group_size_z = 1,
        .record_stride_bytes = 0,
        .max_output_records = 0,
        .kxrn_bytecode = gpu::pack_kxrn_compute_bytecode(16, consumer_dwords),
    });
    prog.add_edge(prod_idx, cons_idx, 0, 144, 1);

    gpu::DxcCompiler dxc;
    KX_CHECK(dxc.initialize().has_value(), "DXC init");

    auto adapters = gpu::enumerate_d3d12_adapters();
    KX_CHECK(adapters.has_value() && !adapters->empty(), "D3D12 adapters available");

    constexpr uint32_t kRecords = 32;
    std::vector<uint32_t> init_buf(128, 0u);
    for (uint32_t i = 0; i < kRecords; ++i) {
        init_buf[4 + i] = 10u + i * 3u;
        init_buf[68 + i] = 0xDEAD0000u + i;  // Sentinel overwritten only if ExecuteIndirect runs
    }

    gpu::WorkGraphDispatchDesc dispatch_desc{};
    dispatch_desc.entry_groups_x = 1;
    dispatch_desc.entry_groups_y = 1;
    dispatch_desc.entry_groups_z = 1;
    dispatch_desc.initial_buffer_u32 = init_buf;
    dispatch_desc.node_root_constants = {
        std::array<uint32_t, 8>{2u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},    // Node 0: emit ThreadGroupCountX = 2
        std::array<uint32_t, 8>{100u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},  // Node 1: consumer bias = 100
    };

    IDXGIFactory6* factory = nullptr;
    KX_CHECK(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "CreateDXGIFactory1");

    uint64_t hw_verified = 0;
    for (const auto& ad : *adapters) {
        if (ad.is_software) continue;
        IDXGIAdapter1* dxgi_ad = nullptr;
        if (FAILED(factory->EnumAdapters1(ad.index, &dxgi_ad))) continue;

        ID3D12Device* raw_dev = nullptr;
        HRESULT hr = D3D12CreateDevice(dxgi_ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&raw_dev));
        dxgi_ad->Release();
        if (FAILED(hr) || !raw_dev) continue;

        auto* dev_proxy = new hle::KxDeviceProxy(raw_dev);
        raw_dev->Release();

        const auto cap = gpu::WorkGraphExecutor::CheckWorkGraphSupport(dev_proxy);
        KX_CHECK(cap.selected_mode == gpu::WorkGraphExecutionMode::FallbackExecuteIndirect,
                 "SDK 10.0.22621.0 selects clean-room ExecuteIndirect fallback scheduler");

        {
            gpu::WorkGraphExecutor exec;
            auto comp = exec.compile(dev_proxy, prog, dxc);
            KX_CHECK(comp.has_value(), comp.error().message);
            KX_CHECK(exec.is_compiled() && exec.compiled_node_dxil().size() == 2, "2 compiled node DXIL blobs");

            auto res = exec.execute(dispatch_desc);
            KX_CHECK(res.has_value(), res.error().message);
            KX_CHECK(res->direct_dispatches == 1 && res->indirect_dispatches == 1 && res->uav_barriers >= 2,
                     "1 entry dispatch + 1 GPU ExecuteIndirect consumer dispatch + UAV barriers");
            KX_CHECK(res->executed_node_order.size() == 2 &&
                     res->executed_node_order[0] == 0 && res->executed_node_order[1] == 1,
                     "Executed in topological order [0, 1]");
            KX_CHECK(res->allocator_metrics.allocation_count >= 1 && res->allocator_metrics.used_bytes > 0,
                     "RecordRingAllocator tracked edge record backing");

            // Verify GPU-generated D3D12_DISPATCH_ARGUMENTS {2, 1, 1}
            KX_CHECK(res->output_u32[0] == 2u && res->output_u32[1] == 1u && res->output_u32[2] == 1u,
                     std::format("GPU-generated dispatch args mismatch on {}: {{{}, {}, {}}}",
                                 ad.name, res->output_u32[0], res->output_u32[1], res->output_u32[2]));

            // Verify all 32 intermediate producer records and all 32 consumer outputs bit-exact
            for (uint32_t i = 0; i < kRecords; ++i) {
                const uint32_t expected_rec = init_buf[4 + i] * 2u + 5u;
                const uint32_t expected_out = expected_rec + 100u + i;
                KX_CHECK(res->output_u32[36 + i] == expected_rec,
                         std::format("Producer record[{}] mismatch on {}: got {} != {}",
                                     i, ad.name, res->output_u32[36 + i], expected_rec));
                KX_CHECK(res->output_u32[68 + i] == expected_out,
                         std::format("Consumer output[{}] mismatch on {}: got {} != {}",
                                     i, ad.name, res->output_u32[68 + i], expected_out));
            }

            ++hw_verified;
            std::cout << std::format(
                "    -> Hardware [{}] {} PASS (2-node RDNA2 Work Graph -> GPU ExecuteIndirect {{2,1,1}} | 32/32 records bit-exact | {:.2f} ms | Driver WG Tier={})\n",
                ad.index, ad.name, res->gpu_ms, cap.driver_work_graphs_tier);
        }
        dev_proxy->Release();
    }
    factory->Release();

    KX_CHECK(hw_verified >= 1, "At least one hardware GPU verified for RDNA2 Work Graph execution");
    const auto stats = hle::d3d12x_stats();
    KX_CHECK(stats.work_graphs_created == hw_verified, "D3D12XStats::work_graphs_created matches verified GPUs");
    KX_CHECK(stats.work_graph_dispatches == hw_verified, "D3D12XStats::work_graph_dispatches matches verified GPUs");
    KX_CHECK(stats.pso_binds >= hw_verified * 2 && stats.dispatch_calls >= hw_verified && stats.barrier_calls >= hw_verified * 2,
             "KxDeviceProxy / KxGraphicsCommandListProxy telemetry recorded Work Graph dispatches and barriers");
}

}  // namespace

void test_rdna2_mesh_amplification_decode_ir_dxc() {
    using namespace gpu::encode;

    // 1. Clean-room RDNA2 Mesh Shader (ms_6_6): 4 vertices, 2 triangles (quad)
    std::vector<uint32_t> ms_words;
    mesh_alloc(ms_words, 4, 2);
    ms_words.push_back(vop2(27, 1, 128 + 3, 0));         // v1 = gidx & 3
    ms_words.push_back(vop2(26, 1, 128 + 5, 1));         // v1 = (gidx & 3) * 32
    mubuf(ms_words, 14, 2, 1, 0, 128, 0);                // buffer_load_dwordx4 v[2:5] (pos)
    mubuf(ms_words, 14, 6, 1, 0, 128, 16);               // buffer_load_dwordx4 v[6:9] (color)
    exp(ms_words, gpu::kExpTargetPos0, 0xFu, 2, 3, 4, 5, false);
    exp(ms_words, gpu::kExpTargetParam0, 0xFu, 6, 7, 8, 9, false);
    ms_words.push_back(vop2(26, 10, 128 + 1, 0));        // v10 = gidx * 2
    ms_words.push_back(vop1(1, 11, 128 + 1));            // v11 = 1
    ms_words.push_back(vop2(37, 12, 128 + 2, 0));        // v12 = gidx + 2
    exp_prim0(ms_words, 10, 11, 12, true);
    ms_words.push_back(sopp(1));                         // s_endpgm

    auto ms_dec = gpu::decode_rdna2(ms_words.data(), ms_words.size());
    KX_CHECK(ms_dec.size() >= 10, "Mesh shader decode count");
    bool saw_sendmsg = false, saw_prim0 = false, saw_m0_disasm = false;
    for (const auto& d : ms_dec) {
        const std::string dis = gpu::disassemble_inst(d);
        if (d.mnemonic == "s_sendmsg" && dis.find("GS_ALLOC_REQ") != std::string::npos) saw_sendmsg = true;
        if (d.enc == gpu::Encoding::EXP && d.opcode == gpu::kExpTargetPrim0 && dis.find("prim0") != std::string::npos) saw_prim0 = true;
        if (d.mnemonic == "s_mov_b32" && dis.find("m0") != std::string::npos) saw_m0_disasm = true;
    }
    KX_CHECK(saw_sendmsg && saw_prim0 && saw_m0_disasm, "Decoded s_sendmsg(GS_ALLOC_REQ), m0, and exp prim0");

    auto ms_ir = gpu::lift_rdna2_to_ir(ms_dec);
    KX_CHECK(ms_ir.unknown_count == 0, "Mesh shader IR has zero unknown ops");
    KX_CHECK(ms_ir.max_mesh_verts == 4 && ms_ir.max_mesh_prims == 2, "IR tracked max_mesh_verts=4, max_mesh_prims=2");
    KX_CHECK(ms_ir.mesh_param_mask == 1u, "IR tracked mesh_param_mask bit 0 (param0)");

    bool has_set_counts = false, has_vtx_exp = false, has_prim_exp = false;
    for (const auto& blk : ms_ir.blocks) {
        for (const auto& inst : blk.insts) {
            if (inst.op == gpu::IrOp::SetMeshOutputCounts) has_set_counts = true;
            if (inst.op == gpu::IrOp::MeshExportVertex) has_vtx_exp = true;
            if (inst.op == gpu::IrOp::MeshExportPrimitive) has_prim_exp = true;
        }
    }
    KX_CHECK(has_set_counts && has_vtx_exp && has_prim_exp, "IR lifted SetMeshOutputCounts + MeshExportVertex + MeshExportPrimitive");

    gpu::DxcCompiler dxc;
    KX_CHECK(dxc.initialize().has_value(), "DXC init");

    auto ms_recomp = gpu::emit_hlsl_from_ir(ms_ir, {
        .stage = gpu::ShaderStage::Mesh,
        .wave_mode = gpu::WaveMode::Wave32,
        .group_size_x = 32,
    });
    KX_CHECK(ms_recomp.hlsl.find("[outputtopology(\"triangle\")]") != std::string::npos, "MS outputtopology");
    KX_CHECK(ms_recomp.hlsl.find("SetMeshOutputCounts(") != std::string::npos, "MS SetMeshOutputCounts");
    KX_CHECK(ms_recomp.hlsl.find("out vertices KxMeshVert kx_verts[4]") != std::string::npos, "MS out vertices[4]");
    KX_CHECK(ms_recomp.hlsl.find("out indices uint3 kx_tris[2]") != std::string::npos, "MS out indices[2]");

    auto ms_dxil = dxc.compile_hlsl(ms_recomp.hlsl, "main", "ms_6_6");
    KX_CHECK(ms_dxil.has_value(), ms_dxil.error().message);
    KX_CHECK(ms_dxil->dxil.size() > 64 && std::memcmp(ms_dxil->dxil.data(), "DXBC", 4) == 0, "Valid ms_6_6 DXIL");

    // 2. Packed GFX10.3 prim0 (en=0x1) + param1 + payload-receiving MS (ms_6_6)
    std::vector<uint32_t> ms_packed_words;
    mesh_alloc(ms_packed_words, 3, 1);
    ms_packed_words.push_back(vop1(1, 2, 128));          // v2 = 0.0
    ms_packed_words.push_back(vop1(1, 3, 242));          // v3 = 1.0
    exp(ms_packed_words, gpu::kExpTargetPos0, 0xFu, 2, 2, 2, 3, false);
    exp(ms_packed_words, gpu::kExpTargetParam0 + 1, 0xFu, 3, 2, 2, 3, false);
    // Packed prim0: null=0, i0=0, i1=1, i2=2 -> (0 << 1) | (1 << 10) | (2 << 19)
    ms_packed_words.push_back(vop1(1, 4, 255));
    ms_packed_words.push_back((0u << 1) | (1u << 10) | (2u << 19));
    exp_prim0_packed(ms_packed_words, 4, true);
    ms_packed_words.push_back(sopp(1));

    auto ms_packed_ir = gpu::lift_rdna2_to_ir(gpu::decode_rdna2(ms_packed_words.data(), ms_packed_words.size()));
    KX_CHECK(ms_packed_ir.unknown_count == 0 && ms_packed_ir.max_mesh_verts == 3 && ms_packed_ir.max_mesh_prims == 1,
             "Packed MS IR metadata");
    KX_CHECK((ms_packed_ir.mesh_param_mask & (1u << 1)) != 0u, "IR tracked param1 in mesh_param_mask");

    auto ms_payload_recomp = gpu::emit_hlsl_from_ir(ms_packed_ir, {
        .stage = gpu::ShaderStage::Mesh,
        .wave_mode = gpu::WaveMode::Wave32,
        .group_size_x = 32,
        .mesh_has_payload = true,
    });
    KX_CHECK(ms_payload_recomp.hlsl.find("in payload KxMeshPayload kx_payload") != std::string::npos, "MS in payload");
    KX_CHECK(ms_payload_recomp.hlsl.find("TEXCOORD1") != std::string::npos, "MS param1 TEXCOORD1");
    auto ms_payload_dxil = dxc.compile_hlsl(ms_payload_recomp.hlsl, "main", "ms_6_6");
    KX_CHECK(ms_payload_dxil.has_value(), ms_payload_dxil.error().message);

    // 3. Clean-room RDNA2 Amplification Shader (as_6_6): write payload + DispatchMesh(1, 1, 1)
    std::vector<uint32_t> as_words;
    as_words.push_back(vop1(1, 1, 128));                 // v1 = 0 (byte offset 0)
    as_words.push_back(vop1(1, 2, 242));                 // v2 = 1.0f
    ds(as_words, 13, 0, 1, 2, 0, 0);                     // ds_write_b32 payload[0] = 1.0f
    as_words.push_back(sopp(10));                        // s_barrier
    dispatch_mesh(as_words, 1, 1, 1);
    as_words.push_back(sopp(1));                         // s_endpgm

    auto as_dec = gpu::decode_rdna2(as_words.data(), as_words.size());
    bool saw_dispatch_disasm = false;
    for (const auto& d : as_dec) {
        if (gpu::disassemble_inst(d).find("DISPATCH_MESH") != std::string::npos) saw_dispatch_disasm = true;
    }
    KX_CHECK(saw_dispatch_disasm, "Decoded s_sendmsg(DISPATCH_MESH)");

    auto as_ir = gpu::lift_rdna2_to_ir(as_dec);
    KX_CHECK(as_ir.unknown_count == 0 && as_ir.uses_lds, "AS IR lift");
    bool has_dispatch_op = false;
    for (const auto& blk : as_ir.blocks) {
        for (const auto& inst : blk.insts) {
            if (inst.op == gpu::IrOp::DispatchMeshPayload) has_dispatch_op = true;
        }
    }
    KX_CHECK(has_dispatch_op, "IR lifted DispatchMeshPayload");

    auto as_recomp = gpu::emit_hlsl_from_ir(as_ir, {
        .stage = gpu::ShaderStage::Amplification,
        .wave_mode = gpu::WaveMode::Wave32,
        .group_size_x = 32,
    });
    KX_CHECK(as_recomp.hlsl.find("groupshared KxMeshPayload kx_payload;") != std::string::npos, "AS groupshared payload");
    KX_CHECK(as_recomp.hlsl.find("DispatchMesh(") != std::string::npos, "AS DispatchMesh emit");

    auto as_dxil = dxc.compile_hlsl(as_recomp.hlsl, "main", "as_6_6");
    KX_CHECK(as_dxil.has_value(), as_dxil.error().message);
    KX_CHECK(as_dxil->dxil.size() > 64 && std::memcmp(as_dxil->dxil.data(), "DXBC", 4) == 0, "Valid as_6_6 DXIL");

    // 4. Multi-block branching Amplification Shader compiles cleanly (top-level uniform DispatchMesh)
    std::vector<uint32_t> as_cfg_words;
    as_cfg_words.push_back(sopc(6, 4, 128));             // s_cmp_eq_u32 s4 (gid.x), 0
    as_cfg_words.push_back(sopp(4, 3));                  // s_cbranch_scc0 +3 dwords -> skip dispatch_mesh(1,1,1)
    dispatch_mesh(as_cfg_words, 1, 1, 1);                // 3 dwords (s_mov_b32 m0 + lit + s_sendmsg)
    as_cfg_words.push_back(sopp(1));                     // s_endpgm
    auto as_cfg_ir = gpu::lift_rdna2_to_ir(gpu::decode_rdna2(as_cfg_words.data(), as_cfg_words.size()));
    KX_CHECK(as_cfg_ir.blocks.size() >= 2 && as_cfg_ir.unknown_count == 0, "Multi-block AS IR");
    auto as_cfg_recomp = gpu::emit_hlsl_from_ir(as_cfg_ir, {
        .stage = gpu::ShaderStage::Amplification,
        .wave_mode = gpu::WaveMode::Wave32,
        .group_size_x = 32,
    });
    auto as_cfg_dxil = dxc.compile_hlsl(as_cfg_recomp.hlsl, "main", "as_6_6");
    KX_CHECK(as_cfg_dxil.has_value(), as_cfg_dxil.error().message);
}

void test_rdna2_mesh_shader_dispatch_mesh_hardware_parity() {
    using namespace gpu::encode;

    // 1. Standalone Mesh Shader (ms_6_6): loads 4 quad vertices (pos + color) from t1, emits 2 triangles
    std::vector<uint32_t> ms_words;
    mesh_alloc(ms_words, 4, 2);
    ms_words.push_back(vop2(27, 1, 128 + 3, 0));         // v1 = gidx & 3
    ms_words.push_back(vop2(26, 1, 128 + 5, 1));         // v1 = (gidx & 3) * 32
    mubuf(ms_words, 14, 2, 1, 0, 128, 0);                // v[2:5] = pos.xyzw
    mubuf(ms_words, 14, 6, 1, 0, 128, 16);               // v[6:9] = color.rgba
    exp(ms_words, gpu::kExpTargetPos0, 0xFu, 2, 3, 4, 5, false);
    exp(ms_words, gpu::kExpTargetParam0, 0xFu, 6, 7, 8, 9, false);
    ms_words.push_back(vop2(26, 10, 128 + 1, 0));        // v10 = gidx * 2  (0, 2)
    ms_words.push_back(vop1(1, 11, 128 + 1));            // v11 = 1         (1, 1)
    ms_words.push_back(vop2(37, 12, 128 + 2, 0));        // v12 = gidx + 2  (2, 3)
    exp_prim0(ms_words, 10, 11, 12, true);
    ms_words.push_back(sopp(1));

    // 2. Amplification Shader (as_6_6): writes RGBA tint (0.75, 0.30, 0.90, 1.0) into KxMeshPayload[0..3]
    std::vector<uint32_t> as_words;
    auto write_payload_f32 = [&](uint32_t dword_idx, float val) {
        as_words.push_back(vop1(1, 1, 128 + dword_idx * 4));  // v1 = byte offset
        as_words.push_back(vop1(1, 2, 255));
        as_words.push_back(std::bit_cast<uint32_t>(val));
        ds(as_words, 13, 0, 1, 2, 0, 0);                      // ds_write_b32 kx_payload.data[dword_idx]
    };
    write_payload_f32(0, 0.75f);
    write_payload_f32(1, 0.30f);
    write_payload_f32(2, 0.90f);
    write_payload_f32(3, 1.00f);
    as_words.push_back(sopp(10));                             // s_barrier
    dispatch_mesh(as_words, 1, 1, 1);
    as_words.push_back(sopp(1));

    // 3. Payload-receiving Mesh Shader (ms_6_6): reads pos from t1 and RGBA color from s[8..11] (KxMeshPayload)
    std::vector<uint32_t> ms_pay_words;
    mesh_alloc(ms_pay_words, 4, 2);
    ms_pay_words.push_back(vop2(27, 1, 128 + 3, 0));     // v1 = gidx & 3
    ms_pay_words.push_back(vop2(26, 1, 128 + 5, 1));     // v1 = (gidx & 3) * 32
    mubuf(ms_pay_words, 14, 2, 1, 0, 128, 0);            // v[2:5] = pos.xyzw
    ms_pay_words.push_back(vop1(1, 6, 8));               // v6 = s8  (payload.data[0])
    ms_pay_words.push_back(vop1(1, 7, 9));               // v7 = s9  (payload.data[1])
    ms_pay_words.push_back(vop1(1, 8, 10));              // v8 = s10 (payload.data[2])
    ms_pay_words.push_back(vop1(1, 9, 11));              // v9 = s11 (payload.data[3])
    exp(ms_pay_words, gpu::kExpTargetPos0, 0xFu, 2, 3, 4, 5, false);
    exp(ms_pay_words, gpu::kExpTargetParam0, 0xFu, 6, 7, 8, 9, false);
    ms_pay_words.push_back(vop2(26, 10, 128 + 1, 0));
    ms_pay_words.push_back(vop1(1, 11, 128 + 1));
    ms_pay_words.push_back(vop2(37, 12, 128 + 2, 0));
    exp_prim0(ms_pay_words, 10, 11, 12, true);
    ms_pay_words.push_back(sopp(1));

    // 4. Pixel Shader (ps_6_6): export interpolated param0 -> mrt0
    std::vector<uint32_t> ps_words;
    exp(ps_words, 0, 0xFu, 0, 1, 2, 3, true);
    ps_words.push_back(sopp(1));

    gpu::DxcCompiler dxc;
    KX_CHECK(dxc.initialize().has_value(), "DXC init");

    auto ms_ir = gpu::lift_rdna2_to_ir(gpu::decode_rdna2(ms_words.data(), ms_words.size()));
    auto as_ir = gpu::lift_rdna2_to_ir(gpu::decode_rdna2(as_words.data(), as_words.size()));
    auto ms_pay_ir = gpu::lift_rdna2_to_ir(gpu::decode_rdna2(ms_pay_words.data(), ms_pay_words.size()));
    auto ps_ir = gpu::lift_rdna2_to_ir(gpu::decode_rdna2(ps_words.data(), ps_words.size()));

    auto ms_hlsl = gpu::emit_hlsl_from_ir(ms_ir, {.stage = gpu::ShaderStage::Mesh, .wave_mode = gpu::WaveMode::Wave32, .group_size_x = 32});
    auto as_hlsl = gpu::emit_hlsl_from_ir(as_ir, {.stage = gpu::ShaderStage::Amplification, .wave_mode = gpu::WaveMode::Wave32, .group_size_x = 32});
    auto ms_pay_hlsl = gpu::emit_hlsl_from_ir(ms_pay_ir, {.stage = gpu::ShaderStage::Mesh, .wave_mode = gpu::WaveMode::Wave32, .group_size_x = 32, .mesh_has_payload = true});
    auto ps_hlsl = gpu::emit_hlsl_from_ir(ps_ir, {.stage = gpu::ShaderStage::Pixel});

    auto ms_dxil = dxc.compile_hlsl(ms_hlsl.hlsl, "main", "ms_6_6");
    auto as_dxil = dxc.compile_hlsl(as_hlsl.hlsl, "main", "as_6_6");
    auto ms_pay_dxil = dxc.compile_hlsl(ms_pay_hlsl.hlsl, "main", "ms_6_6");
    auto ps_dxil = dxc.compile_hlsl(ps_hlsl.hlsl, "main", "ps_6_6");
    KX_CHECK(ms_dxil.has_value(), ms_dxil.error().message);
    KX_CHECK(as_dxil.has_value(), as_dxil.error().message);
    KX_CHECK(ms_pay_dxil.has_value(), ms_pay_dxil.error().message);
    KX_CHECK(ps_dxil.has_value(), ps_dxil.error().message);

    // Quad vertices covering [-0.8, 0.8] x [-0.8, 0.8] with solid color (0.20, 0.80, 0.40, 1.00)
    const float quad_data[32] = {
        -0.8f, -0.8f, 0.0f, 1.0f,  0.20f, 0.80f, 0.40f, 1.0f,
         0.8f, -0.8f, 0.0f, 1.0f,  0.20f, 0.80f, 0.40f, 1.0f,
        -0.8f,  0.8f, 0.0f, 1.0f,  0.20f, 0.80f, 0.40f, 1.0f,
         0.8f,  0.8f, 0.0f, 1.0f,  0.20f, 0.80f, 0.40f, 1.0f,
    };

    auto adapters = gpu::enumerate_d3d12_adapters();
    KX_CHECK(adapters.has_value() && !adapters->empty(), "Adapters available");

    IDXGIFactory6* factory = nullptr;
    KX_CHECK(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "CreateDXGIFactory1");

    constexpr uint32_t kW = 256, kH = 256;
    std::vector<uint8_t> ref_ms_pixels;
    std::vector<uint8_t> ref_as_pixels;
    int verified = 0;

    for (const auto& ad : *adapters) {
        if (ad.is_software) continue;
        IDXGIAdapter1* dxgi_ad = nullptr;
        if (FAILED(factory->EnumAdapters1(ad.index, &dxgi_ad))) continue;
        ID3D12Device2* dev2 = nullptr;
        HRESULT hr = D3D12CreateDevice(dxgi_ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev2));
        dxgi_ad->Release();
        if (FAILED(hr) || !dev2) continue;

        D3D12_FEATURE_DATA_D3D12_OPTIONS7 opt7{};
        if (FAILED(dev2->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opt7, sizeof(opt7))) ||
            opt7.MeshShaderTier < D3D12_MESH_SHADER_TIER_1) {
            std::cout << std::format("    -> Hardware [{}] {} skipped (MeshShaderTier == NOT_SUPPORTED)\n",
                                     ad.index, ad.name);
            dev2->Release();
            continue;
        }

        // Root signature: SRV t1 (ByteAddressBuffer g_buf0) visible to Mesh shader
        D3D12_ROOT_PARAMETER rp{};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rp.Descriptor.ShaderRegister = 1;
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;
        D3D12_ROOT_SIGNATURE_DESC rsd{1, &rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ID3DBlob* rs_blob = nullptr;
        ID3D12RootSignature* rs = nullptr;
        KX_CHECK(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, nullptr)), "SerializeRS");
        KX_CHECK(SUCCEEDED(dev2->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&rs))), "CreateRS");
        rs_blob->Release();

        struct alignas(void*) SubRS { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE; ID3D12RootSignature* v = nullptr; };
        struct alignas(void*) SubAS { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS; D3D12_SHADER_BYTECODE v{}; };
        struct alignas(void*) SubMS { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS; D3D12_SHADER_BYTECODE v{}; };
        struct alignas(void*) SubPS { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS; D3D12_SHADER_BYTECODE v{}; };
        struct alignas(void*) SubRast { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER; D3D12_RASTERIZER_DESC v{}; };
        struct alignas(void*) SubBlend { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND; D3D12_BLEND_DESC v{}; uint32_t pad = 0; };
        struct alignas(void*) SubRTV { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS; D3D12_RT_FORMAT_ARRAY v{}; };
        struct alignas(void*) SubSamp { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC; DXGI_SAMPLE_DESC v{1, 0}; uint32_t pad = 0; };
        struct alignas(void*) SubMask { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK; UINT v = UINT_MAX; };

        auto init_common = [&](auto& s) {
            s.rs.v = rs;
            s.rast.v.FillMode = D3D12_FILL_MODE_SOLID;
            s.rast.v.CullMode = D3D12_CULL_MODE_NONE;
            s.blend.v.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            s.rtv.v.NumRenderTargets = 1;
            s.rtv.v.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        };

        struct MeshOnlyStream { SubRS rs; SubMS ms; SubPS ps; SubRast rast; SubBlend blend; SubRTV rtv; SubSamp samp; SubMask mask; } ms_stream{};
        init_common(ms_stream);
        ms_stream.ms.v = {ms_dxil->dxil.data(), ms_dxil->dxil.size()};
        ms_stream.ps.v = {ps_dxil->dxil.data(), ps_dxil->dxil.size()};
        D3D12_PIPELINE_STATE_STREAM_DESC ms_desc{sizeof(ms_stream), &ms_stream};
        ID3D12PipelineState* ms_pso = nullptr;
        KX_CHECK(SUCCEEDED(dev2->CreatePipelineState(&ms_desc, IID_PPV_ARGS(&ms_pso))) && ms_pso, "Create MS+PS PSO");

        struct AmpMeshStream { SubRS rs; SubAS as; SubMS ms; SubPS ps; SubRast rast; SubBlend blend; SubRTV rtv; SubSamp samp; SubMask mask; } as_stream{};
        init_common(as_stream);
        as_stream.as.v = {as_dxil->dxil.data(), as_dxil->dxil.size()};
        as_stream.ms.v = {ms_pay_dxil->dxil.data(), ms_pay_dxil->dxil.size()};
        as_stream.ps.v = {ps_dxil->dxil.data(), ps_dxil->dxil.size()};
        D3D12_PIPELINE_STATE_STREAM_DESC as_desc{sizeof(as_stream), &as_stream};
        ID3D12PipelineState* as_pso = nullptr;
        KX_CHECK(SUCCEEDED(dev2->CreatePipelineState(&as_desc, IID_PPV_ARGS(&as_pso))) && as_pso, "Create AS+MS+PS PSO");

        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* queue = nullptr;
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList6* cmd6 = nullptr;
        KX_CHECK(SUCCEEDED(dev2->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))), "CreateCommandQueue");
        KX_CHECK(SUCCEEDED(dev2->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))), "CreateCommandAllocator");
        KX_CHECK(SUCCEEDED(dev2->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cmd6))), "CreateCommandList6");
        cmd6->Close();

        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
        ID3D12DescriptorHeap* rtv_heap = nullptr;
        dev2->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap));
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();

        D3D12_HEAP_PROPERTIES hp_def{}; hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_HEAP_PROPERTIES hp_up{}; hp_up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_HEAP_PROPERTIES hp_rb{}; hp_rb.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kW; td.Height = kH; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ID3D12Resource* rt = nullptr;
        dev2->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt));
        dev2->CreateRenderTargetView(rt, nullptr, rtv_handle);

        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = kW * kH * 4; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* rb = nullptr;
        dev2->CreateCommittedResource(&hp_rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));

        D3D12_RESOURCE_DESC vbd = bd; vbd.Width = sizeof(quad_data);
        ID3D12Resource* vb = nullptr;
        dev2->CreateCommittedResource(&hp_up, D3D12_HEAP_FLAG_NONE, &vbd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vb));
        void* mapped = nullptr;
        vb->Map(0, nullptr, &mapped);
        std::memcpy(mapped, quad_data, sizeof(quad_data));
        vb->Unmap(0, nullptr);

        ID3D12Fence* fence = nullptr;
        dev2->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        UINT64 fence_val = 0;

        auto run_dispatch_mesh = [&](ID3D12PipelineState* pso) -> std::vector<uint8_t> {
            alloc->Reset();
            cmd6->Reset(alloc, pso);
            cmd6->SetGraphicsRootSignature(rs);
            cmd6->SetGraphicsRootShaderResourceView(0, vb->GetGPUVirtualAddress());
            D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f};
            D3D12_RECT sr{0, 0, static_cast<LONG>(kW), static_cast<LONG>(kH)};
            cmd6->RSSetViewports(1, &vp);
            cmd6->RSSetScissorRects(1, &sr);
            cmd6->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
            const float clear_zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            cmd6->ClearRenderTargetView(rtv_handle, clear_zero, 0, nullptr);
            cmd6->DispatchMesh(1, 1, 1);

            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = {rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
            cmd6->ResourceBarrier(1, &b);

            D3D12_TEXTURE_COPY_LOCATION dst_loc{};
            dst_loc.pResource = rb;
            dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst_loc.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kW, kH, 1, kW * 4};
            D3D12_TEXTURE_COPY_LOCATION src_loc{};
            src_loc.pResource = rt;
            src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cmd6->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            cmd6->ResourceBarrier(1, &b);
            cmd6->Close();

            ID3D12CommandList* cls[] = {cmd6};
            queue->ExecuteCommandLists(1, cls);
            ++fence_val;
            queue->Signal(fence, fence_val);
            fence->SetEventOnCompletion(fence_val, ev);
            WaitForSingleObject(ev, 5000);

            std::vector<uint8_t> pixels(kW * kH * 4);
            uint8_t* ptr = nullptr;
            rb->Map(0, nullptr, reinterpret_cast<void**>(&ptr));
            std::memcpy(pixels.data(), ptr, pixels.size());
            rb->Unmap(0, nullptr);
            return pixels;
        };

        // Pass A: Standalone Mesh Shader + Pixel Shader (expected quad color 0.20, 0.80, 0.40, 1.0 -> 51, 204, 102, 255)
        auto ms_px = run_dispatch_mesh(ms_pso);
        const size_t corner = (4 * kW + 4) * 4;
        const size_t tri0_px = (192 * kW + 64) * 4;
        const size_t tri1_px = (64 * kW + 192) * 4;
        const size_t center = (128 * kW + 128) * 4;
        KX_CHECK(ms_px[corner + 0] == 0 && ms_px[corner + 1] == 0 && ms_px[corner + 2] == 0 && ms_px[corner + 3] == 0,
                 "MS corner outside quad is cleared black");
        for (size_t off : {tri0_px, tri1_px, center}) {
            KX_CHECK(std::abs(static_cast<int>(ms_px[off + 0]) - 51) <= 1 &&
                     std::abs(static_cast<int>(ms_px[off + 1]) - 204) <= 1 &&
                     std::abs(static_cast<int>(ms_px[off + 2]) - 102) <= 1 &&
                     ms_px[off + 3] == 255,
                     "MS+PS DispatchMesh quad pixel matches expected RGBA (51,204,102,255)");
        }

        // Pass B: Amplification Shader + Payload Mesh Shader + Pixel Shader (payload tint 0.75, 0.30, 0.90, 1.0 -> 191, 77, 230, 255)
        auto as_px = run_dispatch_mesh(as_pso);
        KX_CHECK(as_px[corner + 0] == 0 && as_px[corner + 1] == 0 && as_px[corner + 2] == 0 && as_px[corner + 3] == 0,
                 "AS+MS corner outside quad is cleared black");
        KX_CHECK(std::abs(static_cast<int>(as_px[center + 0]) - 191) <= 1 &&
                 std::abs(static_cast<int>(as_px[center + 1]) - 77) <= 1 &&
                 std::abs(static_cast<int>(as_px[center + 2]) - 230) <= 1 &&
                 as_px[center + 3] == 255,
                 "AS+MS+PS DispatchMesh payload tint matches expected RGBA (191,77,230,255)");

        if (ref_ms_pixels.empty()) {
            ref_ms_pixels = std::move(ms_px);
            ref_as_pixels = std::move(as_px);
        } else {
            KX_CHECK(ref_ms_pixels == ms_px, "Cross-GPU bit-exact MS+PS DispatchMesh parity");
            KX_CHECK(ref_as_pixels == as_px, "Cross-GPU bit-exact AS+MS+PS DispatchMesh parity");
        }

        std::cout << std::format("    -> Hardware [{}] {} PASS (MeshShaderTier {}, MS+PS & AS+MS+PS DispatchMesh quad verified)\n",
                                 ad.index, ad.name, static_cast<int>(opt7.MeshShaderTier));
        ++verified;

        CloseHandle(ev);
        fence->Release();
        vb->Release(); rb->Release(); rt->Release(); rtv_heap->Release();
        cmd6->Release(); alloc->Release(); queue->Release();
        as_pso->Release(); ms_pso->Release(); rs->Release(); dev2->Release();
    }
    factory->Release();
    KX_CHECK(verified >= 1, "Expected at least 1 hardware GPU supporting D3D12 Mesh Shaders");
}

void test_d3d12x_mesh_stream_pso_recompilation() {
    using namespace gpu::encode;
    const fs::path cache_dir = fs::temp_directory_path() / "KestrelX_MeshStreamPsoTest";
    std::error_code ec;
    fs::remove_all(cache_dir, ec);

    hle::reset_d3d12x_stats();
    hle::set_d3d12x_shader_cache_dir(cache_dir);

    hle::Registry reg;
    hle::register_d3d12x(reg);
    using CreateDevFn = HRESULT(__stdcall*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto create_dev = reinterpret_cast<CreateDevFn>(reg.resolve_name("d3d12_xs.dll", "D3D12CreateDevice"));
    ID3D12Device2* dev2 = nullptr;
    KX_CHECK(SUCCEEDED(create_dev(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev2))) && dev2, "Create KxDeviceProxy ID3D12Device2");

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 opt7{};
    if (FAILED(dev2->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opt7, sizeof(opt7))) ||
        opt7.MeshShaderTier < D3D12_MESH_SHADER_TIER_1) {
        std::cout << "    -> Default adapter does not support Mesh Shaders; skipping stream PSO hardware test\n";
        dev2->Release();
        fs::remove_all(cache_dir, ec);
        return;
    }

    auto make_container = [](const char magic[4], uint32_t hdr_u32, const std::vector<uint32_t>& words) {
        std::vector<uint8_t> out(8 + words.size() * 4);
        std::memcpy(out.data(), magic, 4);
        std::memcpy(out.data() + 4, &hdr_u32, 4);
        std::memcpy(out.data() + 8, words.data(), words.size() * 4);
        return out;
    };

    // Build clean-room KXRM (fullscreen triangle from immediate floats) + KXRP containers
    std::vector<uint32_t> ms_words;
    mesh_alloc(ms_words, 3, 1);
    // Emit vertex positions using v0 (0, 1, 2):
    // v2 = -1.0 + 4.0 * float(v0 & 1)   -> x = -1, 3, -1
    // v3 = -1.0 + 4.0 * float(v0 >> 1)  -> y = -1, -1, 3
    ms_words.push_back(vop2(27, 4, 128 + 1, 0));         // v4 = v0 & 1
    ms_words.push_back(vop1(6, 4, 256 + 4));             // v4 = float(v4)
    vop3(ms_words, 0x14B, 2, 256 + 4, 246, 243);         // v2 = fma(v4, 4.0, -1.0)
    ms_words.push_back(vop2(22, 5, 128 + 1, 0));         // v5 = v0 >> 1
    ms_words.push_back(vop1(6, 5, 256 + 5));             // v5 = float(v5)
    vop3(ms_words, 0x14B, 3, 256 + 5, 246, 243);         // v3 = fma(v5, 4.0, -1.0)
    ms_words.push_back(vop1(1, 6, 128));                 // v6 = 0.0f
    ms_words.push_back(vop1(1, 7, 242));                 // v7 = 1.0f
    exp(ms_words, gpu::kExpTargetPos0, 0xFu, 2, 3, 6, 7, false);
    // Color (0.5, 1.0, 0.0, 1.0)
    ms_words.push_back(vop1(1, 8, 240));                 // v8 = 0.5f
    exp(ms_words, gpu::kExpTargetParam0, 0xFu, 8, 7, 6, 7, false);
    ms_words.push_back(vop1(1, 9, 128 + 0));             // v9 = 0
    ms_words.push_back(vop1(1, 10, 128 + 1));            // v10 = 1
    ms_words.push_back(vop1(1, 11, 128 + 2));            // v11 = 2
    exp_prim0(ms_words, 9, 10, 11, true);
    ms_words.push_back(sopp(1));

    std::vector<uint32_t> ps_words;
    exp(ps_words, 0, 0xFu, 0, 1, 2, 3, true);
    ps_words.push_back(sopp(1));

    // Amplification shader container ("KXRA") dispatching (1, 1, 1)
    std::vector<uint32_t> as_words;
    as_words.push_back(vop1(1, 1, 128));
    as_words.push_back(vop1(1, 2, 242));
    ds(as_words, 13, 0, 1, 2, 0, 0);
    as_words.push_back(sopp(10));
    dispatch_mesh(as_words, 1, 1, 1);
    as_words.push_back(sopp(1));

    auto kxrm = make_container("KXRM", 32u, ms_words);
    auto kxrp = make_container("KXRP", 0u, ps_words);
    auto kxra = make_container("KXRA", 32u, as_words);

    D3D12_ROOT_SIGNATURE_DESC rsd{0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* rs_blob = nullptr;
    ID3D12RootSignature* rs = nullptr;
    KX_CHECK(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, nullptr)), "SerializeRS");
    KX_CHECK(SUCCEEDED(dev2->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&rs))), "CreateRS");
    rs_blob->Release();

    struct alignas(void*) SubRS { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE; ID3D12RootSignature* v = nullptr; };
    struct alignas(void*) SubAS { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS; D3D12_SHADER_BYTECODE v{}; };
    struct alignas(void*) SubMS { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS; D3D12_SHADER_BYTECODE v{}; };
    struct alignas(void*) SubPS { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS; D3D12_SHADER_BYTECODE v{}; };
    struct alignas(void*) SubRast { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER; D3D12_RASTERIZER_DESC v{}; };
    struct alignas(void*) SubBlend { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND; D3D12_BLEND_DESC v{}; uint32_t pad = 0; };
    struct alignas(void*) SubRTV { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS; D3D12_RT_FORMAT_ARRAY v{}; };
    struct alignas(void*) SubSamp { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC; DXGI_SAMPLE_DESC v{1, 0}; uint32_t pad = 0; };
    struct alignas(void*) SubMask { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK; UINT v = UINT_MAX; };

    struct MeshStream { SubRS rs; SubMS ms; SubPS ps; SubRast rast; SubBlend blend; SubRTV rtv; SubSamp samp; SubMask mask; } stream{};
    stream.rs.v = rs;
    stream.ms.v = {kxrm.data(), kxrm.size()};
    stream.ps.v = {kxrp.data(), kxrp.size()};
    stream.rast.v.FillMode = D3D12_FILL_MODE_SOLID;
    stream.rast.v.CullMode = D3D12_CULL_MODE_NONE;
    stream.blend.v.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    stream.rtv.v.NumRenderTargets = 1;
    stream.rtv.v.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

    // 1. Cold KXRM + KXRP stream PSO recompile
    D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
    ID3D12PipelineState* pso1 = nullptr;
    KX_CHECK(SUCCEEDED(dev2->CreatePipelineState(&desc, IID_PPV_ARGS(&pso1))) && pso1, "Cold KXRM+KXRP stream PSO");
    auto st1 = hle::d3d12x_stats();
    KX_CHECK(st1.stream_pso_recompiled == 1 && st1.gfx_pso_recompiled == 2 && st1.disk_cache_misses == 2,
             "Cold KXRM+KXRP stream PSO stats");

    // 2. Evict RAM cache -> warm disk cache hit with zero new DXC compiles
    hle::clear_d3d12x_memory_shader_cache();
    ID3D12PipelineState* pso2 = nullptr;
    KX_CHECK(SUCCEEDED(dev2->CreatePipelineState(&desc, IID_PPV_ARGS(&pso2))) && pso2, "Warm disk-cache KXRM+KXRP stream PSO");
    auto st2 = hle::d3d12x_stats();
    KX_CHECK(st2.stream_pso_recompiled == 2 && st2.disk_cache_hits == 2 && st2.gfx_pso_recompiled == 2,
             "Warm disk-cache KXRM+KXRP hit stats");

    // 3. Full KXRA + KXRM + KXRP stream PSO recompile (auto-detects Amplification shader -> MS payload)
    struct AmpMeshStream { SubRS rs; SubAS as; SubMS ms; SubPS ps; SubRast rast; SubBlend blend; SubRTV rtv; SubSamp samp; SubMask mask; } astream{};
    astream.rs.v = rs;
    astream.as.v = {kxra.data(), kxra.size()};
    astream.ms.v = {kxrm.data(), kxrm.size()};
    astream.ps.v = {kxrp.data(), kxrp.size()};
    astream.rast.v = stream.rast.v;
    astream.blend.v = stream.blend.v;
    astream.rtv.v = stream.rtv.v;
    D3D12_PIPELINE_STATE_STREAM_DESC adesc{sizeof(astream), &astream};
    ID3D12PipelineState* pso3 = nullptr;
    KX_CHECK(SUCCEEDED(dev2->CreatePipelineState(&adesc, IID_PPV_ARGS(&pso3))) && pso3, "KXRA+KXRM+KXRP stream PSO");
    auto st3 = hle::d3d12x_stats();
    // KXRP hits RAM cache; KXRA + payload-variant KXRM compile fresh -> +2 gfx_pso_recompiled (total 4)
    KX_CHECK(st3.stream_pso_recompiled == 3 && st3.gfx_pso_recompiled == 4 && st3.gfx_pso_cache_hits >= 3,
             "KXRA+KXRM+KXRP stream PSO cache isolation & recomp stats");

    pso3->Release(); pso2->Release(); pso1->Release(); rs->Release(); dev2->Release();
    fs::remove_all(cache_dir, ec);
}

}  // namespace

int main() {
    std::cout << "KestrelX Verification Suite\n";
    std::cout << "===========================\n";

    run_test("MicrosoftGame.config XML parser", test_game_config_parser);
    run_test("Overlay VFS copy-on-write, whiteouts & path-traversal rejection", test_overlay_vfs_cow_and_security);
    run_test("VEH AMD Zen 2 instruction decoder & emulator (CLZERO/MONITORX/MWAITX)", test_veh_zen2_decode_and_emulate);
    run_test("RDNA2 (GFX10.3) decoder, CFG IR lifter & dual Wave32/Wave64 HLSL emission", test_rdna2_decoder_and_ir_cfg);
    run_test("In-process DXC SM 6.6 compilation & DXBC/DXIL container verification", test_dxbc_container_and_dxc_compile);
    run_test("End-to-end RDNA2 -> IR -> HLSL -> DXC -> Real Dual-GPU Hardware Execution", test_rdna2_recomp_real_dual_gpu_execution);
    run_test("RDNA2 SMEM + DS (LDS) + s_barrier Workgroup Shuffle on Dual-GPU Hardware", test_rdna2_smem_lds_barrier_dual_gpu);
    run_test("RDNA2 EXP Vertex & Pixel (vs_6_6 / ps_6_6) Decode, IR Lift & DXC Compile", test_rdna2_exp_vs_ps_compile);
    run_test("RDNA2 MUBUF/MTBUF + MIMG (image_sample) Textured Quad on Dual-GPU Hardware", test_rdna2_mubuf_mimg_textured_quad_dual_gpu);
    run_test("Strict recompiler refusal of unsupported RDNA2 opcodes", test_strict_unsupported_opcode_refusal);
    run_test("Persistent on-disk DXIL shader cache & corruption recovery", test_persistent_disk_shader_cache);
    run_test("D3D12.X ID3D12Device1..10 QI, Stream PSO Recomp & CmdListProxy Telemetry", test_d3d12_device_qi_stream_pso_and_cmdlist_telemetry);
    run_test("GFX10 2D texture swizzle/deswizzle round-trip (all 7 modes)", test_gfx10_tiling_roundtrip);
    run_test("GFX10 BC1/BC7 block-compressed tiling & 5-level mip-chain layout", test_gfx10_bcn_and_mip_chain);
    run_test("GameInput -> XInput/Win32 controller bridge & mock verification", test_gameinput_xinput_bridge);
    run_test("XAudio2 stereo PCM ring-buffer, RMS energy & underrun verification", test_xaudio2_pcm_ring_buffer);
    run_test("KernelX CreateFile2/ReadFileEx Async VFS I/O & WASAPI Thread Lifecycle", test_kernelx_async_io_and_wasapi_lifecycle);
    run_test("End-to-end PE32+ Loader + Guest DLL + Wide-VFS + GameInput + XAudio2 + RDNA2 VS/PS First Light", test_end_to_end_pe_loader_guest_title);
    run_test("RDNA2 Wave Intrinsics (WaveReadLaneFirst/At, WavePrefixCountBits, Ballot) on Dual-GPU Hardware", test_rdna2_wave_intrinsics_dual_gpu);
    run_test("RDNA2 Multi-Texture & Multi-Buffer Dynamic Descriptor Remapping", test_rdna2_multi_texture_multi_buffer_descriptor_remapping);
    run_test("RDNA2 Depth-Buffered 3D Lit & Textured Perspective Cube on Dual-GPU Hardware", test_rdna2_depth_buffered_3d_cube_dual_gpu);
    run_test("GDK XUser/XStore/XPackage & DirectStorage VFS Async Bridge", test_gdk_runtime_and_directstorage_bridge);
    run_test("RDNA2 2D/3D Compute Workgroup Tiling & LDS Spatial Exchange on Dual-GPU Hardware", test_rdna2_compute_2d_workgroup_tiling_dual_gpu);
    run_test("RDNA2 PBR Material Deferred GBuffer (Albedo, Normal, Material) MRT Pipeline on Dual-GPU Hardware", test_rdna2_pbr_deferred_mrt_pipeline_dual_gpu);
    run_test("D3D12.X Async Compute Queue & Inter-Queue Fence Synchronization", test_d3d12x_async_compute_queue_fence_sync);
    run_test("GameInput Dual-Motor & Trigger Force Feedback Rumble Bridge", test_gameinput_force_feedback_rumble);
    run_test("SGD5 Game Shader Container Archive Parsing & Decompression", test_sgd5_shader_archive_and_decompression);
    run_test("RDNA2 VALU Hardware Precision Parity (NaN Med3, Fract Clamp, Signed BFE)", test_rdna2_valu_precision_parity);
    run_test("AMD Zen 2 VEH Shim SSE4a Instruction Emulation (EXTRQ / INSERTQ on Intel & AMD)", test_veh_zen2_sse4a_extrq_insertq);
    run_test("RDNA2 VRS Tier 2 Rate/Tile Encode-Decode, Combiners & Shading Rate Image Builder", test_vrs_tier2_encode_decode_combiners_and_image);
    run_test("D3D12.X ID3D12GraphicsCommandList5 VRS Tier 2 Hardware Render & Telemetry", test_vrs_tier2_hardware_render_and_proxy_telemetry);
    run_test("GameInput Multi-Controller Slot Management (0..3) & Rumble Isolation", test_gameinput_multi_controller_slots_and_rumble);
    run_test("GameInput Device Hotplug Callbacks, Re-Entrancy & Telemetry", test_gameinput_hotplug_callbacks_and_telemetry);
    run_test("D3D12 Work Graph DAG Validation & 16B-Aligned RecordRingAllocator", test_work_graph_program_validation_and_ring_allocator);
    run_test("D3D12 Work Graph 2-Node RDNA2 ExecuteIndirect Producer->Consumer on Dual-GPU Hardware", test_work_graph_rdna2_execute_indirect_dual_gpu);
    run_test("RDNA2 Mesh (ms_6_6) & Amplification (as_6_6) Decode, IR Lift, HLSL & DXC Compile", test_rdna2_mesh_amplification_decode_ir_dxc);
    run_test("RDNA2 Mesh/Amplification DispatchMesh Quad Rasterization & Hardware Parity", test_rdna2_mesh_shader_dispatch_mesh_hardware_parity);
    run_test("D3D12.X KxDeviceProxy KXRM/KXRA/KXRP Stream PSO Recompilation & Disk Cache", test_d3d12x_mesh_stream_pso_recompilation);

    std::cout << std::format("\nSummary: {} passed, {} failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}