#include "kestrel/gpu/shader_census.hpp"

#include "kestrel/formats/sgd5.hpp"
#include "kestrel/gpu/dxc_compiler.hpp"
#include "kestrel/gpu/ir.hpp"
#include "kestrel/gpu/rdna2_isa.hpp"
#include "kestrel/gpu/shader_container.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace kestrel::gpu {

namespace {

uint32_t read_u32_le(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

bool read_file_bytes(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream ifs(p, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    const auto sz = ifs.tellg();
    if (sz <= 0) {
        out.clear();
        return sz == 0;
    }
    out.resize(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    return static_cast<bool>(ifs.read(reinterpret_cast<char*>(out.data()), sz));
}

bool has_shader_extension(const fs::path& p) {
    const std::string ext = to_lower_ascii(p.extension().string());
    return ext == ".sgd" || ext == ".sgd5" || ext == ".rpf" || ext == ".dxbc" ||
           ext == ".cso" || ext == ".bin" || ext == ".rdna2" || ext == ".kxrn" ||
           ext == ".kxrv" || ext == ".kxrp" || ext == ".kxrm" || ext == ".kxra";
}

struct KxHeader {
    bool matched = false;
    ShaderStage stage = ShaderStage::Compute;
    uint32_t code_bytes = 0;
};

KxHeader try_parse_kx_header(std::span<const uint8_t> data) {
    KxHeader hdr{};
    if (data.size() < 8) return hdr;
    const char* m = reinterpret_cast<const char*>(data.data());
    if (std::memcmp(m, "KXRN", 4) == 0) {
        hdr.matched = true;
        hdr.stage = ShaderStage::Compute;
    } else if (std::memcmp(m, "KXRV", 4) == 0) {
        hdr.matched = true;
        hdr.stage = ShaderStage::Vertex;
    } else if (std::memcmp(m, "KXRP", 4) == 0) {
        hdr.matched = true;
        hdr.stage = ShaderStage::Pixel;
    } else if (std::memcmp(m, "KXRM", 4) == 0) {
        hdr.matched = true;
        hdr.stage = ShaderStage::Mesh;
    } else if (std::memcmp(m, "KXRA", 4) == 0) {
        hdr.matched = true;
        hdr.stage = ShaderStage::Amplification;
    }
    if (hdr.matched) {
        hdr.code_bytes = read_u32_le(data.data() + 4);
    }
    return hdr;
}

ShaderStage infer_stage_from_decoded(
    const std::vector<DecodedInst>& decoded,
    formats::Sgd5ShaderKind hint_kind) {
    bool has_pos = false;
    bool has_mrt = false;
    bool has_prim = false;
    bool has_mesh_alloc = false;
    bool has_dispatch_mesh = false;
    for (const auto& d : decoded) {
        if (d.enc == Encoding::EXP) {
            if (d.opcode >= 12 && d.opcode <= 15) has_pos = true;
            else if (d.opcode <= 7) has_mrt = true;
            else if (d.opcode == kExpTargetPrim0) has_prim = true;
        } else if (d.enc == Encoding::SOPP && d.mnemonic == "s_sendmsg") {
            const uint16_t msg_id = static_cast<uint16_t>(d.simm16) & 0xFu;
            if (msg_id == kSendMsgGsAllocReq) has_mesh_alloc = true;
            else if (msg_id == kSendMsgDispatchMesh) has_dispatch_mesh = true;
        }
    }
    if (has_dispatch_mesh) return ShaderStage::Amplification;
    if (has_prim || has_mesh_alloc || hint_kind == formats::Sgd5ShaderKind::MeshShader) {
        return ShaderStage::Mesh;
    }
    if (has_pos) return ShaderStage::Vertex;
    if (has_mrt) return ShaderStage::Pixel;
    return ShaderStage::Compute;
}

const char* dxil_profile_for_stage(ShaderStage s) {
    switch (s) {
        case ShaderStage::Compute: return "cs_6_6";
        case ShaderStage::Vertex: return "vs_6_6";
        case ShaderStage::Pixel: return "ps_6_6";
        case ShaderStage::Mesh: return "ms_6_6";
        case ShaderStage::Amplification: return "as_6_6";
    }
    return "cs_6_6";
}

StageCensusBreakdown& stage_bucket(ShaderCensusReport& r, ShaderStage s) {
    switch (s) {
        case ShaderStage::Compute: return r.compute;
        case ShaderStage::Vertex: return r.vertex;
        case ShaderStage::Pixel: return r.pixel;
        case ShaderStage::Mesh: return r.mesh;
        case ShaderStage::Amplification: return r.amplification;
    }
    return r.compute;
}

struct CensusCtx {
    ShaderCensusReport report;
    ShaderCensusOptions opt;
    DxcCompiler dxc;
    bool dxc_ready = false;
    std::unordered_map<std::string, uint64_t> unknown_ops;

    bool limit_reached() const {
        return opt.max_shaders > 0 && report.total_entries >= opt.max_shaders;
    }

    void add_failure(const std::string& msg) {
        if (report.sample_failures.size() < 24) {
            report.sample_failures.push_back(msg);
        }
    }

    void analyze_rdna2_stream(
        std::string_view label,
        const uint8_t* bytes,
        size_t size,
        bool stage_known,
        ShaderStage known_stage,
        formats::Sgd5ShaderKind sgd_hint) {
        if (size < 4 || (size % 4) != 0) {
            add_failure(std::format("{}: non-dword-aligned microcode ({} bytes)", label, size));
            return;
        }

        const size_t nwords = size / 4;
        std::vector<uint32_t> words(nwords);
        std::memcpy(words.data(), bytes, size);

        auto decoded = decode_rdna2(words.data(), words.size());
        if (decoded.empty()) {
            add_failure(std::format("{}: empty decode", label));
            return;
        }

        const ShaderStage stage = stage_known ? known_stage : infer_stage_from_decoded(decoded, sgd_hint);
        StageCensusBreakdown& bucket = stage_bucket(report, stage);
        ++bucket.total;

        report.total_instructions += decoded.size();

        bool all_decoded = true;
        for (const auto& inst : decoded) {
            if (inst.enc == Encoding::Unknown) {
                all_decoded = false;
            }
        }
        if (all_decoded) {
            ++report.decoded_ok;
            ++bucket.decoded_ok;
        }

        auto ir = lift_rdna2_to_ir(decoded);
        report.unknown_instructions += ir.unknown_count;
        for (const auto& blk : ir.blocks) {
            for (const auto& inst : blk.insts) {
                if (inst.op == IrOp::Unknown) {
                    ++unknown_ops[inst.subop.empty() ? "<unknown>" : inst.subop];
                }
            }
        }

        if (ir.unknown_count > 0) {
            add_failure(std::format("{}: {} unsupported RDNA2 instruction(s)", label, ir.unknown_count));
            return;
        }
        ++report.ir_lifted_ok;
        ++bucket.ir_lifted_ok;

        RecompOptions ropt{};
        ropt.stage = stage;
        ropt.wave_mode = opt.wave_mode;
        ropt.enable_ir_opt = true;
        auto recomp = emit_hlsl_from_ir(ir, ropt);
        if (recomp.hlsl.empty()) {
            add_failure(std::format("{}: empty HLSL emission", label));
            return;
        }
        ++report.hlsl_emitted_ok;
        ++bucket.hlsl_emitted_ok;

        if (!opt.compile_dxil || !dxc_ready) return;

        auto compiled = dxc.compile_hlsl(recomp.hlsl, "main", dxil_profile_for_stage(stage));
        if (!compiled) {
            add_failure(std::format("{}: DXC failed ({})", label, compiled.error().message));
            return;
        }
        ++report.dxc_compiled_ok;
        ++bucket.dxc_compiled_ok;
        report.total_dxil_bytes += compiled->dxil.size();
    }

    void analyze_blob(
        std::string_view label,
        std::span<const uint8_t> data,
        formats::Sgd5ShaderKind sgd_hint) {
        if (limit_reached()) return;
        ++report.total_entries;

        if (data.size() >= 4 && std::memcmp(data.data(), "DXBC", 4) == 0) {
            auto parsed = parse_dxbc_container(data.data(), data.size());
            if (!parsed) {
                add_failure(std::format("{}: invalid DXBC ({})", label, parsed.error().message));
                return;
            }
            if (parsed->has_dxil && !parsed->has_xbox_native) {
                ++report.already_dxil;
                return;
            }
            for (const auto& ch : parsed->chunks) {
                if (ch.fourcc == "XBIN" || ch.fourcc == "XBCH" || ch.fourcc == "XDAT") {
                    const KxHeader kh = try_parse_kx_header(ch.data);
                    if (kh.matched && kh.code_bytes > 0 && 8ull + kh.code_bytes <= ch.data.size()) {
                        analyze_rdna2_stream(label, ch.data.data() + 8, kh.code_bytes, true, kh.stage, sgd_hint);
                        return;
                    }
                    if (!ch.data.empty() && (ch.data.size() % 4) == 0) {
                        analyze_rdna2_stream(label, ch.data.data(), ch.data.size(), false, ShaderStage::Compute, sgd_hint);
                        return;
                    }
                }
            }
            add_failure(std::format("{}: DXBC container has no DXIL or Xbox microcode chunk", label));
            return;
        }

        const KxHeader kh = try_parse_kx_header(data);
        if (kh.matched) {
            if (kh.code_bytes == 0 || 8ull + kh.code_bytes > data.size() || (kh.code_bytes % 4) != 0) {
                add_failure(std::format("{}: malformed KX shader container size={}", label, kh.code_bytes));
                return;
            }
            analyze_rdna2_stream(label, data.data() + 8, kh.code_bytes, true, kh.stage, sgd_hint);
            return;
        }

        analyze_rdna2_stream(label, data.data(), data.size(), false, ShaderStage::Compute, sgd_hint);
    }

    void scan_file(const fs::path& file_path) {
        if (limit_reached()) return;
        std::vector<uint8_t> bytes;
        if (!read_file_bytes(file_path, bytes)) {
            add_failure(std::format("{}: failed to read file", file_path.filename().string()));
            return;
        }
        ++report.files_scanned;

        const bool is_sgd = formats::Sgd5Archive::is_sgd5(bytes);
        auto records = formats::Sgd5Archive::parse_records(bytes);
        if (is_sgd || !records.empty()) {
            ++report.sgd5_archives;
            for (size_t i = 0; i < records.size(); ++i) {
                if (limit_reached()) break;
                const auto& rec = records[i];
                const std::string label = rec.name.empty()
                    ? std::format("{}#{}", file_path.filename().string(), i)
                    : rec.name;
                auto unpacked = rec.unpack();
                if (!unpacked) {
                    ++report.total_entries;
                    ++report.unpack_failed;
                    add_failure(std::format("{}: SGD5 DEFLATE unpack failed (packed={} B, expected={} B)",
                                            label, rec.packed_size, rec.unpacked_size));
                    continue;
                }
                ++report.unpacked_ok;
                analyze_blob(label, *unpacked, rec.kind);
            }
            return;
        }

        analyze_blob(file_path.filename().string(), bytes, formats::Sgd5ShaderKind::Unknown);
    }
};

}  // namespace

Result<ShaderCensusReport> run_shader_census(
    const fs::path& target,
    const ShaderCensusOptions& opt) {
    std::error_code ec;
    if (!fs::exists(target, ec)) {
        return fail("Shader census target does not exist: " + target.string());
    }

    CensusCtx ctx;
    ctx.opt = opt;
    ctx.report.input_path = target.string();
    ctx.report.wave_mode = opt.wave_mode;
    ctx.report.dxc_enabled = opt.compile_dxil;

    if (opt.compile_dxil) {
        if (auto init = ctx.dxc.initialize(); init) {
            ctx.dxc_ready = true;
            ctx.report.dxc_available = true;
        }
    }

    if (fs::is_regular_file(target, ec)) {
        ctx.scan_file(target);
    } else if (fs::is_directory(target, ec)) {
        std::vector<fs::path> files;
        for (fs::recursive_directory_iterator it(target, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (ec) continue;
            if (it->is_regular_file(ec) && has_shader_extension(it->path())) {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            if (ctx.limit_reached()) break;
            ctx.scan_file(f);
        }
    } else {
        return fail("Shader census target is neither a file nor directory: " + target.string());
    }

    ctx.report.top_unknown_opcodes.reserve(ctx.unknown_ops.size());
    for (const auto& [op, cnt] : ctx.unknown_ops) {
        ctx.report.top_unknown_opcodes.push_back({op, cnt});
    }
    std::sort(ctx.report.top_unknown_opcodes.begin(), ctx.report.top_unknown_opcodes.end(),
              [](const OpcodeCount& a, const OpcodeCount& b) {
                  if (a.count != b.count) return a.count > b.count;
                  return a.mnemonic < b.mnemonic;
              });
    if (ctx.report.top_unknown_opcodes.size() > 32) {
        ctx.report.top_unknown_opcodes.resize(32);
    }

    return ctx.report;
}

std::string format_shader_census_text(const ShaderCensusReport& r) {
    auto pct = [](uint64_t num, uint64_t den) -> double {
        return den == 0 ? 0.0 : (100.0 * static_cast<double>(num) / static_cast<double>(den));
    };
    const uint64_t rdna2_entries = (r.total_entries >= r.already_dxil + r.unpack_failed)
                                       ? (r.total_entries - r.already_dxil - r.unpack_failed)
                                       : 0;
    std::ostringstream oss;
    oss << std::format(
        "KestrelX Shader Census Report\n"
        "  Target        : {}\n"
        "  Wave Mode     : Wave{} (DXC={})\n"
        "  Files / SGD5  : {} file(s), {} SGD5 archive(s)\n"
        "  Total Entries : {} (unpacked={}, unpack_err={}, pre_dxil={})\n"
        "  RDNA2 Decoded : {} / {} ({:.1f}%)\n"
        "  IR Clean      : {} / {} ({:.1f}%)\n"
        "  HLSL Emitted  : {} / {} ({:.1f}%)\n"
        "  DXIL Compiled : {} / {} ({:.1f}%, {} bytes total)\n"
        "  Instructions  : {} total, {} unsupported ({:.2f}% supported)\n",
        r.input_path,
        static_cast<uint32_t>(r.wave_mode),
        r.dxc_enabled ? (r.dxc_available ? "active" : "unavailable") : "disabled",
        r.files_scanned, r.sgd5_archives,
        r.total_entries, r.unpacked_ok, r.unpack_failed, r.already_dxil,
        r.decoded_ok, rdna2_entries, pct(r.decoded_ok, rdna2_entries),
        r.ir_lifted_ok, rdna2_entries, pct(r.ir_lifted_ok, rdna2_entries),
        r.hlsl_emitted_ok, rdna2_entries, pct(r.hlsl_emitted_ok, rdna2_entries),
        r.dxc_compiled_ok, rdna2_entries, pct(r.dxc_compiled_ok, rdna2_entries), r.total_dxil_bytes,
        r.total_instructions, r.unknown_instructions,
        r.total_instructions == 0 ? 100.0 : pct(r.total_instructions - r.unknown_instructions, r.total_instructions));

    auto print_stage = [&](const char* name, const StageCensusBreakdown& b) {
        if (b.total == 0) return;
        oss << std::format("    {:<14}: total={}, decoded={}, ir_clean={}, hlsl={}, dxil={}\n",
                           name, b.total, b.decoded_ok, b.ir_lifted_ok, b.hlsl_emitted_ok, b.dxc_compiled_ok);
    };
    if (rdna2_entries > 0) {
        oss << "  Stage Breakdown:\n";
        print_stage("Compute", r.compute);
        print_stage("Vertex", r.vertex);
        print_stage("Pixel", r.pixel);
        print_stage("Mesh", r.mesh);
        print_stage("Amplification", r.amplification);
    }

    if (!r.top_unknown_opcodes.empty()) {
        oss << "  Top Unsupported RDNA2 Opcodes:\n";
        for (const auto& u : r.top_unknown_opcodes) {
            oss << std::format("    {:<28} {}\n", u.mnemonic, u.count);
        }
    }
    return oss.str();
}

std::string format_shader_census_json(const ShaderCensusReport& r) {
    auto stage_json = [](const StageCensusBreakdown& b) {
        return std::format(
            "{{\"total\":{},\"decoded_ok\":{},\"ir_lifted_ok\":{},\"hlsl_emitted_ok\":{},\"dxc_compiled_ok\":{}}}",
            b.total, b.decoded_ok, b.ir_lifted_ok, b.hlsl_emitted_ok, b.dxc_compiled_ok);
    };
    std::ostringstream oss;
    oss << "{\n"
        << std::format("  \"input_path\": \"{}\",\n", json_escape(r.input_path))
        << std::format("  \"wave_mode\": {},\n", static_cast<uint32_t>(r.wave_mode))
        << std::format("  \"dxc_enabled\": {},\n", r.dxc_enabled ? "true" : "false")
        << std::format("  \"dxc_available\": {},\n", r.dxc_available ? "true" : "false")
        << std::format("  \"files_scanned\": {},\n", r.files_scanned)
        << std::format("  \"sgd5_archives\": {},\n", r.sgd5_archives)
        << std::format("  \"total_entries\": {},\n", r.total_entries)
        << std::format("  \"unpacked_ok\": {},\n", r.unpacked_ok)
        << std::format("  \"unpack_failed\": {},\n", r.unpack_failed)
        << std::format("  \"already_dxil\": {},\n", r.already_dxil)
        << std::format("  \"decoded_ok\": {},\n", r.decoded_ok)
        << std::format("  \"ir_lifted_ok\": {},\n", r.ir_lifted_ok)
        << std::format("  \"hlsl_emitted_ok\": {},\n", r.hlsl_emitted_ok)
        << std::format("  \"dxc_compiled_ok\": {},\n", r.dxc_compiled_ok)
        << std::format("  \"total_instructions\": {},\n", r.total_instructions)
        << std::format("  \"unknown_instructions\": {},\n", r.unknown_instructions)
        << std::format("  \"total_dxil_bytes\": {},\n", r.total_dxil_bytes)
        << "  \"stages\": {\n"
        << std::format("    \"compute\": {},\n", stage_json(r.compute))
        << std::format("    \"vertex\": {},\n", stage_json(r.vertex))
        << std::format("    \"pixel\": {},\n", stage_json(r.pixel))
        << std::format("    \"mesh\": {},\n", stage_json(r.mesh))
        << std::format("    \"amplification\": {}\n", stage_json(r.amplification))
        << "  },\n"
        << "  \"top_unknown_opcodes\": [";
    for (size_t i = 0; i < r.top_unknown_opcodes.size(); ++i) {
        if (i > 0) oss << ",";
        oss << std::format("{{\"mnemonic\":\"{}\",\"count\":{}}}",
                           json_escape(r.top_unknown_opcodes[i].mnemonic),
                           r.top_unknown_opcodes[i].count);
    }
    oss << "],\n"
        << "  \"sample_failures\": [";
    for (size_t i = 0; i < r.sample_failures.size(); ++i) {
        if (i > 0) oss << ",";
        oss << std::format("\"{}\"", json_escape(r.sample_failures[i]));
    }
    oss << "]\n}\n";
    return oss.str();
}

}  // namespace kestrel::gpu
