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
        case IrOperand::Kind::M0: return "m0";
        default: return "0u";
    }
}

std::string emit_sdst(const IrOperand& op) {
    return (op.kind == IrOperand::Kind::M0) ? "m0" : std::format("s[{}]", op.reg);
}

struct EmitContext {
    ShaderStage stage = ShaderStage::Compute;
    bool is_mrt = false;
    uint32_t max_mesh_verts = 64;
    uint32_t max_mesh_prims = 64;
    uint32_t mesh_param_mask = 0;
    bool multi_block = false;
};

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

void emit_inst(std::ostringstream& out, const IrInst& i, const char* ind, const EmitContext& ctx) {
    const ShaderStage stage = ctx.stage;
    const bool is_mrt = ctx.is_mrt;
    switch (i.op) {
        case IrOp::Nop:
            break;
        case IrOp::SMov:
            out << ind << std::format("{} = {};\n", emit_sdst(i.dst), emit_u32(i.src0));
            break;
        case IrOp::SAddU32:
            out << ind << std::format("{} = {} + {};\n", emit_sdst(i.dst), emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::SSubU32:
            out << ind << std::format("{} = {} - {};\n", emit_sdst(i.dst), emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::SMulI32:
            out << ind << std::format("{} = asuint(asint({}) * asint({}));\n", emit_sdst(i.dst), emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::SAndB32:
            out << ind << std::format("{} = {} & {};\n", emit_sdst(i.dst), emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::SOrB32:
            out << ind << std::format("{} = {} | {};\n", emit_sdst(i.dst), emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::SXorB32:
            out << ind << std::format("{} = {} ^ {};\n", emit_sdst(i.dst), emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::SLshlB32:
            out << ind << std::format("{} = {} << ({} & 31u);\n", emit_sdst(i.dst), emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::SLshrB32:
            out << ind << std::format("{} = {} >> ({} & 31u);\n", emit_sdst(i.dst), emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::SAshrI32:
            out << ind << std::format("{} = asuint(asint({}) >> ({} & 31u));\n", emit_sdst(i.dst), emit_u32(i.src0), emit_u32(i.src1));
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
        case IrOp::VFractF32: {
            out << ind << "{\n";
            out << ind << std::format("    float _in = {};\n", emit_f32(i.src0));
            out << ind << "    float _fr = _in - floor(_in);\n";
            out << ind << "    if (_fr >= 1.0f) _fr = asfloat(0x3f7fffff);\n";
            out << ind << std::format("    v[{}] = asuint(_fr);\n", i.dst.reg);
            out << ind << "}\n";
            break;
        }
        case IrOp::VMed3F32: {
            out << ind << "{\n";
            out << ind << std::format("    float _a = {}; float _b = {}; float _c = {};\n", emit_f32(i.src0), emit_f32(i.src1), emit_f32(i.src2));
            out << ind << "    if (isnan(_a) || isnan(_b) || isnan(_c)) {\n";
            out << ind << std::format("        v[{}] = asuint(min(min(_a, _b), _c));\n", i.dst.reg);
            out << ind << "    } else {\n";
            out << ind << std::format("        v[{}] = asuint(max(min(_a, _b), min(max(_a, _b), _c)));\n", i.dst.reg);
            out << ind << "    }\n";
            out << ind << "}\n";
            break;
        }
        case IrOp::VBfeI32: {
            out << ind << "{\n";
            out << ind << std::format("    int _val = asint({}); uint _off = asuint({}) & 31u; uint _w = asuint({}) & 63u;\n", emit_f32(i.src0), emit_f32(i.src1), emit_f32(i.src2));
            out << ind << "    if (_w == 0) {\n";
            out << ind << std::format("        v[{}] = 0;\n", i.dst.reg);
            out << ind << "    } else if (_off + _w >= 32) {\n";
            out << ind << std::format("        v[{}] = asuint(_val >> _off);\n", i.dst.reg);
            out << ind << "    } else {\n";
            out << ind << "        int _sh = 32 - (_off + _w);\n";
            out << ind << std::format("        v[{}] = asuint((_val << _sh) >> (_sh + _off));\n", i.dst.reg);
            out << ind << "    }\n";
            out << ind << "}\n";
            break;
        }
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
        case IrOp::VReadFirstLaneB32:
            out << ind << std::format("s[{}] = WaveReadLaneFirst({});\n", i.dst.reg, emit_u32(i.src0));
            break;
        case IrOp::VReadLaneB32:
            out << ind << std::format("s[{}] = WaveReadLaneAt({}, {});\n", i.dst.reg, emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::VWriteLaneB32:
            out << ind << std::format("if (WaveGetLaneIndex() == ({})) {{ v[{}] = {}; }}\n",
                                      emit_u32(i.src1), i.dst.reg, emit_u32(i.src0));
            break;
        case IrOp::VMbcntLoU32B32:
            out << ind << std::format(
                "v[{}] = countbits(WaveActiveBallot(true).x & ({}) & ((WaveGetLaneIndex() < 32u) ? ((1u << WaveGetLaneIndex()) - 1u) : 0xFFFFFFFFu)) + ({});\n",
                i.dst.reg, emit_u32(i.src0), emit_u32(i.src1));
            break;
        case IrOp::SAndSaveExec:
            if (i.subop == "s_and_saveexec_b64") {
                out << ind << std::format(
                    "{{ uint4 kx_bal = WaveActiveBallot(true); s[{}] = kx_bal.x; s[{}] = kx_bal.y; scc = ((kx_bal.x & ({})) != 0u); }}\n",
                    i.dst.reg, i.dst.reg + 1, emit_u32(i.src0));
            } else {
                out << ind << std::format(
                    "{{ uint kx_bal = WaveActiveBallot(true).x; s[{}] = kx_bal; scc = ((kx_bal & ({})) != 0u); }}\n",
                    i.dst.reg, emit_u32(i.src0));
            }
            break;
        case IrOp::SLoadDword: {
            const uint32_t count = (i.subop == "s_load_dwordx4") ? 4u : ((i.subop == "s_load_dwordx2") ? 2u : 1u);
            if (stage == ShaderStage::Compute) {
                for (uint32_t k = 0; k < count; ++k) {
                    out << ind << std::format("s[{}] = g_mem.Load({} + {}u);\n",
                                              i.dst.reg + k, emit_u32(i.src0), i.offset + k * 4u);
                }
            } else {
                for (uint32_t k = 0; k < count; ++k) {
                    out << ind << std::format("s[{}] = kx_cb_load({} + {}u);\n",
                                              i.dst.reg + k, emit_u32(i.src0), i.offset + k * 4u);
                }
            }
            break;
        }
        case IrOp::GlobalLoadDword:
            out << ind << std::format("v[{}] = g_mem.Load({} + {}u);\n", i.dst.reg, emit_u32(i.src0), i.offset);
            break;
        case IrOp::GlobalStoreDword:
            out << ind << std::format("g_mem.Store({} + {}u, {});\n", emit_u32(i.src0), i.offset, emit_u32(i.src1));
            break;
        case IrOp::BufferLoadDword:
        case IrOp::BufferLoadDwordx2:
        case IrOp::BufferLoadDwordx4: {
            const std::string buf = (stage == ShaderStage::Compute) ? "g_mem" : std::format("g_buf{}", i.rsrc_slot);
            const std::string addr = std::format("({} + {} + {}u)", emit_u32(i.src0), emit_u32(i.src3), i.offset);
            if (i.op == IrOp::BufferLoadDword) {
                out << ind << std::format("v[{}] = {}.Load({});\n", i.dst.reg, buf, addr);
            } else if (i.op == IrOp::BufferLoadDwordx2) {
                out << ind << std::format("{{ uint2 kx_b2 = {}.Load2({}); v[{}] = kx_b2.x; v[{}] = kx_b2.y; }}\n",
                                          buf, addr, i.dst.reg, i.dst.reg + 1);
            } else {
                out << ind << std::format("{{ uint4 kx_b4 = {}.Load4({}); v[{}] = kx_b4.x; v[{}] = kx_b4.y; v[{}] = kx_b4.z; v[{}] = kx_b4.w; }}\n",
                                          buf, addr, i.dst.reg, i.dst.reg + 1, i.dst.reg + 2, i.dst.reg + 3);
            }
            break;
        }
        case IrOp::BufferStoreDword:
        case IrOp::BufferStoreDwordx2:
        case IrOp::BufferStoreDwordx4: {
            const std::string addr = std::format("({} + {} + {}u)", emit_u32(i.src0), emit_u32(i.src3), i.offset);
            if (i.op == IrOp::BufferStoreDword) {
                out << ind << std::format("g_mem.Store({}, v[{}]);\n", addr, i.src1.reg);
            } else if (i.op == IrOp::BufferStoreDwordx2) {
                out << ind << std::format("g_mem.Store2({}, uint2(v[{}], v[{}]));\n",
                                          addr, i.src1.reg, i.src1.reg + 1);
            } else {
                out << ind << std::format("g_mem.Store4({}, uint4(v[{}], v[{}], v[{}], v[{}]));\n",
                                          addr, i.src1.reg, i.src1.reg + 1, i.src1.reg + 2, i.src1.reg + 3);
            }
            break;
        }
        case IrOp::ImageSample: {
            const char* comps[4] = {"x", "y", "z", "w"};
            out << ind << "{ ";
            if (stage == ShaderStage::Pixel) {
                out << std::format("float4 kx_tx = g_tex{}.Sample(g_samp{}, float2(asfloat(v[{}]), asfloat(v[{}]))); ",
                                   i.rsrc_slot, i.samp_slot, i.src0.reg, i.src0.reg + 1);
            } else {
                out << std::format("float4 kx_tx = g_tex{}.SampleLevel(g_samp{}, float2(asfloat(v[{}]), asfloat(v[{}])), 0.0); ",
                                   i.rsrc_slot, i.samp_slot, i.src0.reg, i.src0.reg + 1);
            }
            uint32_t out_idx = i.dst.reg;
            for (int c = 0; c < 4; ++c) {
                if (i.dmask & (1u << c)) {
                    out << std::format("v[{}] = asuint(kx_tx.{}); ", out_idx++, comps[c]);
                }
            }
            out << "}\n";
            break;
        }
        case IrOp::ImageLoad: {
            const char* comps[4] = {"x", "y", "z", "w"};
            out << ind << std::format("{{ float4 kx_tx = g_tex{}.Load(int3(int(v[{}]), int(v[{}]), 0)); ",
                                      i.rsrc_slot, i.src0.reg, i.src0.reg + 1);
            uint32_t out_idx = i.dst.reg;
            for (int c = 0; c < 4; ++c) {
                if (i.dmask & (1u << c)) {
                    out << std::format("v[{}] = asuint(kx_tx.{}); ", out_idx++, comps[c]);
                }
            }
            out << "}\n";
            break;
        }
        case IrOp::ImageGetResInfo: {
            out << ind << std::format("{{ uint kx_tw = 0u, kx_th = 0u, kx_tm = 0u; g_tex{}.GetDimensions(v[{}], kx_tw, kx_th, kx_tm); ",
                                      i.rsrc_slot, i.src0.reg);
            const char* fields[4] = {"kx_tw", "kx_th", "1u", "kx_tm"};
            uint32_t out_idx = i.dst.reg;
            for (int c = 0; c < 4; ++c) {
                if (i.dmask & (1u << c)) {
                    out << std::format("v[{}] = {}; ", out_idx++, fields[c]);
                }
            }
            out << "}\n";
            break;
        }
        case IrOp::DsWriteB32:
            if (stage == ShaderStage::Amplification) {
                out << ind << std::format("kx_payload.data[(({} + {}u) >> 2u) & 63u] = {};\n",
                                          emit_u32(i.src0), i.offset, emit_u32(i.src1));
            } else {
                out << ind << std::format("g_lds[(({} + {}u) >> 2u) & 8191u] = {};\n",
                                          emit_u32(i.src0), i.offset, emit_u32(i.src1));
            }
            break;
        case IrOp::DsReadB32:
            if (stage == ShaderStage::Amplification) {
                out << ind << std::format("v[{}] = kx_payload.data[(({} + {}u) >> 2u) & 63u];\n",
                                          i.dst.reg, emit_u32(i.src0), i.offset);
            } else {
                out << ind << std::format("v[{}] = g_lds[(({} + {}u) >> 2u) & 8191u];\n",
                                          i.dst.reg, emit_u32(i.src0), i.offset);
            }
            break;
        case IrOp::Barrier:
            out << ind << "GroupMemoryBarrierWithGroupSync();\n";
            break;
        case IrOp::SetMeshOutputCounts: {
            if (stage == ShaderStage::Mesh) {
                if (i.src0.kind == IrOperand::Kind::Literal && i.src1.kind == IrOperand::Kind::Literal) {
                    out << ind << std::format("kx_mesh_verts = min({}u, {}u);\n", i.src0.lit_u32, ctx.max_mesh_verts);
                    out << ind << std::format("kx_mesh_prims = min({}u, {}u);\n", i.src1.lit_u32, ctx.max_mesh_prims);
                } else {
                    out << ind << std::format("kx_mesh_verts = min(m0 & 0x7FFu, {}u);\n", ctx.max_mesh_verts);
                    out << ind << std::format("kx_mesh_prims = min((m0 >> 12u) & 0x7FFu, {}u);\n", ctx.max_mesh_prims);
                }
                out << ind << "SetMeshOutputCounts(kx_mesh_verts, kx_mesh_prims);\n";
            } else {
                out << ind << "#error SetMeshOutputCounts is only valid in Mesh shader stage\n";
            }
            break;
        }
        case IrOp::MeshExportVertex: {
            if (stage != ShaderStage::Mesh) {
                out << ind << "#error MeshExportVertex is only valid in Mesh shader stage\n";
                break;
            }
            const IrOperand* srcs[4] = {&i.src0, &i.src1, &i.src2, &i.src3};
            auto chan_f32 = [&](int c) -> std::string {
                if (i.exp_en & (1u << c)) return emit_f32(*srcs[c]);
                return (c == 3) ? "1.0" : "0.0";
            };
            if (i.exp_target == kExpTargetPos0) {
                out << ind << std::format("if (gidx < kx_mesh_verts && gidx < {}u) {{\n", ctx.max_mesh_verts);
                out << ind << std::format("    kx_verts[gidx].pos = float4({}, {}, {}, {});\n",
                                          chan_f32(0), chan_f32(1), chan_f32(2), chan_f32(3));
                if ((ctx.mesh_param_mask & 1u) == 0u) {
                    out << ind << "    kx_verts[gidx].param0 = float4(0.0, 0.0, 0.0, 1.0);\n";
                }
                out << ind << "}\n";
            } else if (i.exp_target >= 13 && i.exp_target <= 15) {
                out << ind << std::format("/* Mesh pos{} export ignored */\n", i.exp_target - 12);
            } else if (i.exp_target >= kExpTargetParam0 && i.exp_target <= 63) {
                const uint32_t p = static_cast<uint32_t>(i.exp_target - kExpTargetParam0);
                out << ind << std::format("if (gidx < kx_mesh_verts && gidx < {}u) {{\n", ctx.max_mesh_verts);
                out << ind << std::format("    kx_verts[gidx].param{} = float4({}, {}, {}, {});\n",
                                          p, chan_f32(0), chan_f32(1), chan_f32(2), chan_f32(3));
                out << ind << "}\n";
            } else {
                out << ind << std::format("#error Unsupported Mesh EXP target: {}\n", i.exp_target);
            }
            break;
        }
        case IrOp::MeshExportPrimitive: {
            if (stage != ShaderStage::Mesh) {
                out << ind << "#error MeshExportPrimitive is only valid in Mesh shader stage\n";
                break;
            }
            out << ind << std::format("if (gidx < kx_mesh_prims && gidx < {}u) {{\n", ctx.max_mesh_prims);
            if ((i.exp_en & 0x7u) == 0x1u) {
                out << ind << std::format("    uint kx_p = {};\n", emit_u32(i.src0));
                out << ind << "    kx_tris[gidx] = ((kx_p & 1u) == 0u) ? uint3((kx_p >> 1u) & 0x1FFu, (kx_p >> 10u) & 0x1FFu, (kx_p >> 19u) & 0x1FFu) : uint3(0u, 0u, 0u);\n";
            } else {
                out << ind << std::format("    kx_tris[gidx] = uint3({}, {}, {});\n",
                                          emit_u32(i.src0), emit_u32(i.src1), emit_u32(i.src2));
            }
            out << ind << "}\n";
            break;
        }
        case IrOp::DispatchMeshPayload: {
            if (stage != ShaderStage::Amplification) {
                out << ind << "#error DispatchMeshPayload is only valid in Amplification shader stage\n";
                break;
            }
            if (ctx.multi_block) {
                out << ind << "kx_dispatch_xyz = uint3(m0 & 0x3FFu, (m0 >> 10u) & 0x3FFu, (m0 >> 20u) & 0x3FFu);\n";
            } else {
                out << ind << "GroupMemoryBarrierWithGroupSync();\n";
                out << ind << "DispatchMesh(m0 & 0x3FFu, (m0 >> 10u) & 0x3FFu, (m0 >> 20u) & 0x3FFu, kx_payload);\n";
            }
            break;
        }
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
                if (is_mrt) {
                    for (int c = 0; c < 4; ++c) {
                        if (i.exp_en & (1u << c)) {
                            out << ind << std::format("kx_mrt.mrt{}.{} = {};\n", i.exp_target, comps[c], emit_f32(*srcs[c]));
                        }
                    }
                } else if (i.exp_target == 0) {
                    for (int c = 0; c < 4; ++c) {
                        if (i.exp_en & (1u << c)) {
                            out << ind << std::format("kx_mrt0.{} = {};\n", comps[c], emit_f32(*srcs[c]));
                        }
                    }
                } else {
                    out << ind << std::format("#error Unsupported Pixel EXP target: {}\n", i.exp_target);
                }
            } else if (stage == ShaderStage::Mesh) {
                auto chan_f32 = [&](int c) -> std::string {
                    if (i.exp_en & (1u << c)) return emit_f32(*srcs[c]);
                    return (c == 3) ? "1.0" : "0.0";
                };
                if (i.exp_target == kExpTargetPos0) {
                    out << ind << std::format("if (gidx < kx_mesh_verts && gidx < {}u) {{\n", ctx.max_mesh_verts);
                    out << ind << std::format("    kx_verts[gidx].pos = float4({}, {}, {}, {});\n",
                                              chan_f32(0), chan_f32(1), chan_f32(2), chan_f32(3));
                    if ((ctx.mesh_param_mask & 1u) == 0u) {
                        out << ind << "    kx_verts[gidx].param0 = float4(0.0, 0.0, 0.0, 1.0);\n";
                    }
                    out << ind << "}\n";
                } else if (i.exp_target >= kExpTargetParam0 && i.exp_target <= 63) {
                    const uint32_t p = static_cast<uint32_t>(i.exp_target - kExpTargetParam0);
                    out << ind << std::format("if (gidx < kx_mesh_verts && gidx < {}u) {{\n", ctx.max_mesh_verts);
                    out << ind << std::format("    kx_verts[gidx].param{} = float4({}, {}, {}, {});\n",
                                              p, chan_f32(0), chan_f32(1), chan_f32(2), chan_f32(3));
                    out << ind << "}\n";
                } else {
                    out << ind << std::format("#error Unsupported Mesh EXP target: {}\n", i.exp_target);
                }
            } else {
                out << ind << "#error EXP instruction is not valid in this shader stage\n";
            }
            break;
        }
        case IrOp::EndPgm:
            if (stage == ShaderStage::Vertex) out << ind << "return kx_out;\n";
            else if (stage == ShaderStage::Pixel) out << ind << (is_mrt ? "return kx_mrt;\n" : "return kx_mrt0;\n");
            else out << ind << "return;\n";
            break;
        default:
            out << ind << std::format("#error Unsupported RDNA2 opcode: {}\n", i.subop);
            break;
    }
}

void emit_cbuf_helper(std::ostringstream& out) {
    out << "cbuffer KxConst : register(b0) { uint4 kx_cb[16]; };\n";
    out << "uint kx_cb_load(uint byte_off) {\n";
    out << "    uint idx = byte_off >> 2u;\n";
    out << "    uint4 row = kx_cb[(idx >> 2u) & 15u];\n";
    out << "    uint c = idx & 3u;\n";
    out << "    return (c == 0u) ? row.x : ((c == 1u) ? row.y : ((c == 2u) ? row.z : row.w));\n";
    out << "}\n";
}

void emit_dynamic_descriptors(std::ostringstream& out, const IrFunction& fn, ShaderStage stage) {
    if (stage != ShaderStage::Compute) {
        for (size_t k = 0; k < fn.buf_srv_bindings.size(); ++k) {
            out << std::format("ByteAddressBuffer g_buf{} : register(t{}); // SGPR s[{}..{}]\n",
                               k, fn.buf_srv_bindings[k].hlsl_reg,
                               fn.buf_srv_bindings[k].sgpr_base, fn.buf_srv_bindings[k].sgpr_base + 3);
        }
    }
    for (size_t k = 0; k < fn.tex_bindings.size(); ++k) {
        out << std::format("Texture2D<float4> g_tex{} : register(t{}); // SGPR s[{}..{}]\n",
                           k, fn.tex_bindings[k].hlsl_reg,
                           fn.tex_bindings[k].sgpr_base, fn.tex_bindings[k].sgpr_base + 7);
    }
    for (size_t k = 0; k < fn.samp_bindings.size(); ++k) {
        out << std::format("SamplerState g_samp{} : register(s{}); // SGPR s[{}..{}]\n",
                           k, fn.samp_bindings[k].hlsl_reg,
                           fn.samp_bindings[k].sgpr_base, fn.samp_bindings[k].sgpr_base + 3);
    }
}

}  // namespace

RootSignatureRemapPlan build_root_remap_plan(const IrFunction& fn, ShaderStage stage) {
    RootSignatureRemapPlan plan{};
    uint32_t root_idx = 0;

    if (stage == ShaderStage::Compute || fn.uses_cbuf || fn.uses_mem) {
        plan.needs_cbv_b0 = true;
        plan.entries.push_back({RootBindingEntry::Kind::CbvB0, 0, 0, root_idx++});
    }
    if (stage == ShaderStage::Compute || fn.uses_mem) {
        plan.needs_uav_u0 = true;
        plan.entries.push_back({RootBindingEntry::Kind::UavU0, 0, 0, root_idx++});
    }
    if (stage != ShaderStage::Compute) {
        for (const auto& b : fn.buf_srv_bindings) {
            plan.entries.push_back({RootBindingEntry::Kind::SrvBuffer, b.sgpr_base, b.hlsl_reg, root_idx++});
            ++plan.total_srv_slots;
        }
    }
    for (const auto& t : fn.tex_bindings) {
        plan.entries.push_back({RootBindingEntry::Kind::SrvTexture, t.sgpr_base, t.hlsl_reg, root_idx++});
        ++plan.total_srv_slots;
    }
    for (const auto& s : fn.samp_bindings) {
        plan.entries.push_back({RootBindingEntry::Kind::Sampler, s.sgpr_base, s.hlsl_reg, root_idx++});
        ++plan.total_sampler_slots;
    }
    return plan;
}

RecompResult emit_hlsl_from_ir(const IrFunction& fn, const RecompOptions& opt) {
    RecompResult res{};
    res.wave_mode = opt.wave_mode;
    res.vgpr_count = std::max<uint32_t>(fn.max_vgpr, 4u);
    const uint32_t min_sgpr = (opt.stage == ShaderStage::Mesh || opt.stage == ShaderStage::Amplification) ? 12u
                            : (opt.stage == ShaderStage::Compute) ? 10u : 4u;
    res.sgpr_count = std::max<uint32_t>(fn.max_sgpr, min_sgpr);
    res.blocks_emitted = static_cast<uint32_t>(fn.blocks.size());
    res.uses_cbuf = fn.uses_cbuf;
    res.uses_wave_ops = fn.uses_wave_ops;
    res.tex_bindings = fn.tex_bindings;
    res.samp_bindings = fn.samp_bindings;
    res.buf_srv_bindings = fn.buf_srv_bindings;
    res.mrt_mask = fn.mrt_mask;
    res.mesh_param_mask = fn.mesh_param_mask;

    const uint32_t max_mesh_verts = std::clamp(
        (opt.max_mesh_verts > 0) ? opt.max_mesh_verts : ((fn.max_mesh_verts > 0) ? fn.max_mesh_verts : 64u),
        1u, 256u);
    const uint32_t max_mesh_prims = std::clamp(
        (opt.max_mesh_prims > 0) ? opt.max_mesh_prims : ((fn.max_mesh_prims > 0) ? fn.max_mesh_prims : 64u),
        1u, 256u);
    if (opt.stage == ShaderStage::Mesh) {
        res.max_mesh_verts = max_mesh_verts;
        res.max_mesh_prims = max_mesh_prims;
    }

    const bool is_mrt = (opt.stage == ShaderStage::Pixel) && ((fn.mrt_mask & ~1u) != 0u);
    const char* ret_stmt = (opt.stage == ShaderStage::Vertex) ? "return kx_out;"
                         : (opt.stage == ShaderStage::Pixel)  ? (is_mrt ? "return kx_mrt;" : "return kx_mrt0;")
                                                              : "return;";
    const bool is_multi_block = fn.blocks.size() > 1;
    const EmitContext ctx{
        .stage = opt.stage,
        .is_mrt = is_mrt,
        .max_mesh_verts = max_mesh_verts,
        .max_mesh_prims = max_mesh_prims,
        .mesh_param_mask = fn.mesh_param_mask,
        .multi_block = is_multi_block,
    };

    const uint32_t mesh_gx = std::clamp(opt.group_size_x, 1u, 128u);
    const uint32_t mesh_gy = std::clamp(opt.group_size_y, 1u, 128u);
    const uint32_t mesh_gz = std::clamp(opt.group_size_z, 1u, 128u);

    std::ostringstream out;
    out << "// Generated by KestrelX RDNA2 (GFX10.3) -> IR -> SM 6.6 HLSL Recompiler\n";
    if (opt.stage == ShaderStage::Compute) {
        out << std::format("// Stage: Compute | Target wave mode: Wave{}\n", static_cast<uint32_t>(opt.wave_mode));
        out << "RWByteAddressBuffer g_mem : register(u0);\n";
        out << "cbuffer KxRoot : register(b0) { uint4 kx_root0; uint4 kx_root1; };\n";
        emit_dynamic_descriptors(out, fn, opt.stage);
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
            if (!fn.uses_cbuf) {
                out << "cbuffer KxRoot : register(b0) { uint4 kx_root0; uint4 kx_root1; };\n";
            }
        }
        if (fn.uses_cbuf) {
            emit_cbuf_helper(out);
        }
        emit_dynamic_descriptors(out, fn, opt.stage);
        out << "struct KxVsOut { float4 pos : SV_Position; float4 param0 : TEXCOORD0; };\n\n";
        out << std::format("KxVsOut {}(uint sv_vid : SV_VertexID) {{\n", opt.entry_point);
        out << "    KxVsOut kx_out = (KxVsOut)0;\n";
    } else if (opt.stage == ShaderStage::Mesh) {
        out << std::format("// Stage: Mesh (ms_6_6) | Target wave mode: Wave{}\n", static_cast<uint32_t>(opt.wave_mode));
        if (fn.uses_mem) {
            out << "RWByteAddressBuffer g_mem : register(u0);\n";
            if (!fn.uses_cbuf) {
                out << "cbuffer KxRoot : register(b0) { uint4 kx_root0; uint4 kx_root1; };\n";
            }
        }
        if (fn.uses_cbuf) {
            emit_cbuf_helper(out);
        }
        emit_dynamic_descriptors(out, fn, opt.stage);
        if (fn.uses_lds) {
            out << "groupshared uint g_lds[8192];\n";
        }
        if (opt.mesh_has_payload) {
            out << "struct KxMeshPayload { uint data[64]; };\n";
        }
        out << "struct KxMeshVert {\n";
        out << "    float4 pos : SV_Position;\n";
        out << "    float4 param0 : TEXCOORD0;\n";
        for (uint32_t p = 1; p < 32; ++p) {
            if (fn.mesh_param_mask & (1u << p)) {
                out << std::format("    float4 param{} : TEXCOORD{};\n", p, p);
            }
        }
        out << "};\n\n";
        out << std::format("[numthreads({}, {}, {})]\n", mesh_gx, mesh_gy, mesh_gz);
        out << "[outputtopology(\"triangle\")]\n";
        out << std::format("void {}(\n", opt.entry_point);
        out << "    uint3 gtid : SV_GroupThreadID,\n";
        out << "    uint3 gid : SV_GroupID,\n";
        out << "    uint gidx : SV_GroupIndex,\n";
        if (opt.mesh_has_payload) {
            out << "    in payload KxMeshPayload kx_payload,\n";
        }
        out << std::format("    out vertices KxMeshVert kx_verts[{}],\n", max_mesh_verts);
        out << std::format("    out indices uint3 kx_tris[{}]) {{\n", max_mesh_prims);
    } else if (opt.stage == ShaderStage::Amplification) {
        out << std::format("// Stage: Amplification (as_6_6) | Target wave mode: Wave{}\n", static_cast<uint32_t>(opt.wave_mode));
        if (fn.uses_mem) {
            out << "RWByteAddressBuffer g_mem : register(u0);\n";
            if (!fn.uses_cbuf) {
                out << "cbuffer KxRoot : register(b0) { uint4 kx_root0; uint4 kx_root1; };\n";
            }
        }
        if (fn.uses_cbuf) {
            emit_cbuf_helper(out);
        }
        emit_dynamic_descriptors(out, fn, opt.stage);
        out << "struct KxMeshPayload { uint data[64]; };\n";
        out << "groupshared KxMeshPayload kx_payload;\n\n";
        out << std::format("[numthreads({}, {}, {})]\n", mesh_gx, mesh_gy, mesh_gz);
        out << std::format("void {}(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID, uint gidx : SV_GroupIndex) {{\n", opt.entry_point);
    } else {
        out << "// Stage: Pixel (ps_6_6)\n";
        if (fn.uses_mem) {
            out << "RWByteAddressBuffer g_mem : register(u0);\n";
            if (!fn.uses_cbuf) {
                out << "cbuffer KxRoot : register(b0) { uint4 kx_root0; uint4 kx_root1; };\n";
            }
        }
        if (fn.uses_cbuf) {
            emit_cbuf_helper(out);
        }
        emit_dynamic_descriptors(out, fn, opt.stage);
        out << "struct KxPsIn { float4 pos : SV_Position; float4 param0 : TEXCOORD0; };\n\n";
        if (is_mrt) {
            out << "struct KxPsOut {\n";
            for (int m = 0; m < 8; ++m) {
                if (fn.mrt_mask & (1u << m)) {
                    out << std::format("    float4 mrt{} : SV_Target{};\n", m, m);
                }
            }
            out << "};\n\n";
            out << std::format("KxPsOut {}(KxPsIn kx_in) {{\n", opt.entry_point);
            out << "    KxPsOut kx_mrt = (KxPsOut)0;\n";
        } else {
            out << std::format("float4 {}(KxPsIn kx_in) : SV_Target0 {{\n", opt.entry_point);
            out << "    float4 kx_mrt0 = float4(0.0, 0.0, 0.0, 1.0);\n";
        }
    }

    out << std::format("    uint v[{}] = (uint[{}])0;\n", res.vgpr_count, res.vgpr_count);
    out << std::format("    uint s[{}] = (uint[{}])0;\n", res.sgpr_count, res.sgpr_count);
    out << "    uint m0 = 0u;\n";
    out << "    bool scc = false;\n";
    out << "    bool vcc = false;\n";
    if (opt.stage == ShaderStage::Compute) {
        out << "    v[0] = tid.x;\n";
        out << "    v[1] = tid.y;\n";
        out << "    v[2] = gtid.x;\n";
        out << "    v[3] = gtid.y;\n";
        out << "    s[0] = kx_root0.x; s[1] = kx_root0.y; s[2] = kx_root0.z; s[3] = kx_root0.w;\n";
        out << "    s[4] = gid.x; s[5] = gid.y; s[6] = gid.z;\n";
        out << "    s[8] = tid.x; s[9] = tid.y;\n\n";
    } else if (opt.stage == ShaderStage::Vertex) {
        out << "    v[0] = sv_vid;\n";
        if (fn.uses_mem && !fn.uses_cbuf) {
            out << "    s[0] = kx_root0.x; s[1] = kx_root0.y; s[2] = kx_root0.z; s[3] = kx_root0.w;\n";
        }
        out << "\n";
    } else if (opt.stage == ShaderStage::Mesh) {
        out << "    uint kx_mesh_verts = 0u;\n";
        out << "    uint kx_mesh_prims = 0u;\n";
        out << "    v[0] = gidx;\n";
        out << "    v[1] = gtid.x;\n";
        out << "    v[2] = gtid.y;\n";
        out << "    v[3] = gtid.z;\n";
        out << "    s[4] = gid.x; s[5] = gid.y; s[6] = gid.z;\n";
        if (opt.mesh_has_payload) {
            out << "    s[8] = kx_payload.data[0]; s[9] = kx_payload.data[1]; s[10] = kx_payload.data[2]; s[11] = kx_payload.data[3];\n";
            if (!fn.uses_mem && !fn.uses_cbuf) {
                out << "    s[0] = kx_payload.data[0]; s[1] = kx_payload.data[1]; s[2] = kx_payload.data[2]; s[3] = kx_payload.data[3];\n";
            }
        } else if (fn.uses_mem && !fn.uses_cbuf) {
            out << "    s[0] = kx_root0.x; s[1] = kx_root0.y; s[2] = kx_root0.z; s[3] = kx_root0.w;\n";
        }
        out << "\n";
    } else if (opt.stage == ShaderStage::Amplification) {
        out << "    v[0] = gidx;\n";
        out << "    v[1] = gtid.x;\n";
        out << "    v[2] = gtid.y;\n";
        out << "    v[3] = gtid.z;\n";
        out << "    s[4] = gid.x; s[5] = gid.y; s[6] = gid.z;\n";
        if (fn.uses_mem && !fn.uses_cbuf) {
            out << "    s[0] = kx_root0.x; s[1] = kx_root0.y; s[2] = kx_root0.z; s[3] = kx_root0.w;\n";
        }
        if (is_multi_block) {
            out << "    uint3 kx_dispatch_xyz = uint3(0u, 0u, 0u);\n";
            out << "    bool kx_done = false;\n";
        }
        out << "\n";
    } else {
        out << "    v[0] = asuint(kx_in.param0.x);\n";
        out << "    v[1] = asuint(kx_in.param0.y);\n";
        out << "    v[2] = asuint(kx_in.param0.z);\n";
        out << "    v[3] = asuint(kx_in.param0.w);\n\n";
    }

    if (!is_multi_block) {
        if (!fn.blocks.empty()) {
            for (const auto& inst : fn.blocks[0].insts) {
                emit_inst(out, inst, "    ", ctx);
            }
        }
        if (opt.stage == ShaderStage::Vertex || opt.stage == ShaderStage::Pixel) {
            out << std::format("    {}\n", ret_stmt);
        }
    } else {
        // For multi-block Mesh shaders, hoist entry-block prologue through SetMeshOutputCounts
        // out of the CFG loop so DXC sees SetMeshOutputCounts in uniform top-level control flow.
        size_t blk0_start = 0;
        if (opt.stage == ShaderStage::Mesh && !fn.blocks.empty()) {
            for (size_t idx = 0; idx < fn.blocks[0].insts.size(); ++idx) {
                if (fn.blocks[0].insts[idx].op == IrOp::SetMeshOutputCounts) {
                    for (size_t k = 0; k <= idx; ++k) {
                        emit_inst(out, fn.blocks[0].insts[k], "    ", ctx);
                    }
                    blk0_start = idx + 1;
                    break;
                }
            }
        }
        out << "    uint bb = 0u;\n";
        if (opt.stage == ShaderStage::Amplification) {
            out << "    [loop] for (uint kx_iter = 0u; kx_iter < 65536u && !kx_done; ++kx_iter) {\n";
        } else {
            out << "    [loop] for (uint kx_iter = 0u; kx_iter < 65536u; ++kx_iter) {\n";
        }
        out << "        switch (bb) {\n";
        for (uint32_t b = 0; b < fn.blocks.size(); ++b) {
            const auto& blk = fn.blocks[b];
            out << std::format("        case {}u: {{\n", b);
            bool terminated = false;
            const size_t start_idx = (b == 0) ? blk0_start : 0;
            for (size_t idx = start_idx; idx < blk.insts.size(); ++idx) {
                const auto& inst = blk.insts[idx];
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
                    if (opt.stage == ShaderStage::Amplification) {
                        out << "            kx_done = true; break;\n";
                    } else {
                        out << std::format("            {}\n", ret_stmt);
                    }
                    terminated = true;
                } else {
                    emit_inst(out, inst, "            ", ctx);
                }
            }
            if (!terminated) {
                if (b + 1 < fn.blocks.size()) out << std::format("            bb = {}u; break;\n", b + 1);
                else if (opt.stage == ShaderStage::Amplification) out << "            kx_done = true; break;\n";
                else out << std::format("            {}\n", ret_stmt);
            }
            out << "        }\n";
        }
        if (opt.stage == ShaderStage::Amplification) {
            out << "        default: kx_done = true; break;\n";
        } else {
            out << std::format("        default: {}\n", ret_stmt);
        }
        out << "        }\n";
        out << "    }\n";
        if (opt.stage == ShaderStage::Amplification) {
            out << "    GroupMemoryBarrierWithGroupSync();\n";
            out << "    DispatchMesh(kx_dispatch_xyz.x, kx_dispatch_xyz.y, kx_dispatch_xyz.z, kx_payload);\n";
        } else if (opt.stage == ShaderStage::Vertex || opt.stage == ShaderStage::Pixel) {
            out << std::format("    {}\n", ret_stmt);
        }
    }

    out << "}\n";
    res.hlsl = out.str();
    return res;
}

}  // namespace kestrel::gpu
