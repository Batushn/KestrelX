#include "kestrel/gpu/rdna2_recomp.hpp"

#include <algorithm>
#include <format>
#include <sstream>

namespace kestrel::gpu {

namespace {

struct EmitContext {
    ShaderStage stage = ShaderStage::Compute;
    bool is_mrt = false;
    uint32_t max_mesh_verts = 64;
    uint32_t max_mesh_prims = 64;
    uint32_t mesh_param_mask = 0;
    bool multi_block = false;
    bool use_exec_mask = false;
    bool use_mask_mode = false;
    bool is_wave64 = false;
    uint32_t sgpr_count = 32;
};

std::string emit_exec_cond(const EmitContext& ctx) {
    if (ctx.is_wave64) {
        return "((((WaveGetLaneIndex() < 32u) ? exec_lo : exec_hi) >> (WaveGetLaneIndex() & 31u)) & 1u) != 0u";
    }
    return "((exec_lo >> WaveGetLaneIndex()) & 1u) != 0u";
}

std::string emit_mask_bit_bool(const EmitContext& ctx, const IrOperand& mask_op) {
    if (mask_op.kind == IrOperand::Kind::Sgpr) {
        if (ctx.is_wave64 && mask_op.reg + 1 < ctx.sgpr_count) {
            return std::format("((((WaveGetLaneIndex() < 32u) ? s[{}] : s[{}]) >> (WaveGetLaneIndex() & 31u)) & 1u) != 0u",
                               mask_op.reg, mask_op.reg + 1);
        }
        return std::format("(((s[{}] >> WaveGetLaneIndex()) & 1u) != 0u)", mask_op.reg);
    }
    if (ctx.is_wave64) {
        return "((((WaveGetLaneIndex() < 32u) ? vcc_lo : vcc_hi) >> (WaveGetLaneIndex() & 31u)) & 1u) != 0u";
    }
    return "(((vcc_lo >> WaveGetLaneIndex()) & 1u) != 0u)";
}

std::string emit_sdst(const IrOperand& op);

void emit_ballot_store(std::ostringstream& out, const char* ind, const EmitContext& ctx,
                       const IrOperand& sdst, const std::string& pred_expr) {
    if (sdst.kind == IrOperand::Kind::Null) {
        return;
    }
    if (!ctx.is_wave64) {
        const std::string target = (sdst.kind == IrOperand::Kind::None) ? "vcc_lo" : emit_sdst(sdst);
        out << ind << std::format("{} = WaveActiveBallot({}).x{};\n",
                                  target, pred_expr, ctx.use_exec_mask ? " & exec_lo" : "");
        return;
    }
    out << ind << std::format("{{ uint4 kx_bal = WaveActiveBallot({}); ", pred_expr);
    if (sdst.kind == IrOperand::Kind::Vcc || sdst.kind == IrOperand::Kind::None) {
        out << (ctx.use_exec_mask ? "vcc_lo = kx_bal.x & exec_lo; vcc_hi = kx_bal.y & exec_hi; }\n"
                                  : "vcc_lo = kx_bal.x; vcc_hi = kx_bal.y; }\n");
    } else if (sdst.kind == IrOperand::Kind::Exec) {
        out << (ctx.use_exec_mask ? "exec_lo = kx_bal.x & exec_lo; exec_hi = kx_bal.y & exec_hi; }\n"
                                  : "exec_lo = kx_bal.x; exec_hi = kx_bal.y; }\n");
    } else if (sdst.kind == IrOperand::Kind::Sgpr) {
        out << std::format("s[{}] = kx_bal.x{}; ", sdst.reg, ctx.use_exec_mask ? " & exec_lo" : "");
        if (sdst.reg + 1 < ctx.sgpr_count) {
            out << std::format("s[{}] = kx_bal.y{}; ", sdst.reg + 1, ctx.use_exec_mask ? " & exec_hi" : "");
        }
        out << "}\n";
    } else {
        out << std::format("{} = kx_bal.x; }}\n", emit_sdst(sdst));
    }
}

std::string emit_u32(const IrOperand& op, bool use_mask_mode = false) {
    switch (op.kind) {
        case IrOperand::Kind::Sgpr: return std::format("s[{}]", op.reg);
        case IrOperand::Kind::Vgpr: return std::format("v[{}]", op.reg);
        case IrOperand::Kind::InlineInt: return std::format("asuint(int({}))", op.imm_i);
        case IrOperand::Kind::InlineFloat: return std::format("asuint(float({:.9g}))", op.imm_f);
        case IrOperand::Kind::Literal: return std::format("{}u", op.lit_u32);
        case IrOperand::Kind::Scc: return "uint(scc ? 1u : 0u)";
        case IrOperand::Kind::Vcc: return use_mask_mode ? "vcc_lo" : "uint(vcc ? 1u : 0u)";
        case IrOperand::Kind::Exec: return use_mask_mode ? "exec_lo" : "1u";
        case IrOperand::Kind::M0: return "m0";
        case IrOperand::Kind::Null: return "0u";
        default: return "0u";
    }
}

std::string emit_sdst(const IrOperand& op) {
    switch (op.kind) {
        case IrOperand::Kind::M0: return "m0";
        case IrOperand::Kind::Exec: return "exec_lo";
        case IrOperand::Kind::Vcc: return "vcc_lo";
        case IrOperand::Kind::Null: return "kx_null";
        default: return std::format("s[{}]", op.reg);
    }
}

std::string emit_f32(const IrOperand& op, bool use_mask_mode = false) {
    std::string base;
    switch (op.kind) {
        case IrOperand::Kind::InlineFloat: base = std::format("float({:.9g})", op.imm_f); break;
        case IrOperand::Kind::InlineInt: base = std::format("asfloat(asuint(int({})))", op.imm_i); break;
        default: base = std::format("asfloat({})", emit_u32(op, use_mask_mode)); break;
    }
    if (op.abs) base = std::format("abs({})", base);
    if (op.neg) base = std::format("(-({}))", base);
    return base;
}

std::string emit_u64_lo(const IrOperand& op, bool use_mask_mode = false) {
    if (op.kind == IrOperand::Kind::Exec) {
        return use_mask_mode ? "exec_lo" : "WaveActiveBallot(true).x";
    }
    return emit_u32(op, use_mask_mode);
}

std::string emit_u64_hi(const IrOperand& op, bool use_mask_mode = false, bool is_wave64 = false) {
    switch (op.kind) {
        case IrOperand::Kind::Sgpr: return std::format("s[{}]", op.reg + 1);
        case IrOperand::Kind::Vgpr: return std::format("v[{}]", op.reg + 1);
        case IrOperand::Kind::InlineInt: return (op.imm_i < 0) ? "0xFFFFFFFFu" : "0u";
        case IrOperand::Kind::Exec: return use_mask_mode ? (is_wave64 ? "exec_hi" : "0u") : "WaveActiveBallot(true).y";
        default: return "0u";
    }
}

void emit_vgpr_write(std::ostringstream& out, const char* ind, const EmitContext& ctx, uint32_t reg, const std::string& val_expr) {
    if (ctx.use_exec_mask) {
        out << ind << std::format("if ({}) {{ v[{}] = {}; }}\n", emit_exec_cond(ctx), reg, val_expr);
    } else {
        out << ind << std::format("v[{}] = {};\n", reg, val_expr);
    }
}

std::string format_saddr_expr(const IrInst& i, const EmitContext& ctx) {
    const std::string base = (i.src2.kind == IrOperand::Kind::Sgpr)
        ? std::format("(s[{}] + {})", i.src2.reg, emit_u32(i.src0, ctx.use_mask_mode))
        : emit_u32(i.src0, ctx.use_mask_mode);
    if (i.soffset == 0) return base;
    if (i.soffset > 0) return std::format("({} + {}u)", base, static_cast<uint32_t>(i.soffset));
    return std::format("asuint(asint({}) + ({}))", base, i.soffset);
}

std::string wrap_clamp(const std::string& expr, bool clamp) {
    return clamp ? std::format("saturate({})", expr) : expr;
}

void emit_inst(std::ostringstream& out, const IrInst& i, const char* ind, const EmitContext& ctx) {
    const ShaderStage stage = ctx.stage;
    const bool is_mrt = ctx.is_mrt;
    const bool mm = ctx.use_mask_mode;
    auto u32 = [&](const IrOperand& op) { return emit_u32(op, mm); };
    auto f32 = [&](const IrOperand& op) { return emit_f32(op, mm); };
    switch (i.op) {
        case IrOp::Nop:
            break;
        case IrOp::SMov:
            out << ind << std::format("{} = {};\n", emit_sdst(i.dst), u32(i.src0));
            break;
        case IrOp::SMovB64:
            out << ind << std::format("{{ uint kx_lo = {}; uint kx_hi = {}; s[{}] = kx_lo; s[{}] = kx_hi; }}\n",
                                      emit_u64_lo(i.src0, mm), emit_u64_hi(i.src0, mm, ctx.is_wave64), i.dst.reg, i.dst.reg + 1);
            break;
        case IrOp::SNotB32:
            out << ind << std::format("{{ uint kx_r = ~({}); {} = kx_r; scc = (kx_r != 0u); }}\n",
                                      u32(i.src0), emit_sdst(i.dst));
            break;
        case IrOp::SNotB64:
            out << ind << std::format("{{ uint kx_lo = ~({}); uint kx_hi = ~({}); s[{}] = kx_lo; s[{}] = kx_hi; scc = ((kx_lo | kx_hi) != 0u); }}\n",
                                      emit_u64_lo(i.src0, mm), emit_u64_hi(i.src0, mm, ctx.is_wave64), i.dst.reg, i.dst.reg + 1);
            break;
        case IrOp::SCSelectB32:
            out << ind << std::format("{} = scc ? ({}) : ({});\n", emit_sdst(i.dst), u32(i.src0), u32(i.src1));
            break;
        case IrOp::SAddU32:
            if (i.subop == "s_add_u32") {
                out << ind << std::format("{{ uint kx_a = {}; uint kx_b = {}; uint kx_r = kx_a + kx_b; {} = kx_r; scc = (kx_r < kx_a); }}\n",
                                          u32(i.src0), u32(i.src1), emit_sdst(i.dst));
            } else if (i.subop == "s_add_i32") {
                out << ind << std::format("{{ uint kx_a = {}; uint kx_b = {}; uint kx_r = kx_a + kx_b; {} = kx_r; scc = ((((kx_r ^ kx_a) & (kx_r ^ kx_b)) & 0x80000000u) != 0u); }}\n",
                                          u32(i.src0), u32(i.src1), emit_sdst(i.dst));
            } else {
                out << ind << std::format("{} = {} + {};\n", emit_sdst(i.dst), u32(i.src0), u32(i.src1));
            }
            break;
        case IrOp::SSubU32:
            if (i.subop == "s_sub_u32") {
                out << ind << std::format("{{ uint kx_a = {}; uint kx_b = {}; {} = kx_a - kx_b; scc = (kx_a < kx_b); }}\n",
                                          u32(i.src0), u32(i.src1), emit_sdst(i.dst));
            } else if (i.subop == "s_sub_i32") {
                out << ind << std::format("{{ uint kx_a = {}; uint kx_b = {}; uint kx_r = kx_a - kx_b; {} = kx_r; scc = ((((kx_a ^ kx_b) & (kx_a ^ kx_r)) & 0x80000000u) != 0u); }}\n",
                                          u32(i.src0), u32(i.src1), emit_sdst(i.dst));
            } else {
                out << ind << std::format("{} = {} - {};\n", emit_sdst(i.dst), u32(i.src0), u32(i.src1));
            }
            break;
        case IrOp::SAddCU32:
            out << ind << std::format("{{ uint kx_a = {}; uint kx_b = {}; uint kx_c = scc ? 1u : 0u; uint kx_ab = kx_a + kx_b; uint kx_r = kx_ab + kx_c; {} = kx_r; scc = (kx_ab < kx_a) || (kx_r < kx_ab); }}\n",
                                      u32(i.src0), u32(i.src1), emit_sdst(i.dst));
            break;
        case IrOp::SSubBU32:
            out << ind << std::format("{{ uint kx_a = {}; uint kx_b = {}; uint kx_c = scc ? 1u : 0u; uint kx_ab = kx_a - kx_b; uint kx_r = kx_ab - kx_c; {} = kx_r; scc = (kx_a < kx_b) || (kx_ab < kx_c); }}\n",
                                      u32(i.src0), u32(i.src1), emit_sdst(i.dst));
            break;
        case IrOp::SMulI32:
            out << ind << std::format("{} = asuint(asint({}) * asint({}));\n", emit_sdst(i.dst), u32(i.src0), u32(i.src1));
            break;
        case IrOp::SAndB32:
            out << ind << std::format("{{ uint kx_r = ({}) & ({}); {} = kx_r; scc = (kx_r != 0u); }}\n", u32(i.src0), u32(i.src1), emit_sdst(i.dst));
            break;
        case IrOp::SAndN2B32:
            out << ind << std::format("{{ uint kx_r = ({}) & (~({})); {} = kx_r; scc = (kx_r != 0u); }}\n", u32(i.src0), u32(i.src1), emit_sdst(i.dst));
            break;
        case IrOp::SOrB32:
            out << ind << std::format("{{ uint kx_r = ({}) | ({}); {} = kx_r; scc = (kx_r != 0u); }}\n", u32(i.src0), u32(i.src1), emit_sdst(i.dst));
            break;
        case IrOp::SXorB32:
            out << ind << std::format("{{ uint kx_r = ({}) ^ ({}); {} = kx_r; scc = (kx_r != 0u); }}\n", u32(i.src0), u32(i.src1), emit_sdst(i.dst));
            break;
        case IrOp::SLshlB32:
            out << ind << std::format("{{ uint kx_r = {} << ({} & 31u); {} = kx_r; scc = (kx_r != 0u); }}\n", u32(i.src0), u32(i.src1), emit_sdst(i.dst));
            break;
        case IrOp::SLshrB32:
            out << ind << std::format("{{ uint kx_r = {} >> ({} & 31u); {} = kx_r; scc = (kx_r != 0u); }}\n", u32(i.src0), u32(i.src1), emit_sdst(i.dst));
            break;
        case IrOp::SAshrI32:
            out << ind << std::format("{{ uint kx_r = asuint(asint({}) >> ({} & 31u)); {} = kx_r; scc = (kx_r != 0u); }}\n", u32(i.src0), u32(i.src1), emit_sdst(i.dst));
            break;
        case IrOp::SCmp: {
            const std::string a = u32(i.src0), b = u32(i.src1);
            if (i.subop == "s_cmp_eq_i32" || i.subop == "s_cmp_eq_u32") out << ind << std::format("scc = ({} == {});\n", a, b);
            else if (i.subop == "s_cmp_lg_i32" || i.subop == "s_cmp_lg_u32") out << ind << std::format("scc = ({} != {});\n", a, b);
            else if (i.subop == "s_cmp_lt_i32") out << ind << std::format("scc = (asint({}) < asint({}));\n", a, b);
            else if (i.subop == "s_cmp_le_i32") out << ind << std::format("scc = (asint({}) <= asint({}));\n", a, b);
            else if (i.subop == "s_cmp_gt_i32") out << ind << std::format("scc = (asint({}) > asint({}));\n", a, b);
            else if (i.subop == "s_cmp_ge_i32") out << ind << std::format("scc = (asint({}) >= asint({}));\n", a, b);
            else if (i.subop == "s_cmp_lt_u32") out << ind << std::format("scc = ({} < {});\n", a, b);
            else if (i.subop == "s_cmp_le_u32") out << ind << std::format("scc = ({} <= {});\n", a, b);
            else if (i.subop == "s_cmp_gt_u32") out << ind << std::format("scc = ({} > {});\n", a, b);
            else if (i.subop == "s_cmp_ge_u32") out << ind << std::format("scc = ({} >= {});\n", a, b);
            else out << ind << std::format("#error Unsupported RDNA2 SCmp predicate: {}\n", i.subop);
            break;
        }
        case IrOp::VMov:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, u32(i.src0));
            break;
        case IrOp::VAddF32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint({})", wrap_clamp(std::format("({} + {})", f32(i.src0), f32(i.src1)), i.clamp)));
            break;
        case IrOp::VSubF32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint({})", wrap_clamp(std::format("({} - {})", f32(i.src0), f32(i.src1)), i.clamp)));
            break;
        case IrOp::VSubRevF32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint({})", wrap_clamp(std::format("({} - {})", f32(i.src1), f32(i.src0)), i.clamp)));
            break;
        case IrOp::VMulF32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint({})", wrap_clamp(std::format("({} * {})", f32(i.src0), f32(i.src1)), i.clamp)));
            break;
        case IrOp::VFmaF32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint({})", wrap_clamp(std::format("mad({}, {}, {})", f32(i.src0), f32(i.src1), f32(i.src2)), i.clamp)));
            break;
        case IrOp::VMinF32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint(min({}, {}))", f32(i.src0), f32(i.src1)));
            break;
        case IrOp::VMaxF32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint(max({}, {}))", f32(i.src0), f32(i.src1)));
            break;
        case IrOp::VMinI32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint(min(asint({}), asint({})))", u32(i.src0), u32(i.src1)));
            break;
        case IrOp::VMaxI32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint(max(asint({}), asint({})))", u32(i.src0), u32(i.src1)));
            break;
        case IrOp::VMinU32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("min({}, {})", u32(i.src0), u32(i.src1)));
            break;
        case IrOp::VMaxU32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("max({}, {})", u32(i.src0), u32(i.src1)));
            break;
        case IrOp::VAddU32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("{} + {}", u32(i.src0), u32(i.src1)));
            break;
        case IrOp::VSubU32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("{} - {}", u32(i.src0), u32(i.src1)));
            break;
        case IrOp::VSubRevU32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("{} - {}", u32(i.src1), u32(i.src0)));
            break;
        case IrOp::VAddCoU32:
        case IrOp::VSubCoU32:
        case IrOp::VSubRevCoU32: {
            out << ind << "{\n";
            if (i.op == IrOp::VAddCoU32) {
                out << ind << std::format("    uint kx_a = {}; uint kx_b = {}; uint kx_r = kx_a + kx_b; bool kx_c = (kx_r < kx_a);\n",
                                          u32(i.src0), u32(i.src1));
            } else if (i.op == IrOp::VSubCoU32) {
                out << ind << std::format("    uint kx_a = {}; uint kx_b = {}; uint kx_r = kx_a - kx_b; bool kx_c = (kx_a < kx_b);\n",
                                          u32(i.src0), u32(i.src1));
            } else {
                out << ind << std::format("    uint kx_a = {}; uint kx_b = {}; uint kx_r = kx_b - kx_a; bool kx_c = (kx_b < kx_a);\n",
                                          u32(i.src0), u32(i.src1));
            }
            emit_vgpr_write(out, (std::string(ind) + "    ").c_str(), ctx, i.dst.reg, "kx_r");
            emit_ballot_store(out, (std::string(ind) + "    ").c_str(), ctx, i.sdst, "kx_c");
            out << ind << "}\n";
            break;
        }
        case IrOp::VAddCoCiU32:
        case IrOp::VSubCoCiU32:
        case IrOp::VSubRevCoCiU32: {
            out << ind << "{\n";
            out << ind << std::format("    uint kx_ci = ({}) ? 1u : 0u;\n", emit_mask_bit_bool(ctx, i.src2));
            if (i.op == IrOp::VAddCoCiU32) {
                out << ind << std::format("    uint kx_a = {}; uint kx_b = {}; uint kx_ab = kx_a + kx_b; uint kx_r = kx_ab + kx_ci; bool kx_c = (kx_ab < kx_a) || (kx_r < kx_ab);\n",
                                          u32(i.src0), u32(i.src1));
            } else if (i.op == IrOp::VSubCoCiU32) {
                out << ind << std::format("    uint kx_a = {}; uint kx_b = {}; uint kx_ab = kx_a - kx_b; uint kx_r = kx_ab - kx_ci; bool kx_c = (kx_a < kx_b) || (kx_ab < kx_ci);\n",
                                          u32(i.src0), u32(i.src1));
            } else {
                out << ind << std::format("    uint kx_a = {}; uint kx_b = {}; uint kx_ab = kx_b - kx_a; uint kx_r = kx_ab - kx_ci; bool kx_c = (kx_b < kx_a) || (kx_ab < kx_ci);\n",
                                          u32(i.src0), u32(i.src1));
            }
            emit_vgpr_write(out, (std::string(ind) + "    ").c_str(), ctx, i.dst.reg, "kx_r");
            emit_ballot_store(out, (std::string(ind) + "    ").c_str(), ctx, i.sdst, "kx_c");
            out << ind << "}\n";
            break;
        }
        case IrOp::VAdd3U32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("({}) + ({}) + ({})", u32(i.src0), u32(i.src1), u32(i.src2)));
            break;
        case IrOp::VLshlAddU32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("(({}) << (({}) & 31u)) + ({})", u32(i.src0), u32(i.src1), u32(i.src2)));
            break;
        case IrOp::VLshlOrB32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("(({}) << (({}) & 31u)) | ({})", u32(i.src0), u32(i.src1), u32(i.src2)));
            break;
        case IrOp::VXor3B32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("({}) ^ ({}) ^ ({})", u32(i.src0), u32(i.src1), u32(i.src2)));
            break;
        case IrOp::VPermB32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("kx_v_perm_b32({}, {}, {})", u32(i.src0), u32(i.src1), u32(i.src2)));
            break;
        case IrOp::VMadU64U32: {
            out << ind << "{\n";
            out << ind << std::format("    uint kx_a = {}; uint kx_b = {}; uint kx_c0 = {}; uint kx_c1 = {};\n",
                                      u32(i.src0), u32(i.src1), emit_u64_lo(i.src2, mm), emit_u64_hi(i.src2, mm, ctx.is_wave64));
            out << ind << "    uint kx_p0 = kx_a * kx_b; uint kx_p1 = kx_umul_hi(kx_a, kx_b);\n";
            out << ind << "    uint kx_lo = kx_p0 + kx_c0; uint kx_cry = (kx_lo < kx_p0) ? 1u : 0u;\n";
            out << ind << "    uint kx_h1 = kx_p1 + kx_c1; uint kx_hi = kx_h1 + kx_cry;\n";
            if (ctx.use_exec_mask) {
                out << ind << std::format("    if ({}) {{ v[{}] = kx_lo; v[{}] = kx_hi; }}\n",
                                          emit_exec_cond(ctx), i.dst.reg, i.dst.reg + 1);
            } else {
                out << ind << std::format("    v[{}] = kx_lo; v[{}] = kx_hi;\n", i.dst.reg, i.dst.reg + 1);
            }
            emit_ballot_store(out, (std::string(ind) + "    ").c_str(), ctx, i.sdst, "(kx_h1 < kx_p1) || (kx_hi < kx_h1)");
            out << ind << "}\n";
            break;
        }
        case IrOp::VMulLoU32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("{} * {}", u32(i.src0), u32(i.src1)));
            break;
        case IrOp::VMadU32U24:
            emit_vgpr_write(out, ind, ctx, i.dst.reg,
                            std::format("((({}) & 0xFFFFFFu) * (({}) & 0xFFFFFFu)) + ({})", u32(i.src0), u32(i.src1), u32(i.src2)));
            break;
        case IrOp::VBfeU32:
            out << ind << "{\n";
            out << ind << std::format("    uint kx_s0 = {}; uint kx_off = ({}) & 31u; uint kx_w = ({}) & 31u;\n",
                                      u32(i.src0), u32(i.src1), u32(i.src2));
            emit_vgpr_write(out, (std::string(ind) + "    ").c_str(), ctx, i.dst.reg,
                            "(kx_w == 0u) ? 0u : ((kx_s0 >> kx_off) & ((1u << kx_w) - 1u))");
            out << ind << "}\n";
            break;
        case IrOp::VBfiB32:
            out << ind << "{\n";
            out << ind << std::format("    uint kx_m = {}; uint kx_r = (kx_m & ({})) | ((~kx_m) & ({}));\n",
                                      u32(i.src0), u32(i.src1), u32(i.src2));
            emit_vgpr_write(out, (std::string(ind) + "    ").c_str(), ctx, i.dst.reg, "kx_r");
            out << ind << "}\n";
            break;
        case IrOp::VAndB32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("{} & {}", u32(i.src0), u32(i.src1)));
            break;
        case IrOp::VOrB32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("{} | {}", u32(i.src0), u32(i.src1)));
            break;
        case IrOp::VXorB32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("{} ^ {}", u32(i.src0), u32(i.src1)));
            break;
        case IrOp::VLshlRevB32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("{} << ({} & 31u)", u32(i.src1), u32(i.src0)));
            break;
        case IrOp::VLshrRevB32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("{} >> ({} & 31u)", u32(i.src1), u32(i.src0)));
            break;
        case IrOp::VAshrRevI32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint(asint({}) >> ({} & 31u))", u32(i.src1), u32(i.src0)));
            break;
        case IrOp::VLshlRevB64: {
            out << ind << "{\n";
            out << ind << std::format("    uint kx_sh = ({}) & 63u; uint kx_lo = {}; uint kx_hi = {};\n",
                                      u32(i.src0), emit_u64_lo(i.src1, mm), emit_u64_hi(i.src1, mm));
            out << ind << "    uint kx_rlo = (kx_sh == 0u) ? kx_lo : ((kx_sh < 32u) ? (kx_lo << kx_sh) : 0u);\n";
            out << ind << "    uint kx_rhi = (kx_sh == 0u) ? kx_hi : ((kx_sh < 32u) ? ((kx_hi << kx_sh) | (kx_lo >> (32u - kx_sh))) : (kx_lo << (kx_sh - 32u)));\n";
            if (ctx.use_exec_mask) {
                out << ind << std::format("    if (((exec_lo >> WaveGetLaneIndex()) & 1u) != 0u) {{ v[{}] = kx_rlo; v[{}] = kx_rhi; }}\n",
                                          i.dst.reg, i.dst.reg + 1);
            } else {
                out << ind << std::format("    v[{}] = kx_rlo; v[{}] = kx_rhi;\n", i.dst.reg, i.dst.reg + 1);
            }
            out << ind << "}\n";
            break;
        }
        case IrOp::VLshrRevB64: {
            out << ind << "{\n";
            out << ind << std::format("    uint kx_sh = ({}) & 63u; uint kx_lo = {}; uint kx_hi = {};\n",
                                      u32(i.src0), emit_u64_lo(i.src1, mm), emit_u64_hi(i.src1, mm));
            out << ind << "    uint kx_rlo = (kx_sh == 0u) ? kx_lo : ((kx_sh < 32u) ? ((kx_lo >> kx_sh) | (kx_hi << (32u - kx_sh))) : (kx_hi >> (kx_sh - 32u)));\n";
            out << ind << "    uint kx_rhi = (kx_sh == 0u) ? kx_hi : ((kx_sh < 32u) ? (kx_hi >> kx_sh) : 0u);\n";
            if (ctx.use_exec_mask) {
                out << ind << std::format("    if (((exec_lo >> WaveGetLaneIndex()) & 1u) != 0u) {{ v[{}] = kx_rlo; v[{}] = kx_rhi; }}\n",
                                          i.dst.reg, i.dst.reg + 1);
            } else {
                out << ind << std::format("    v[{}] = kx_rlo; v[{}] = kx_rhi;\n", i.dst.reg, i.dst.reg + 1);
            }
            out << ind << "}\n";
            break;
        }
        case IrOp::VAshrRevI64: {
            out << ind << "{\n";
            out << ind << std::format("    uint kx_sh = ({}) & 63u; uint kx_lo = {}; uint kx_hi = {};\n",
                                      u32(i.src0), emit_u64_lo(i.src1, mm), emit_u64_hi(i.src1, mm));
            out << ind << "    uint kx_sign = (asint(kx_hi) < 0) ? 0xFFFFFFFFu : 0u;\n";
            out << ind << "    uint kx_rlo = (kx_sh == 0u) ? kx_lo : ((kx_sh < 32u) ? ((kx_lo >> kx_sh) | (kx_hi << (32u - kx_sh))) : asuint(asint(kx_hi) >> (kx_sh - 32u)));\n";
            out << ind << "    uint kx_rhi = (kx_sh == 0u) ? kx_hi : ((kx_sh < 32u) ? asuint(asint(kx_hi) >> kx_sh) : kx_sign);\n";
            if (ctx.use_exec_mask) {
                out << ind << std::format("    if (((exec_lo >> WaveGetLaneIndex()) & 1u) != 0u) {{ v[{}] = kx_rlo; v[{}] = kx_rhi; }}\n",
                                          i.dst.reg, i.dst.reg + 1);
            } else {
                out << ind << std::format("    v[{}] = kx_rlo; v[{}] = kx_rhi;\n", i.dst.reg, i.dst.reg + 1);
            }
            out << ind << "}\n";
            break;
        }
        case IrOp::VCvtF32I32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint(float(asint({})))", u32(i.src0)));
            break;
        case IrOp::VCvtF32U32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint(float({}))", u32(i.src0)));
            break;
        case IrOp::VCvtI32F32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint(int({}))", f32(i.src0)));
            break;
        case IrOp::VCvtU32F32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("uint({})", f32(i.src0)));
            break;
        case IrOp::VRcpF32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint({})", wrap_clamp(std::format("rcp({})", f32(i.src0)), i.clamp)));
            break;
        case IrOp::VRsqF32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint({})", wrap_clamp(std::format("rsqrt({})", f32(i.src0)), i.clamp)));
            break;
        case IrOp::VSqrtF32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint({})", wrap_clamp(std::format("sqrt({})", f32(i.src0)), i.clamp)));
            break;
        case IrOp::VFloorF32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint(floor({}))", f32(i.src0)));
            break;
        case IrOp::VCeilF32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint(ceil({}))", f32(i.src0)));
            break;
        case IrOp::VTruncF32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint(trunc({}))", f32(i.src0)));
            break;
        case IrOp::VRndneF32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint(round({}))", f32(i.src0)));
            break;
        case IrOp::VFractF32: {
            out << ind << "{\n";
            out << ind << std::format("    float _in = {};\n", f32(i.src0));
            out << ind << "    float _fr = _in - floor(_in);\n";
            out << ind << "    if (_fr >= 1.0f) _fr = asfloat(0x3f7fffff);\n";
            emit_vgpr_write(out, (std::string(ind) + "    ").c_str(), ctx, i.dst.reg, "asuint(_fr)");
            out << ind << "}\n";
            break;
        }
        case IrOp::VFrexpExpI32F32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("kx_frexp_exp_i32_f32(asuint({}))", f32(i.src0)));
            break;
        case IrOp::VFrexpMantF32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("kx_frexp_mant_f32(asuint({}))", f32(i.src0)));
            break;
        case IrOp::VLdexpF32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg,
                            std::format("asuint({})", wrap_clamp(std::format("asfloat(kx_ldexp_f32(asuint({}), asint({})))", f32(i.src0), u32(i.src1)), i.clamp)));
            break;
        case IrOp::VSinF32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint({})", wrap_clamp(std::format("sin(({}) * 6.283185307179586f)", f32(i.src0)), i.clamp)));
            break;
        case IrOp::VCosF32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("asuint({})", wrap_clamp(std::format("cos(({}) * 6.283185307179586f)", f32(i.src0)), i.clamp)));
            break;
        case IrOp::VMed3F32: {
            out << ind << "{\n";
            out << ind << std::format("    float _a = {}; float _b = {}; float _c = {};\n", f32(i.src0), f32(i.src1), f32(i.src2));
            out << ind << "    if (isnan(_a) || isnan(_b) || isnan(_c)) {\n";
            emit_vgpr_write(out, (std::string(ind) + "        ").c_str(), ctx, i.dst.reg,
                            std::format("asuint({})", wrap_clamp("min(min(_a, _b), _c)", i.clamp)));
            out << ind << "    } else {\n";
            emit_vgpr_write(out, (std::string(ind) + "        ").c_str(), ctx, i.dst.reg,
                            std::format("asuint({})", wrap_clamp("max(min(_a, _b), min(max(_a, _b), _c))", i.clamp)));
            out << ind << "    }\n";
            out << ind << "}\n";
            break;
        }
        case IrOp::VBfeI32: {
            out << ind << "{\n";
            out << ind << std::format("    int _val = asint({}); uint _off = asuint({}) & 31u; uint _w = asuint({}) & 63u;\n",
                                      f32(i.src0), f32(i.src1), f32(i.src2));
            out << ind << "    uint _res = 0u;\n";
            out << ind << "    if (_w == 0) { _res = 0u; }\n";
            out << ind << "    else if (_off + _w >= 32) { _res = asuint(_val >> _off); }\n";
            out << ind << "    else { int _sh = 32 - (_off + _w); _res = asuint((_val << _sh) >> (_sh + _off)); }\n";
            emit_vgpr_write(out, (std::string(ind) + "    ").c_str(), ctx, i.dst.reg, "_res");
            out << ind << "}\n";
            break;
        }
        case IrOp::VCmp: {
            auto cmp_suffix = [](const std::string& m) -> std::string {
                if (m.rfind("v_cmpx_", 0) == 0) return "v_cmp_" + m.substr(7);
                return m;
            };
            const std::string op = cmp_suffix(i.subop);
            std::string pred;
            if (op == "v_cmp_f_f32" || op == "v_cmp_f_i32" || op == "v_cmp_f_u32") pred = "false";
            else if (op == "v_cmp_t_f32" || op == "v_cmp_t_i32" || op == "v_cmp_t_u32") pred = "true";
            else if (op == "v_cmp_lt_f32") pred = std::format("({} < {})", f32(i.src0), f32(i.src1));
            else if (op == "v_cmp_eq_f32") pred = std::format("({} == {})", f32(i.src0), f32(i.src1));
            else if (op == "v_cmp_le_f32") pred = std::format("({} <= {})", f32(i.src0), f32(i.src1));
            else if (op == "v_cmp_gt_f32") pred = std::format("({} > {})", f32(i.src0), f32(i.src1));
            else if (op == "v_cmp_lg_f32" || op == "v_cmp_neq_f32") pred = std::format("({} != {})", f32(i.src0), f32(i.src1));
            else if (op == "v_cmp_ge_f32") pred = std::format("({} >= {})", f32(i.src0), f32(i.src1));
            else if (op == "v_cmp_o_f32") pred = std::format("(!isnan({}) && !isnan({}))", f32(i.src0), f32(i.src1));
            else if (op == "v_cmp_u_f32") pred = std::format("(isnan({}) || isnan({}))", f32(i.src0), f32(i.src1));
            else if (op == "v_cmp_nge_f32") pred = std::format("(!({} >= {}))", f32(i.src0), f32(i.src1));
            else if (op == "v_cmp_nlg_f32") pred = std::format("(!({} != {}))", f32(i.src0), f32(i.src1));
            else if (op == "v_cmp_ngt_f32") pred = std::format("(!({} > {}))", f32(i.src0), f32(i.src1));
            else if (op == "v_cmp_nle_f32") pred = std::format("(!({} <= {}))", f32(i.src0), f32(i.src1));
            else if (op == "v_cmp_nlt_f32") pred = std::format("(!({} < {}))", f32(i.src0), f32(i.src1));
            else if (op == "v_cmp_lt_i32") pred = std::format("(asint({}) < asint({}))", u32(i.src0), u32(i.src1));
            else if (op == "v_cmp_eq_i32") pred = std::format("(asint({}) == asint({}))", u32(i.src0), u32(i.src1));
            else if (op == "v_cmp_le_i32") pred = std::format("(asint({}) <= asint({}))", u32(i.src0), u32(i.src1));
            else if (op == "v_cmp_gt_i32") pred = std::format("(asint({}) > asint({}))", u32(i.src0), u32(i.src1));
            else if (op == "v_cmp_ne_i32") pred = std::format("(asint({}) != asint({}))", u32(i.src0), u32(i.src1));
            else if (op == "v_cmp_ge_i32") pred = std::format("(asint({}) >= asint({}))", u32(i.src0), u32(i.src1));
            else if (op == "v_cmp_lt_u32") pred = std::format("({} < {})", u32(i.src0), u32(i.src1));
            else if (op == "v_cmp_eq_u32") pred = std::format("({} == {})", u32(i.src0), u32(i.src1));
            else if (op == "v_cmp_le_u32") pred = std::format("({} <= {})", u32(i.src0), u32(i.src1));
            else if (op == "v_cmp_gt_u32") pred = std::format("({} > {})", u32(i.src0), u32(i.src1));
            else if (op == "v_cmp_ne_u32") pred = std::format("({} != {})", u32(i.src0), u32(i.src1));
            else if (op == "v_cmp_ge_u32") pred = std::format("({} >= {})", u32(i.src0), u32(i.src1));

            if (pred.empty()) {
                out << ind << std::format("#error Unsupported RDNA2 VCmp predicate: {}\n", i.subop);
            } else if (!ctx.use_mask_mode && (i.sdst.kind == IrOperand::Kind::Vcc || i.sdst.kind == IrOperand::Kind::None)) {
                out << ind << std::format("vcc = {};\n", pred);
            } else {
                emit_ballot_store(out, ind, ctx, i.sdst, pred);
            }
            break;
        }
        case IrOp::VCmpClassF32: {
            const std::string pred = std::format("kx_cmp_class_f32(asuint({}), {})", f32(i.src0), u32(i.src1));
            if (!ctx.use_mask_mode && (i.sdst.kind == IrOperand::Kind::Vcc || i.sdst.kind == IrOperand::Kind::None)) {
                out << ind << std::format("vcc = {};\n", pred);
            } else {
                emit_ballot_store(out, ind, ctx, i.sdst, pred);
            }
            break;
        }
        case IrOp::VCndMask: {
            const std::string cond =
                (!ctx.use_mask_mode && (i.src2.kind == IrOperand::Kind::Vcc || i.src2.kind == IrOperand::Kind::None))
                    ? "vcc"
                    : emit_mask_bit_bool(ctx, i.src2);
            emit_vgpr_write(out, ind, ctx, i.dst.reg, std::format("({}) ? {} : {}", cond, u32(i.src1), u32(i.src0)));
            break;
        }
        case IrOp::VReadFirstLaneB32:
            out << ind << std::format("{} = WaveReadLaneFirst({});\n", emit_sdst(i.dst), u32(i.src0));
            break;
        case IrOp::VReadLaneB32:
            out << ind << std::format("{} = WaveReadLaneAt({}, {});\n", emit_sdst(i.dst), u32(i.src0), u32(i.src1));
            break;
        case IrOp::VWriteLaneB32:
            out << ind << std::format("if (WaveGetLaneIndex() == ({})) {{ v[{}] = {}; }}\n",
                                      u32(i.src1), i.dst.reg, u32(i.src0));
            break;
        case IrOp::VMbcntLoU32B32:
            emit_vgpr_write(out, ind, ctx, i.dst.reg,
                            std::format("countbits(WaveActiveBallot(true).x & ({}) & ((WaveGetLaneIndex() < 32u) ? ((1u << WaveGetLaneIndex()) - 1u) : 0xFFFFFFFFu)) + ({})",
                                        u32(i.src0), u32(i.src1)));
            break;
        case IrOp::SAndSaveExec:
            if (i.subop == "s_and_saveexec_b64") {
                if (ctx.use_exec_mask && ctx.is_wave64) {
                    out << ind << std::format(
                        "{{ uint kx_old0 = exec_lo; uint kx_old1 = exec_hi; exec_lo = kx_old0 & ({}); exec_hi = kx_old1 & ({}); s[{}] = kx_old0; s[{}] = kx_old1; scc = ((exec_lo | exec_hi) != 0u); }}\n",
                        u32(i.src0), emit_u64_hi(i.src0, mm, true), i.dst.reg, i.dst.reg + 1);
                } else {
                    out << ind << std::format(
                        "{{ uint4 kx_bal = WaveActiveBallot(true); s[{}] = kx_bal.x; s[{}] = kx_bal.y; scc = ((kx_bal.x & ({})) != 0u); }}\n",
                        i.dst.reg, i.dst.reg + 1, u32(i.src0));
                }
            } else if (ctx.use_exec_mask) {
                out << ind << std::format(
                    "{{ uint kx_old = exec_lo; exec_lo = kx_old & ({}); {} = kx_old; scc = (exec_lo != 0u); }}\n",
                    u32(i.src0), emit_sdst(i.dst));
            } else {
                out << ind << std::format(
                    "{{ uint kx_bal = WaveActiveBallot(true).x; {} = kx_bal; scc = ((kx_bal & ({})) != 0u); }}\n",
                    emit_sdst(i.dst), u32(i.src0));
            }
            break;
        case IrOp::SAndN2SaveExec:
            out << ind << std::format(
                "{{ uint kx_old = exec_lo; exec_lo = ({}) & (~kx_old); {} = kx_old; scc = (exec_lo != 0u); }}\n",
                u32(i.src0), emit_sdst(i.dst));
            break;
        case IrOp::SLoadDword: {
            uint32_t count = 1u;
            if (i.subop == "s_load_dwordx16") count = 16u;
            else if (i.subop == "s_load_dwordx8") count = 8u;
            else if (i.subop == "s_load_dwordx4") count = 4u;
            else if (i.subop == "s_load_dwordx2") count = 2u;
            const std::string soff_reg = (i.src1.kind == IrOperand::Kind::Sgpr) ? std::format(" + s[{}]", i.src1.reg) : "";
            const std::string base_addr = (i.soffset == 0)
                ? std::format("({}{})", u32(i.src0), soff_reg)
                : ((i.soffset > 0)
                    ? std::format("({}{} + {}u)", u32(i.src0), soff_reg, static_cast<uint32_t>(i.soffset))
                    : std::format("asuint(asint({}{}) + ({}))", u32(i.src0), soff_reg, i.soffset));
            out << ind << std::format("{{ uint kx_sa = {};\n", base_addr);
            if (stage == ShaderStage::Compute) {
                for (uint32_t k = 0; k < count; ++k) {
                    out << ind << std::format("  s[{}] = g_mem.Load(kx_sa + {}u);\n", i.dst.reg + k, k * 4u);
                }
            } else {
                for (uint32_t k = 0; k < count; ++k) {
                    out << ind << std::format("  s[{}] = kx_cb_load(kx_sa + {}u);\n", i.dst.reg + k, k * 4u);
                }
            }
            out << ind << "}\n";
            break;
        }
        case IrOp::GlobalLoadDword: {
            const std::string addr = format_saddr_expr(i, ctx);
            if (ctx.use_exec_mask) {
                out << ind << std::format("if ({}) {{ v[{}] = g_mem.Load({}); }}\n",
                                          emit_exec_cond(ctx), i.dst.reg, addr);
            } else {
                out << ind << std::format("v[{}] = g_mem.Load({});\n", i.dst.reg, addr);
            }
            break;
        }
        case IrOp::GlobalLoadDwordx2: {
            const std::string addr = format_saddr_expr(i, ctx);
            if (ctx.use_exec_mask) {
                out << ind << std::format("if ({}) {{ uint2 kx_g2 = g_mem.Load2({}); v[{}] = kx_g2.x; v[{}] = kx_g2.y; }}\n",
                                          emit_exec_cond(ctx), addr, i.dst.reg, i.dst.reg + 1);
            } else {
                out << ind << std::format("{{ uint2 kx_g2 = g_mem.Load2({}); v[{}] = kx_g2.x; v[{}] = kx_g2.y; }}\n",
                                          addr, i.dst.reg, i.dst.reg + 1);
            }
            break;
        }
        case IrOp::GlobalLoadDwordx4: {
            const std::string addr = format_saddr_expr(i, ctx);
            if (ctx.use_exec_mask) {
                out << ind << std::format("if ({}) {{ uint4 kx_g4 = g_mem.Load4({}); v[{}] = kx_g4.x; v[{}] = kx_g4.y; v[{}] = kx_g4.z; v[{}] = kx_g4.w; }}\n",
                                          emit_exec_cond(ctx), addr, i.dst.reg, i.dst.reg + 1, i.dst.reg + 2, i.dst.reg + 3);
            } else {
                out << ind << std::format("{{ uint4 kx_g4 = g_mem.Load4({}); v[{}] = kx_g4.x; v[{}] = kx_g4.y; v[{}] = kx_g4.z; v[{}] = kx_g4.w; }}\n",
                                          addr, i.dst.reg, i.dst.reg + 1, i.dst.reg + 2, i.dst.reg + 3);
            }
            break;
        }
        case IrOp::GlobalStoreDword: {
            const std::string addr = format_saddr_expr(i, ctx);
            if (ctx.use_exec_mask) {
                out << ind << std::format("if ({}) {{ g_mem.Store({}, {}); }}\n",
                                          emit_exec_cond(ctx), addr, u32(i.src1));
            } else {
                out << ind << std::format("g_mem.Store({}, {});\n", addr, u32(i.src1));
            }
            break;
        }
        case IrOp::GlobalStoreDwordx2: {
            const std::string addr = format_saddr_expr(i, ctx);
            if (ctx.use_exec_mask) {
                out << ind << std::format("if ({}) {{ g_mem.Store2({}, uint2(v[{}], v[{}])); }}\n",
                                          emit_exec_cond(ctx), addr, i.src1.reg, i.src1.reg + 1);
            } else {
                out << ind << std::format("g_mem.Store2({}, uint2(v[{}], v[{}]));\n",
                                          addr, i.src1.reg, i.src1.reg + 1);
            }
            break;
        }
        case IrOp::GlobalStoreDwordx4: {
            const std::string addr = format_saddr_expr(i, ctx);
            if (ctx.use_exec_mask) {
                out << ind << std::format("if ({}) {{ g_mem.Store4({}, uint4(v[{}], v[{}], v[{}], v[{}])); }}\n",
                                          emit_exec_cond(ctx), addr, i.src1.reg, i.src1.reg + 1, i.src1.reg + 2, i.src1.reg + 3);
            } else {
                out << ind << std::format("g_mem.Store4({}, uint4(v[{}], v[{}], v[{}], v[{}]));\n",
                                          addr, i.src1.reg, i.src1.reg + 1, i.src1.reg + 2, i.src1.reg + 3);
            }
            break;
        }
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
            if (i.tfe) {
                out << "uint kx_tfe_st = 0u; ";
                if (stage == ShaderStage::Pixel) {
                    out << std::format("float4 kx_tx = g_tex{}.Sample(g_samp{}, float2(asfloat(v[{}]), asfloat(v[{}])), int2(0, 0), 0.0f, kx_tfe_st); ",
                                       i.rsrc_slot, i.samp_slot, i.src0.reg, i.src0.reg + 1);
                } else {
                    out << std::format("float4 kx_tx = g_tex{}.SampleLevel(g_samp{}, float2(asfloat(v[{}]), asfloat(v[{}])), 0.0, int2(0, 0), kx_tfe_st); ",
                                       i.rsrc_slot, i.samp_slot, i.src0.reg, i.src0.reg + 1);
                }
            } else if (stage == ShaderStage::Pixel) {
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
            if (i.tfe) {
                out << std::format("v[{}] = CheckAccessFullyMapped(kx_tfe_st) ? 0u : 1u; ", out_idx);
            }
            out << "}\n";
            break;
        }
        case IrOp::ImageLoad: {
            const char* comps[4] = {"x", "y", "z", "w"};
            out << ind << "{ ";
            if (i.tfe) {
                out << std::format("uint kx_tfe_st = 0u; float4 kx_tx = g_tex{}.Load(int3(int(v[{}]), int(v[{}]), 0), int2(0, 0), kx_tfe_st); ",
                                   i.rsrc_slot, i.src0.reg, i.src0.reg + 1);
            } else {
                out << std::format("float4 kx_tx = g_tex{}.Load(int3(int(v[{}]), int(v[{}]), 0)); ",
                                   i.rsrc_slot, i.src0.reg, i.src0.reg + 1);
            }
            uint32_t out_idx = i.dst.reg;
            for (int c = 0; c < 4; ++c) {
                if (i.dmask & (1u << c)) {
                    out << std::format("v[{}] = asuint(kx_tx.{}); ", out_idx++, comps[c]);
                }
            }
            if (i.tfe) {
                out << std::format("v[{}] = CheckAccessFullyMapped(kx_tfe_st) ? 0u : 1u; ", out_idx);
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
        case IrOp::ImageBvhIntersectRay: {
            out << ind << "{\n";
            out << ind << "    RayDesc kx_ray;\n";
            out << ind << std::format("    kx_ray.Origin = float3(asfloat(v[{}]), asfloat(v[{}]), asfloat(v[{}]));\n",
                                      i.src0.reg, i.src0.reg + 1, i.src0.reg + 2);
            out << ind << std::format("    kx_ray.TMin = asfloat(v[{}]);\n", i.src0.reg + 3);
            out << ind << std::format("    kx_ray.Direction = float3(asfloat(v[{}]), asfloat(v[{}]), asfloat(v[{}]));\n",
                                      i.src0.reg + 4, i.src0.reg + 5, i.src0.reg + 6);
            out << ind << std::format("    kx_ray.TMax = asfloat(v[{}]);\n", i.src0.reg + 7);
            out << ind << "    RayQuery<RAY_FLAG_NONE> kx_q;\n";
            out << ind << std::format("    kx_q.TraceRayInline(g_as{}, RAY_FLAG_NONE, 0xFF, kx_ray);\n", i.rsrc_slot);
            out << ind << "    kx_q.Proceed();\n";
            out << ind << std::format("    v[{}] = (kx_q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 1u : 0u;\n", i.dst.reg);
            out << ind << std::format("    v[{}] = asuint(kx_q.CommittedRayT());\n", i.dst.reg + 1);
            out << ind << std::format("    v[{}] = asuint(kx_q.CommittedTriangleBarycentrics().x);\n", i.dst.reg + 2);
            out << ind << std::format("    v[{}] = asuint(kx_q.CommittedTriangleBarycentrics().y);\n", i.dst.reg + 3);
            out << ind << "}\n";
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
            if (i.subop.rfind("vopc_", 0) == 0) {
                out << ind << std::format("#error Unsupported RDNA2 VCmp predicate: {}\n", i.subop);
            } else {
                out << ind << std::format("#error Unsupported RDNA2 opcode: {}\n", i.subop);
            }
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
    for (size_t k = 0; k < fn.as_bindings.size(); ++k) {
        out << std::format("RaytracingAccelerationStructure g_as{} : register(t{}); // SGPR s[{}..{}]\n",
                           k, fn.as_bindings[k].hlsl_reg,
                           fn.as_bindings[k].sgpr_base, fn.as_bindings[k].sgpr_base + 3);
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
    for (const auto& as : fn.as_bindings) {
        plan.entries.push_back({RootBindingEntry::Kind::SrvTexture, as.sgpr_base, as.hlsl_reg, root_idx++});
        ++plan.total_srv_slots;
    }
    for (const auto& s : fn.samp_bindings) {
        plan.entries.push_back({RootBindingEntry::Kind::Sampler, s.sgpr_base, s.hlsl_reg, root_idx++});
        ++plan.total_sampler_slots;
    }
    return plan;
}

RecompResult emit_hlsl_from_ir(const IrFunction& fn_in, const RecompOptions& opt) {
    IrFunction opt_fn;
    const IrFunction& fn = opt.enable_ir_opt ? (opt_fn = fn_in, optimize_ir(opt_fn), opt_fn) : fn_in;
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
    const bool use_exec_mask = fn.uses_exec_mask;
    const bool use_mask_mode = fn.uses_exec_mask || fn.reads_vcc_mask;
    bool needs_umul_hi = false;
    bool needs_perm = false;
    bool needs_frexp = false;
    bool needs_ldexp = false;
    bool needs_class = false;
    bool needs_null = false;
    for (const auto& blk : fn.blocks) {
        for (const auto& inst : blk.insts) {
            if (inst.op == IrOp::VMadU64U32) needs_umul_hi = true;
            else if (inst.op == IrOp::VPermB32) needs_perm = true;
            else if (inst.op == IrOp::VFrexpExpI32F32 || inst.op == IrOp::VFrexpMantF32) needs_frexp = true;
            else if (inst.op == IrOp::VLdexpF32) needs_ldexp = true;
            else if (inst.op == IrOp::VCmpClassF32) needs_class = true;
            if (inst.dst.kind == IrOperand::Kind::Null || inst.sdst.kind == IrOperand::Kind::Null) needs_null = true;
        }
    }
    const EmitContext ctx{
        .stage = opt.stage,
        .is_mrt = is_mrt,
        .max_mesh_verts = max_mesh_verts,
        .max_mesh_prims = max_mesh_prims,
        .mesh_param_mask = fn.mesh_param_mask,
        .multi_block = is_multi_block,
        .use_exec_mask = use_exec_mask,
        .use_mask_mode = use_mask_mode,
        .is_wave64 = (opt.wave_mode == WaveMode::Wave64),
        .sgpr_count = res.sgpr_count,
    };

    const uint32_t mesh_gx = std::clamp(opt.group_size_x, 1u, 128u);
    const uint32_t mesh_gy = std::clamp(opt.group_size_y, 1u, 128u);
    const uint32_t mesh_gz = std::clamp(opt.group_size_z, 1u, 128u);

    std::ostringstream out;
    out << "// Generated by KestrelX RDNA2 (GFX10.3) -> IR -> SM 6.6 HLSL Recompiler\n";
    auto emit_math_helpers = [&]() {
        if (needs_umul_hi) {
            out << "uint kx_umul_hi(uint a, uint b) {\n"
                << "    uint a0 = a & 0xFFFFu; uint a1 = a >> 16u;\n"
                << "    uint b0 = b & 0xFFFFu; uint b1 = b >> 16u;\n"
                << "    uint p00 = a0 * b0; uint p01 = a0 * b1; uint p10 = a1 * b0; uint p11 = a1 * b1;\n"
                << "    uint mid = (p00 >> 16u) + (p01 & 0xFFFFu) + (p10 & 0xFFFFu);\n"
                << "    return p11 + (p01 >> 16u) + (p10 >> 16u) + (mid >> 16u);\n"
                << "}\n";
        }
        if (needs_perm) {
            out << "uint kx_v_perm_b32(uint s0, uint s1, uint sel) {\n"
                << "    uint res = 0u;\n"
                << "    [unroll] for (uint b = 0u; b < 4u; ++b) {\n"
                << "        uint k = (sel >> (b * 8u)) & 0xFFu;\n"
                << "        uint bv = 0u;\n"
                << "        if (k <= 3u) bv = (s1 >> (k * 8u)) & 0xFFu;\n"
                << "        else if (k <= 7u) bv = (s0 >> ((k - 4u) * 8u)) & 0xFFu;\n"
                << "        else if (k == 8u) bv = ((s1 & 0x80000000u) != 0u) ? 0xFFu : 0u;\n"
                << "        else if (k <= 11u) bv = ((s0 & 0x80000000u) != 0u) ? 0xFFu : 0u;\n"
                << "        else if (k == 12u) bv = 0x00u;\n"
                << "        else bv = 0xFFu;\n"
                << "        res |= (bv << (b * 8u));\n"
                << "    }\n"
                << "    return res;\n"
                << "}\n";
        }
        if (needs_frexp) {
            out << "uint kx_frexp_mant_f32(uint bits) {\n"
                << "    uint exp = (bits >> 23u) & 0xFFu;\n"
                << "    if (exp == 0xFFu || (bits & 0x7FFFFFFFu) == 0u) return bits;\n"
                << "    if (exp == 0u) { bits = asuint(asfloat(bits) * 16777216.0f); }\n"
                << "    return (bits & 0x807FFFFFu) | 0x3F000000u;\n"
                << "}\n"
                << "uint kx_frexp_exp_i32_f32(uint bits) {\n"
                << "    uint exp = (bits >> 23u) & 0xFFu;\n"
                << "    if (exp == 0xFFu || (bits & 0x7FFFFFFFu) == 0u) return 0u;\n"
                << "    if (exp == 0u) {\n"
                << "        uint nb = asuint(asfloat(bits) * 16777216.0f);\n"
                << "        return asuint(int((nb >> 23u) & 0xFFu) - 150);\n"
                << "    }\n"
                << "    return asuint(int(exp) - 126);\n"
                << "}\n";
        }
        if (needs_ldexp) {
            out << "uint kx_ldexp_f32(uint bits, int e) {\n"
                << "    uint exp = (bits >> 23u) & 0xFFu;\n"
                << "    if (exp == 0xFFu || (bits & 0x7FFFFFFFu) == 0u) return bits;\n"
                << "    if (exp == 0u) {\n"
                << "        bits = asuint(asfloat(bits) * 16777216.0f);\n"
                << "        exp = (bits >> 23u) & 0xFFu; e -= 24;\n"
                << "    }\n"
                << "    int ne = int(exp) + e;\n"
                << "    if (ne >= 255) return (bits & 0x80000000u) | 0x7F800000u;\n"
                << "    if (ne <= 0) {\n"
                << "        if (ne < -23) return bits & 0x80000000u;\n"
                << "        uint mant = (bits & 0x7FFFFFu) | 0x800000u;\n"
                << "        return (bits & 0x80000000u) | (mant >> uint(1 - ne));\n"
                << "    }\n"
                << "    return (bits & 0x807FFFFFu) | (uint(ne) << 23u);\n"
                << "}\n";
        }
        if (needs_class) {
            out << "bool kx_cmp_class_f32(uint bits, uint mask) {\n"
                << "    uint exp = (bits >> 23u) & 0xFFu;\n"
                << "    uint mant = bits & 0x7FFFFFu;\n"
                << "    bool neg = (bits >> 31u) != 0u;\n"
                << "    uint cbit = 0u;\n"
                << "    if (exp == 0xFFu) { cbit = (mant != 0u) ? (((mant & 0x400000u) != 0u) ? 2u : 1u) : (neg ? 4u : 0x200u); }\n"
                << "    else if (exp == 0u) { cbit = (mant == 0u) ? (neg ? 0x20u : 0x40u) : (neg ? 0x10u : 0x80u); }\n"
                << "    else { cbit = neg ? 8u : 0x100u; }\n"
                << "    return (cbit & mask) != 0u;\n"
                << "}\n";
        }
    };
    emit_math_helpers();
    if (opt.stage == ShaderStage::Compute) {
        out << std::format("// Stage: Compute | Target wave mode: Wave{}\n", static_cast<uint32_t>(opt.wave_mode));
        out << "RWByteAddressBuffer g_mem : register(u0);\n";
        out << "cbuffer KxRoot : register(b0) { uint4 kx_root0; uint4 kx_root1; };\n";
        emit_dynamic_descriptors(out, fn, opt.stage);
        if (fn.uses_lds) {
            out << "groupshared uint g_lds[8192];\n";
        }
        out << "\n";
        if (opt.emit_wave_size_attr) {
            out << std::format("[WaveSize({})]\n", static_cast<uint32_t>(opt.wave_mode));
        }
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
    if (use_mask_mode) {
        out << "    uint vcc_lo = 0u;\n";
        if (ctx.is_wave64) out << "    uint vcc_hi = 0u;\n";
    } else {
        out << "    bool vcc = false;\n";
    }
    if (use_exec_mask) {
        if (ctx.is_wave64) {
            out << "    uint4 kx_init_bal = WaveActiveBallot(true);\n";
            out << "    uint exec_lo = kx_init_bal.x;\n";
            out << "    uint exec_hi = kx_init_bal.y;\n";
        } else {
            out << "    uint exec_lo = WaveActiveBallot(true).x;\n";
        }
    }
    if (needs_null) {
        out << "    uint kx_null = 0u;\n";
    }
    if (opt.stage == ShaderStage::Compute) {
        if (opt.use_amdhsa_abi) {
            out << "    v[0] = gtid.x;\n";
            if (res.vgpr_count > 1) out << "    v[1] = gtid.y;\n";
            if (res.vgpr_count > 2) out << "    v[2] = gtid.z;\n";
            if (opt.amdhsa_kernarg_sgpr + 1u < res.sgpr_count) {
                out << std::format("    s[{}] = kx_root0.x; s[{}] = 0u;\n",
                                   opt.amdhsa_kernarg_sgpr, opt.amdhsa_kernarg_sgpr + 1u);
            }
            if (opt.amdhsa_wg_id_x_sgpr < res.sgpr_count) {
                out << std::format("    s[{}] = gid.x;\n", opt.amdhsa_wg_id_x_sgpr);
            }
            if (opt.amdhsa_wg_id_y_sgpr != opt.amdhsa_wg_id_x_sgpr && opt.amdhsa_wg_id_y_sgpr < res.sgpr_count) {
                out << std::format("    s[{}] = gid.y;\n", opt.amdhsa_wg_id_y_sgpr);
            }
            if (opt.amdhsa_wg_id_z_sgpr != opt.amdhsa_wg_id_x_sgpr &&
                opt.amdhsa_wg_id_z_sgpr != opt.amdhsa_wg_id_y_sgpr &&
                opt.amdhsa_wg_id_z_sgpr < res.sgpr_count) {
                out << std::format("    s[{}] = gid.z;\n", opt.amdhsa_wg_id_z_sgpr);
            }
            out << "\n";
        } else {
            out << "    v[0] = tid.x;\n";
            out << "    v[1] = tid.y;\n";
            out << "    v[2] = gtid.x;\n";
            out << "    v[3] = gtid.y;\n";
            out << "    s[0] = kx_root0.x; s[1] = kx_root0.y; s[2] = kx_root0.z; s[3] = kx_root0.w;\n";
            out << "    s[4] = gid.x; s[5] = gid.y; s[6] = gid.z;\n";
            out << "    s[8] = tid.x; s[9] = tid.y;\n\n";
        }
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
                    const char* cond_expr = (inst.subop.find("exec") != std::string::npos)
                                                ? (use_exec_mask
                                                    ? (ctx.is_wave64 ? "((exec_lo != 0u) || (exec_hi != 0u))" : "(exec_lo != 0u)")
                                                    : "any(WaveActiveBallot(true).xy != uint2(0u, 0u))")
                                                : ((inst.subop.find("vcc") != std::string::npos)
                                                    ? (use_mask_mode
                                                        ? (ctx.is_wave64 ? "((vcc_lo != 0u) || (vcc_hi != 0u))" : "(vcc_lo != 0u)")
                                                        : "vcc")
                                                    : "scc");
                    out << std::format("            bb = ({}{}) ? {}u : {}u; break;\n",
                                       inst.branch_on_true ? "" : "!", cond_expr, inst.target_block, fallthrough);
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
