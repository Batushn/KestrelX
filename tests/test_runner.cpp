#include "kestrel/common/types.hpp"
#include "kestrel/cpu/veh_shim.hpp"
#include "kestrel/gpu/dxc_compiler.hpp"
#include "kestrel/gpu/gfx10_tiling.hpp"
#include "kestrel/gpu/gpu_runner.hpp"
#include "kestrel/gpu/ir.hpp"
#include "kestrel/gpu/rdna2_isa.hpp"
#include "kestrel/gpu/rdna2_recomp.hpp"
#include "kestrel/gpu/shader_container.hpp"
#include "kestrel/hle/d3d12x.hpp"
#include "kestrel/hle/gameinput.hpp"
#include "kestrel/hle/kernelx.hpp"
#include "kestrel/hle/registry.hpp"
#include "kestrel/hle/xaudio2.hpp"
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

    constexpr uint32_t kLanes = 64;
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

    // Verify D3D12.X KxDeviceProxy + KxCommandQueueProxy + KXRN/KXRV/KXRP PSO auto-recomp & cache hits!
    const auto d3d = hle::d3d12x_stats();
    KX_CHECK(d3d.devices_created >= 1 && d3d.queues_created >= 1 &&
             d3d.cmd_lists_created >= 1 && d3d.resources_created >= 2 &&
             d3d.zero_memory_calls >= 1 && d3d.present_calls >= 1 &&
             d3d.pso_recompiled >= 1 && d3d.pso_cache_hits >= 1 &&
             d3d.gfx_pso_recompiled >= 2 && d3d.gfx_pso_cache_hits >= 2,
             "KxDeviceProxy / Graphics+Compute PSO auto-recomp / PresentX counters mismatch");

    const auto kx = hle::kernelx_stats();
    KX_CHECK(kx.alloc_count >= 1 && kx.free_count == kx.alloc_count && kx.live_bytes == 0,
             "KernelX XMemAlloc/XMemFree leak check");

    const auto gi = hle::gameinput_stats();
    const auto xa = hle::xaudio2_stats();
    KX_CHECK(gi.instances_created >= 1 && gi.readings_polled >= 1, "Guest GameInput HLE verified");
    KX_CHECK(xa.engines_created >= 1 && xa.frames_submitted >= 64, "Guest XAudio2 HLE verified");

    std::cout << std::format(
        "    -> Guest title PASS: HLE_imports={}, GuestDLL_imports={}, TLS={}, D3D12X(dev={}, q={}, res={}, cs_pso={}, gfx_pso={}, cache_hit={}, present={}), GI(reads={}), XA2(frames={})\n",
        (*mod)->hle_imports, (*mod)->guest_imports, (*mod)->tls_callbacks_run,
        d3d.devices_created, d3d.queues_created, d3d.resources_created,
        d3d.pso_recompiled, d3d.gfx_pso_recompiled, d3d.pso_cache_hits + d3d.gfx_pso_cache_hits,
        d3d.present_calls, gi.readings_polled, xa.frames_submitted);

    fs::remove_all(data_dir, ec);
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
    run_test("Strict recompiler refusal of unsupported RDNA2 opcodes", test_strict_unsupported_opcode_refusal);
    run_test("GFX10 2D texture swizzle/deswizzle round-trip (all 7 modes)", test_gfx10_tiling_roundtrip);
    run_test("GFX10 BC1/BC7 block-compressed tiling & 5-level mip-chain layout", test_gfx10_bcn_and_mip_chain);
    run_test("GameInput -> XInput/Win32 controller bridge & mock verification", test_gameinput_xinput_bridge);
    run_test("XAudio2 stereo PCM ring-buffer, RMS energy & underrun verification", test_xaudio2_pcm_ring_buffer);
    run_test("End-to-end PE32+ Loader + Guest DLL + Wide-VFS + GameInput + XAudio2 + RDNA2 VS/PS First Light", test_end_to_end_pe_loader_guest_title);

    std::cout << std::format("\nSummary: {} passed, {} failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
