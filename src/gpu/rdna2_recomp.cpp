#include "kestrel/gpu/rdna2_recomp.hpp"

#include <algorithm>
#include <format>
#include <sstream>

namespace kestrel::gpu {

namespace {

std::string emit_u32(const IrOperand& op) {
    switch (op.kind) {
        case IrOperand::Kind::Sgpr: return std::format("s[{}]", op.reg);
        case IrOperand::Kind::Vgpr: return std::format("v[{}]", op.reg);
        case IrOperand::Kind::InlineInt: return std::format("asuint(int({}))", op.imm_i);
        case IrOperand::Kind::InlineFloat: return std::format("asuint(float({:.9g}))", op.imm_f);
        case IrOperand::Kind::Literal: return std::format("{}u", op.lit_u32);
        case IrOperand::Kind::Scc: return "uint(scc ? 1u : 0u)";
        case IrOperand::Kind::Vcc: return "uint(vcc ? 1u : 0u)";
        case IrOperand::Kind::Exec: return "1u";
        default: return "0u";
    }
}

std::string emit_f32(const IrOperand& op) {
    std::string base;
    switch (op.kind) {
        case IrOperand::Kind::InlineFloat: base = std::format("float({:.9g})", op.imm_f); break;
        case IrOperand::Kind::InlineInt: base = std::format("asfloat(asuint(int({})))", op.imm_i); break;
        default: base = std::format("asfloat({})", emit_u32(op)); break;
    }
    if (op.abs) base = std::format("abs({})", base);
    if (op.neg) base = std::format("(-({}))", base);
    return base;
}

std::string wrap_clamp(const std::string& expr, bool clamp) {
    return clamp ? std::format("saturate({})", expr) : expr;
}

void emit_inst(std::ostringstream& out, const IrInst& i, const char* ind, ShaderStage stage) {
    switch (i.op) {
        case IrOp::Nop:
            break;
        case IrOp::SMov:
            out << ind << std::format("s[{}] = {};\n", i.dst.reg, emit_u32(i.src0));
            break;
        case IrOp::SAddU32:
            out << ind << std::format("s[{}] = {} + {};\n", i.dst.reg, emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::SSubU32:
            out << ind << std::format("s[{}] = {} - {};\n", i.dst.reg, emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::SMulI32:
            out << ind << std::format("s[{}] = asuint(asint({}) * asint({}));\n", i.dst.reg, emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::SAndB32:
            out << ind << std::format("s[{}] = {} & {};\n", i.dst.reg, emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::SOrB32:
            out << ind << std::format("s[{}] = {} | {};\n", i.dst.reg, emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::SXorB32:
            out << ind << std::format("s[{}] = {} ^ {};\n", i.dst.reg, emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::SLshlB32:
            out << ind << std::format("s[{}] = {} << ({} & 31u);\n", i.dst.reg, emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::SLshrB32:
            out << ind << std::format("s[{}] = {} >> ({} & 31u);\n", i.dst.reg, emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::SAshrI32:
            out << ind << std::format("s[{}] = asuint(asint({}) >> ({} & 31u));\n", i.dst.reg, emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::SCmp: {
            const std::string a = emit_u32(i.src0), b = emit_u32(i.src1);
            if (i.subop == "s_cmp_eq_i32" || i.subop == "s_cmp_eq_u32") out << ind << std::format("scc = ({} == {});\n", a, b);
            else if (i.subop == "s_cmp_lg_i32" || i.subop == "s_cmp_lg_u32") out << ind << std::format("scc = ({} != {});\n", a, b);
            else if (i.subop == "s_cmp_lt_i32") out << ind << std::format("scc = (asint({}) < asint({}));\n", a, b);
            else if (i.subop == "s_cmp_le_i32") out << ind << std::format("scc = (asint({}) <= asint({}));\n", a, b);
            else if (i.subop == "s_cmp_gt_i32") out << ind << std::format("scc = (asint({}) > asint({}));\n", a, b);
            else if (i.subop == "s_cmp_ge_i32") out << ind << std::format("scc = (asint({}) >= asint({}));\n", a, b);
            else if (i.subop == "s_cmp_lt_u32") out << ind << std::format("scc = ({} < {});\n", a, b);
            else if (i.subop == "s_cmp_le_u32") out << ind << std::format("scc = ({} <= {});\n", a, b);
            else if (i.subop == "s_cmp_gt_u32") out << ind << std::format("scc = ({} > {});\n", a, b);
            else out << ind << std::format("scc = ({} >= {});\n", a, b);
            break;
        }
        case IrOp::VMov:
            out << ind << std::format("v[{}] = {};\n", i.dst.reg, emit_u32(i.src0));
            break;
        case IrOp::VAddF32:
            out << ind << std::format("v[{}] = asuint({});\n", i.dst.reg, wrap_clamp(std::format("({} + {})", emit_f32(i.src0), emit_f32(i.src1)), i.clamp));
            break;
        case IrOp::VSubF32:
            out << ind << std::format("v[{}] = asuint({});\n", i.dst.reg, wrap_clamp(std::format("({} - {})", emit_f32(i.src0), emit_f32(i.src1)), i.clamp));
            break;
        case IrOp::VSubRevF32:
            out << ind << std::format("v[{}] = asuint({});\n", i.dst.reg, wrap_clamp(std::format("({} - {})", emit_f32(i.src1), emit_f32(i.src0)), i.clamp));
            break;
        case IrOp::VMulF32:
            out << ind << std::format("v[{}] = asuint({});\n", i.dst.reg, wrap_clamp(std::format("({} * {})", emit_f32(i.src0), emit_f32(i.src1)), i.clamp));
            break;
        case IrOp::VFmaF32:
            out << ind << std::format("v[{}] = asuint({});\n", i.dst.reg, wrap_clamp(std::format("mad({}, {}, {})", emit_f32(i.src0), emit_f32(i.src1), emit_f32(i.src2)), i.clamp));
            break;
        case IrOp::VMinF32:
            out << ind << std::format("v[{}] = asuint(min({}, {}));\n", i.dst.reg, emit_f32(i.src0), emit_f32(i.src1));
            break;
        case IrOp::VMaxF32:
            out << ind << std::format("v[{}] = asuint(max({}, {}));\n", i.dst.reg, emit_f32(i.src0), emit_f32(i.src1));
            break;
        case IrOp::VAddU32:
            out << ind << std::format("v[{}] = {} + {};\n", i.dst.reg, emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::VSubU32:
            out << ind << std::format("v[{}] = {} - {};\n", i.dst.reg, emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::VMulLoU32:
            out << ind << std::format("v[{}] = {} * {};\n", i.dst.reg, emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::VAndB32:
            out << ind << std::format("v[{}] = {} & {};\n", i.dst.reg, emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::VOrB32:
            out << ind << std::format("v[{}] = {} | {};\n", i.dst.reg, emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::VXorB32:
            out << ind << std::format("v[{}] = {} ^ {};\n", i.dst.reg, emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::VLshlRevB32:
            out << ind << std::format("v[{}] = {} << ({} & 31u);\n", i.dst.reg, emit_u32(i.src1), emit_u32(i.src0));
            break;
        case IrOp::VLshrRevB32:
            out << ind << std::format("v[{}] = {} >> ({} & 31u);\n", i.dst.reg, emit_u32(i.src1), emit_u32(i.src0));
            break;
        case IrOp::VAshrRevI32:
            out << ind << std::format("v[{}] = asuint(asint({}) >> ({} & 31u));\n", i.dst.reg, emit_u32(i.src1), emit_u32(i.src0));
            break;
        case IrOp::VCvtF32I32:
            out << ind << std::format("v[{}] = asuint(float(asint({})));\n", i.dst.reg, emit_u32(i.src0));
            break;
        case IrOp::VCvtF32U32:
            out << ind << std::format("v[{}] = asuint(float({}));\n", i.dst.reg, emit_u32(i.src0));
            break;
        case IrOp::VCvtI32F32:
            out << ind << std::format("v[{}] = asuint(int({}));\n", i.dst.reg, emit_f32(i.src0));
            break;
        case IrOp::VCvtU32F32:
            out << ind << std::format("v[{}] = uint({});\n", i.dst.reg, emit_f32(i.src0));
            break;
        case IrOp::VRcpF32:
            out << ind << std::format("v[{}] = asuint(rcp({}));\n", i.dst.reg, emit_f32(i.src0));
            break;
        case IrOp::VRsqF32:
            out << ind << std::format("v[{}] = asuint(rsqrt({}));\n", i.dst.reg, emit_f32(i.src0));
            break;
        case IrOp::VSqrtF32:
            out << ind << std::format("v[{}] = asuint(sqrt({}));\n", i.dst.reg, emit_f32(i.src0));
            break;
        case IrOp::VFloorF32:
            out << ind << std::format("v[{}] = asuint(floor({}));\n", i.dst.reg, emit_f32(i.src0));
            break;
        case IrOp::VCeilF32:
            out << ind << std::format("v[{}] = asuint(ceil({}));\n", i.dst.reg, emit_f32(i.src0));
            break;
        case IrOp::VFractF32:
            out << ind << std::format("v[{}] = asuint(frac({}));\n", i.dst.reg, emit_f32(i.src0));
            break;
        case IrOp::VCmp: {
            if (i.subop == "v_cmp_lt_f32") out << ind << std::format("vcc = ({} < {});\n", emit_f32(i.src0), emit_f32(i.src1));
            else if (i.subop == "v_cmp_eq_f32") out << ind << std::format("vcc = ({} == {});\n", emit_f32(i.src0), emit_f32(i.src1));
            else if (i.subop == "v_cmp_le_f32") out << ind << std::format("vcc = ({} <= {});\n", emit_f32(i.src0), emit_f32(i.src1));
            else if (i.subop == "v_cmp_gt_f32") out << ind << std::format("vcc = ({} > {});\n", emit_f32(i.src0), emit_f32(i.src1));
            else if (i.subop == "v_cmp_lg_f32") out << ind << std::format("vcc = ({} != {});\n", emit_f32(i.src0), emit_f32(i.src1));
            else if (i.subop == "v_cmp_ge_f32") out << ind << std::format("vcc = ({} >= {});\n", emit_f32(i.src0), emit_f32(i.src1));
            else if (i.subop == "v_cmp_lt_u32") out << ind << std::format("vcc = ({} < {});\n", emit_u32(i.src0), emit_u32(i.src1));
            else if (i.subop == "v_cmp_gt_u32") out << ind << std::format("vcc = ({} > {});\n", emit_u32(i.src0), emit_u32(i.src1));
            else if (i.subop == "v_cmp_eq_i32" || i.subop == "v_cmp_eq_u32") out << ind << std::format("vcc = ({} == {});\n", emit_u32(i.src0), emit_u32(i.src1));
            else out << ind << std::format("vcc = (asint({}) < asint({}));\n", emit_u32(i.src0), emit_u32(i.src1));
            break;
        }
        case IrOp::VCndMask:
            out << ind << std::format("v[{}] = vcc ? {} : {};\n", i.dst.reg, emit_u32(i.src1), emit_u32(i.src0));
            break;
        case IrOp::SLoadDword: {
            const uint32_t count = (i.subop == "s_load_dwordx4") ? 4u : ((i.subop == "s_load_dwordx2") ? 2u : 1u);
            for (uint32_t k = 0; k < count; ++k) {
                out << ind << std::format("s[{}] = g_mem.Load({} + {}u);\n",
                                          i.dst.reg + k, emit_u32(i.src0), i.offset + k * 4u);
            }
            break;
        }
        case IrOp::GlobalLoadDword:
            out << ind << std::format("v[{}] = g_mem.Load({} + {}u);\n", i.dst.reg, emit_u32(i.src0), i.offset);
            break;
        case IrOp::GlobalStoreDword:
            out << ind << std::format("g_mem.Store({} + {}u, {});\n", emit_u32(i.src0), i.offset, emit_u32(i.src1));
            break;
        case IrOp::DsWriteB32:
            out << ind << std::format("g_lds[(({} + {}u) >> 2u) & 8191u] = {};\n",
                                      emit_u32(i.src0), i.offset, emit_u32(i.src1));
            break;
        case IrOp::DsReadB32:
            out << ind << std::format("v[{}] = g_lds[(({} + {}u) >> 2u) & 8191u];\n",
                                      i.dst.reg, emit_u32(i.src0), i.offset);
            break;
        case IrOp::Barrier:
            out << ind << "GroupMemoryBarrierWithGroupSync();\n";
            break;
        case IrOp::Exp: {
            const char* comps[4] = {"x", "y", "z", "w"};
            const IrOperand* srcs[4] = {&i.src0, &i.src1, &i.src2, &i.src3};
            if (stage == ShaderStage::Vertex) {
                if (i.exp_target == 12) {
                    for (int c = 0; c < 4; ++c) {
                        if (i.exp_en & (1u << c)) {
                            out << ind << std::format("kx_out.pos.{} = {};\n", comps[c], emit_f32(*srcs[c]));
                        }
                    }
                } else if (i.exp_target == 32) {
                    for (int c = 0; c < 4; ++c) {
                        if (i.exp_en & (1u << c)) {
                            out << ind << std::format("kx_out.param0.{} = {};\n", comps[c], emit_f32(*srcs[c]));
                        }
                    }
                } else {
                    out << ind << std::format("#error Unsupported Vertex EXP target: {}\n", i.exp_target);
                }
            } else if (stage == ShaderStage::Pixel) {
                if (i.exp_target == 0) {
                    for (int c = 0; c < 4; ++c) {
                        if (i.exp_en & (1u << c)) {
                            out << ind << std::format("kx_mrt0.{} = {};\n", comps[c], emit_f32(*srcs[c]));
                        }
                    }
                } else {
                    out << ind << std::format("#error Unsupported Pixel EXP target: {}\n", i.exp_target);
                }
            } else {
                out << ind << "#error EXP instruction is not valid in compute shader stage\n";
            }
            break;
        }
        case IrOp::EndPgm:
            if (stage == ShaderStage::Vertex) out << ind << "return kx_out;\n";
            else if (stage == ShaderStage::Pixel) out << ind << "return kx_mrt0;\n";
            else out << ind << "return;\n";
            break;
        default:
            out << ind << std::format("#error Unsupported RDNA2 opcode: {}\n", i.subop);
            break;
    }
}

}  // namespace

RecompResult emit_hlsl_from_ir(const IrFunction& fn, const RecompOptions& opt) {
    RecompResult res{};
    res.wave_mode = opt.wave_mode;
    res.vgpr_count = std::max<uint32_t>(fn.max_vgpr, 4u);
    res.sgpr_count = std::max<uint32_t>(fn.max_sgpr, 4u);
    res.blocks_emitted = static_cast<uint32_t>(fn.blocks.size());

    const char* ret_stmt = (opt.stage == ShaderStage::Vertex) ? "return kx_out;"
                         : (opt.stage == ShaderStage::Pixel)  ? "return kx_mrt0;"
                                                              : "return;";

    std::ostringstream out;
    out << "// Generated by KestrelX RDNA2 (GFX10.3) -> IR -> SM 6.6 HLSL Recompiler\n";
    if (opt.stage == ShaderStage::Compute) {
        out << std::format("// Stage: Compute | Target wave mode: Wave{}\n", static_cast<uint32_t>(opt.wave_mode));
        out << "RWByteAddressBuffer g_mem : register(u0);\n";
        out << "cbuffer KxRoot : register(b0) { uint4 kx_root0; uint4 kx_root1; };\n";
        if (fn.uses_lds) {
            out << "groupshared uint g_lds[8192];\n";
        }
        out << "\n";
        out << std::format("[WaveSize({})]\n", static_cast<uint32_t>(opt.wave_mode));
        out << std::format("[numthreads({}, {}, {})]\n", opt.group_size_x, opt.group_size_y, opt.group_size_z);
        out << std::format("void {}(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {{\n", opt.entry_point);
    } else if (opt.stage == ShaderStage::Vertex) {
        out << "// Stage: Vertex (vs_6_6)\n";
        if (fn.uses_mem) {
            out << "RWByteAddressBuffer g_mem : register(u0);\n";
            out << "cbuffer KxRoot : register(b0) { uint4 kx_root0; uint4 kx_root1; };\n";
        }
        out << "struct KxVsOut { float4 pos : SV_Position; float4 param0 : TEXCOORD0; };\n\n";
        out << std::format("KxVsOut {}(uint sv_vid : SV_VertexID) {{\n", opt.entry_point);
        out << "    KxVsOut kx_out = (KxVsOut)0;\n";
    } else {
        out << "// Stage: Pixel (ps_6_6)\n";
        if (fn.uses_mem) {
            out << "RWByteAddressBuffer g_mem : register(u0);\n";
            out << "cbuffer KxRoot : register(b0) { uint4 kx_root0; uint4 kx_root1; };\n";
        }
        out << "struct KxPsIn { float4 pos : SV_Position; float4 param0 : TEXCOORD0; };\n\n";
        out << std::format("float4 {}(KxPsIn kx_in) : SV_Target0 {{\n", opt.entry_point);
        out << "    float4 kx_mrt0 = float4(0.0, 0.0, 0.0, 1.0);\n";
    }

    out << std::format("    uint v[{}] = (uint[{}])0;\n", res.vgpr_count, res.vgpr_count);
    out << std::format("    uint s[{}] = (uint[{}])0;\n", res.sgpr_count, res.sgpr_count);
    out << "    bool scc = false;\n";
    out << "    bool vcc = false;\n";
    if (opt.stage == ShaderStage::Compute) {
        out << "    v[0] = tid.x;\n";
        out << "    s[0] = kx_root0.x; s[1] = kx_root0.y; s[2] = kx_root0.z; s[3] = kx_root0.w;\n\n";
    } else if (opt.stage == ShaderStage::Vertex) {
        out << "    v[0] = sv_vid;\n";
        if (fn.uses_mem) {
            out << "    s[0] = kx_root0.x; s[1] = kx_root0.y; s[2] = kx_root0.z; s[3] = kx_root0.w;\n";
        }
        out << "\n";
    } else {
        out << "    v[0] = asuint(kx_in.param0.x);\n";
        out << "    v[1] = asuint(kx_in.param0.y);\n";
        out << "    v[2] = asuint(kx_in.param0.z);\n";
        out << "    v[3] = asuint(kx_in.param0.w);\n\n";
    }

    if (fn.blocks.size() <= 1) {
        if (!fn.blocks.empty()) {
            for (const auto& inst : fn.blocks[0].insts) {
                emit_inst(out, inst, "    ", opt.stage);
            }
        }
        if (opt.stage != ShaderStage::Compute) {
            out << std::format("    {}\n", ret_stmt);
        }
    } else {
        out << "    uint bb = 0u;\n";
        out << "    [loop] for (uint kx_iter = 0u; kx_iter < 65536u; ++kx_iter) {\n";
        out << "        switch (bb) {\n";
        for (uint32_t b = 0; b < fn.blocks.size(); ++b) {
            const auto& blk = fn.blocks[b];
            out << std::format("        case {}u: {{\n", b);
            bool terminated = false;
            for (const auto& inst : blk.insts) {
                if (inst.op == IrOp::Branch) {
                    out << std::format("            bb = {}u; break;\n", inst.target_block);
                    terminated = true;
                } else if (inst.op == IrOp::CondBranch) {
                    const uint32_t fallthrough = (b + 1 < fn.blocks.size()) ? (b + 1) : 0xFFFFFFFFu;
                    const char* cond_var = (inst.subop.find("vcc") != std::string::npos) ? "vcc" : "scc";
                    out << std::format("            bb = ({}{}) ? {}u : {}u; break;\n",
                                       inst.branch_on_true ? "" : "!", cond_var, inst.target_block, fallthrough);
                    terminated = true;
                } else if (inst.op == IrOp::EndPgm) {
                    out << std::format("            {}\n", ret_stmt);
                    terminated = true;
                } else {
                    emit_inst(out, inst, "            ", opt.stage);
                }
            }
            if (!terminated) {
                if (b + 1 < fn.blocks.size()) out << std::format("            bb = {}u; break;\n", b + 1);
                else out << std::format("            {}\n", ret_stmt);
            }
            out << "        }\n";
        }
        out << std::format("        default: {}\n", ret_stmt);
        out << "        }\n";
        out << "    }\n";
        if (opt.stage != ShaderStage::Compute) {
            out << std::format("    {}\n", ret_stmt);
        }
    }

    out << "}\n";
    res.hlsl = out.str();
    return res;
}

}  // namespace kestrel::gpu
