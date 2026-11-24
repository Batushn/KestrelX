#include "kestrel/gpu/ir.hpp"

#include <algorithm>
#include <format>
#include <set>
#include <sstream>
#include <unordered_map>

namespace kestrel::gpu {

namespace {

IrOperand decode_operand(uint32_t code, bool has_lit, uint32_t lit, bool abs = false, bool neg = false) {
    IrOperand op{};
    op.abs = abs;
    op.neg = neg;
    if (code <= 105) {
        op.kind = IrOperand::Kind::Sgpr;
        op.reg = code;
    } else if (code == 106 || code == 107) {
        op.kind = IrOperand::Kind::Vcc;
    } else if (code == 124) {
        op.kind = IrOperand::Kind::M0;
    } else if (code == 126 || code == 127) {
        op.kind = IrOperand::Kind::Exec;
    } else if (code >= 128 && code <= 192) {
        op.kind = IrOperand::Kind::InlineInt;
        op.imm_i = static_cast<int32_t>(code) - 128;
    } else if (code >= 193 && code <= 208) {
        op.kind = IrOperand::Kind::InlineInt;
        op.imm_i = -(static_cast<int32_t>(code) - 192);
    } else if (code >= 240 && code <= 248) {
        static constexpr float kConsts[9] = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 4.0f, -4.0f, 0.15915494f};
        op.kind = IrOperand::Kind::InlineFloat;
        op.imm_f = kConsts[code - 240];
    } else if (code == 253) {
        op.kind = IrOperand::Kind::Scc;
    } else if (code == 255) {
        op.kind = IrOperand::Kind::Literal;
        op.lit_u32 = has_lit ? lit : 0u;
    } else if (code >= 256 && code <= 511) {
        op.kind = IrOperand::Kind::Vgpr;
        op.reg = code - 256;
    }
    return op;
}

IrOperand make_sgpr(uint32_t r) {
    IrOperand op{};
    op.kind = IrOperand::Kind::Sgpr;
    op.reg = r;
    return op;
}

IrOperand make_sdst(uint32_t r) {
    if (r == 124) {
        IrOperand op{};
        op.kind = IrOperand::Kind::M0;
        return op;
    }
    return make_sgpr(r);
}

IrOperand make_vgpr(uint32_t r) {
    IrOperand op{};
    op.kind = IrOperand::Kind::Vgpr;
    op.reg = r;
    return op;
}

void track_regs(const IrOperand& op, uint32_t& max_v, uint32_t& max_s) {
    if (op.kind == IrOperand::Kind::Vgpr && op.reg + 1 > max_v) max_v = op.reg + 1;
    if (op.kind == IrOperand::Kind::Sgpr && op.reg + 1 > max_s) max_s = op.reg + 1;
}

}  // namespace

IrFunction lift_rdna2_to_ir(const std::vector<DecodedInst>& decoded) {
    IrFunction fn{};
    fn.inst_count = static_cast<uint32_t>(decoded.size());
    if (decoded.empty()) return fn;

    // 1. Discover basic-block leaders (entry, branch targets, fallthrough after branch/terminator)
    std::set<uint32_t> leaders;
    leaders.insert(decoded.front().pc);
    std::unordered_map<uint32_t, size_t> pc_to_idx;
    for (size_t i = 0; i < decoded.size(); ++i) {
        pc_to_idx[decoded[i].pc] = i;
    }
    for (size_t i = 0; i < decoded.size(); ++i) {
        const auto& d = decoded[i];
        if (d.is_branch) {
            if (pc_to_idx.count(d.branch_target)) leaders.insert(d.branch_target);
            if (i + 1 < decoded.size()) leaders.insert(decoded[i + 1].pc);
        } else if (d.is_terminator && i + 1 < decoded.size()) {
            leaders.insert(decoded[i + 1].pc);
        }
    }

    std::unordered_map<uint32_t, uint32_t> pc_to_block;
    uint32_t next_id = 0;
    for (uint32_t lpc : leaders) {
        pc_to_block[lpc] = next_id;
        IrBlock blk{};
        blk.id = next_id++;
        blk.start_pc = lpc;
        fn.blocks.push_back(std::move(blk));
    }

    // 2. Lift instructions into their owning basic blocks
    uint32_t next_extra_t = 2;
    auto alloc_tex_slot = [&](uint32_t srsrc) -> uint32_t {
        for (uint32_t k = 0; k < fn.tex_bindings.size(); ++k) {
            if (fn.tex_bindings[k].sgpr_base == srsrc) return k;
        }
        const uint32_t reg = fn.tex_bindings.empty() ? 0u : next_extra_t++;
        fn.tex_bindings.push_back({srsrc, reg});
        return static_cast<uint32_t>(fn.tex_bindings.size() - 1);
    };
    auto alloc_buf_srv_slot = [&](uint32_t srsrc) -> uint32_t {
        for (uint32_t k = 0; k < fn.buf_srv_bindings.size(); ++k) {
            if (fn.buf_srv_bindings[k].sgpr_base == srsrc) return k;
        }
        const uint32_t reg = fn.buf_srv_bindings.empty() ? 1u : next_extra_t++;
        fn.buf_srv_bindings.push_back({srsrc, reg});
        return static_cast<uint32_t>(fn.buf_srv_bindings.size() - 1);
    };
    auto alloc_samp_slot = [&](uint32_t ssamp) -> uint32_t {
        for (uint32_t k = 0; k < fn.samp_bindings.size(); ++k) {
            if (fn.samp_bindings[k].sgpr_base == ssamp) return k;
        }
        const uint32_t reg = static_cast<uint32_t>(fn.samp_bindings.size());
        fn.samp_bindings.push_back({ssamp, reg});
        return reg;
    };
    auto alloc_as_slot = [&](uint32_t srsrc) -> uint32_t {
        for (uint32_t k = 0; k < fn.as_bindings.size(); ++k) {
            if (fn.as_bindings[k].sgpr_base == srsrc) return k;
        }
        const uint32_t reg = next_extra_t++;
        fn.as_bindings.push_back({srsrc, reg});
        return static_cast<uint32_t>(fn.as_bindings.size() - 1);
    };

    // Pre-scan for mesh shader markers (s_sendmsg GS_ALLOC_REQ or EXP prim0)
    bool is_mesh_shader = false;
    for (const auto& d : decoded) {
        if (d.enc == Encoding::SOPP && d.mnemonic == "s_sendmsg" &&
            ((static_cast<uint16_t>(d.simm16) & 0xFu) == kSendMsgGsAllocReq)) {
            is_mesh_shader = true;
            break;
        }
        if (d.enc == Encoding::EXP && d.opcode == kExpTargetPrim0) {
            is_mesh_shader = true;
            break;
        }
    }

    uint32_t cur_blk = 0;
    bool has_const_m0 = false;
    uint32_t const_m0 = 0;
    for (size_t i = 0; i < decoded.size(); ++i) {
        const auto& d = decoded[i];
        auto it = pc_to_block.find(d.pc);
        if (it != pc_to_block.end()) {
            cur_blk = it->second;
            has_const_m0 = false;
        }

        IrInst ir{};
        ir.pc = d.pc;
        ir.clamp = d.clamp;
        ir.offset = d.offset;

        switch (d.enc) {
            case Encoding::SOPP:
                if (d.mnemonic == "s_barrier") {
                    ir.op = IrOp::Barrier;
                } else if (d.mnemonic == "s_sendmsg") {
                    const uint16_t raw_msg = static_cast<uint16_t>(d.simm16);
                    const uint16_t msg_id = raw_msg & 0xFu;
                    ir.offset = raw_msg;
                    if (msg_id == kSendMsgGsAllocReq) {
                        ir.op = IrOp::SetMeshOutputCounts;
                        ir.subop = "gs_alloc_req";
                        if ((raw_msg >> 4) != 0) {
                            const uint32_t vcnt = (raw_msg >> 4) & 0x3Fu;
                            const uint32_t pcnt = (raw_msg >> 10) & 0x3Fu;
                            ir.src0.kind = IrOperand::Kind::Literal;
                            ir.src0.lit_u32 = vcnt;
                            ir.src1.kind = IrOperand::Kind::Literal;
                            ir.src1.lit_u32 = pcnt;
                            if (vcnt > fn.max_mesh_verts) fn.max_mesh_verts = vcnt;
                            if (pcnt > fn.max_mesh_prims) fn.max_mesh_prims = pcnt;
                        } else {
                            ir.src0.kind = IrOperand::Kind::M0;
                            if (has_const_m0) {
                                const uint32_t vcnt = const_m0 & 0x7FFu;
                                const uint32_t pcnt = (const_m0 >> 12u) & 0x7FFu;
                                if (vcnt > fn.max_mesh_verts) fn.max_mesh_verts = vcnt;
                                if (pcnt > fn.max_mesh_prims) fn.max_mesh_prims = pcnt;
                            }
                        }
                    } else if (msg_id == kSendMsgDispatchMesh) {
                        ir.op = IrOp::DispatchMeshPayload;
                        ir.subop = "dispatch_mesh";
                        ir.src0.kind = IrOperand::Kind::M0;
                    } else {
                        ir.op = IrOp::Nop;
                        ir.subop = d.mnemonic;
                    }
                } else if (d.mnemonic == "s_nop" || d.mnemonic == "s_waitcnt" || d.mnemonic == "s_waitcnt_vscnt" ||
                           d.mnemonic == "s_clause") {
                    ir.op = IrOp::Nop;
                } else if (d.mnemonic == "s_endpgm" || d.mnemonic == "s_endpgm_saved") {
                    ir.op = IrOp::EndPgm;
                } else if (d.mnemonic == "s_branch") {
                    ir.op = IrOp::Branch;
                    ir.target_block = pc_to_block.count(d.branch_target) ? pc_to_block[d.branch_target] : 0;
                } else if (d.mnemonic == "s_cbranch_scc0" || d.mnemonic == "s_cbranch_scc1" ||
                           d.mnemonic == "s_cbranch_vccz" || d.mnemonic == "s_cbranch_vccnz" ||
                           d.mnemonic == "s_cbranch_execz" || d.mnemonic == "s_cbranch_execnz") {
                    ir.op = IrOp::CondBranch;
                    ir.subop = d.mnemonic;
                    ir.branch_on_true = (d.mnemonic == "s_cbranch_scc1" || d.mnemonic == "s_cbranch_vccnz" ||
                                         d.mnemonic == "s_cbranch_execnz");
                    ir.target_block = pc_to_block.count(d.branch_target) ? pc_to_block[d.branch_target] : 0;
                    if (d.mnemonic == "s_cbranch_execz" || d.mnemonic == "s_cbranch_execnz") {
                        fn.uses_wave_ops = true;
                    }
                } else {
                    ir.op = IrOp::Nop;
                    ir.subop = d.mnemonic;
                }
                break;

            case Encoding::SOP1:
                ir.dst = make_sdst(d.dst);
                ir.src0 = decode_operand(d.src0, d.has_literal, d.literal);
                if (d.mnemonic == "s_mov_b32") {
                    ir.op = IrOp::SMov;
                    if (ir.dst.kind == IrOperand::Kind::M0) {
                        if (ir.src0.kind == IrOperand::Kind::Literal) {
                            const_m0 = ir.src0.lit_u32;
                            has_const_m0 = true;
                        } else if (ir.src0.kind == IrOperand::Kind::InlineInt) {
                            const_m0 = static_cast<uint32_t>(ir.src0.imm_i);
                            has_const_m0 = true;
                        } else {
                            has_const_m0 = false;
                        }
                    }
                } else if (d.mnemonic == "s_mov_b64") {
                    ir.op = IrOp::SMovB64;
                    if (d.dst + 2 > fn.max_sgpr) fn.max_sgpr = d.dst + 2;
                    if (ir.src0.kind == IrOperand::Kind::Sgpr && ir.src0.reg + 2 > fn.max_sgpr) fn.max_sgpr = ir.src0.reg + 2;
                    if (ir.src0.kind == IrOperand::Kind::Exec) fn.uses_wave_ops = true;
                } else if (d.mnemonic == "s_not_b32") {
                    ir.op = IrOp::SNotB32;
                } else if (d.mnemonic == "s_not_b64") {
                    ir.op = IrOp::SNotB64;
                    if (d.dst + 2 > fn.max_sgpr) fn.max_sgpr = d.dst + 2;
                    if (ir.src0.kind == IrOperand::Kind::Sgpr && ir.src0.reg + 2 > fn.max_sgpr) fn.max_sgpr = ir.src0.reg + 2;
                    if (ir.src0.kind == IrOperand::Kind::Exec) fn.uses_wave_ops = true;
                } else if (d.mnemonic == "s_and_saveexec_b32" || d.mnemonic == "s_and_saveexec_b64") {
                    ir.op = IrOp::SAndSaveExec;
                    ir.subop = d.mnemonic;
                    fn.uses_wave_ops = true;
                    if (d.mnemonic == "s_and_saveexec_b64" && d.dst + 2 > fn.max_sgpr) fn.max_sgpr = d.dst + 2;
                } else {
                    ir.op = IrOp::Unknown;
                    ir.subop = d.mnemonic;
                    ++fn.unknown_count;
                }
                break;

            case Encoding::SOPK:
                ir.dst = make_sdst(d.dst);
                ir.src0.kind = IrOperand::Kind::InlineInt;
                ir.src0.imm_i = d.simm16;
                if (d.mnemonic == "s_movk_i32") {
                    ir.op = IrOp::SMov;
                    if (ir.dst.kind == IrOperand::Kind::M0) {
                        const_m0 = static_cast<uint32_t>(d.simm16);
                        has_const_m0 = true;
                    }
                } else if (d.mnemonic == "s_addk_i32") {
                    ir.op = IrOp::SAddU32;
                    ir.src1 = ir.dst;
                    if (ir.dst.kind == IrOperand::Kind::M0) has_const_m0 = false;
                } else if (d.mnemonic == "s_mulk_i32") {
                    ir.op = IrOp::SMulI32;
                    ir.src1 = ir.dst;
                    if (ir.dst.kind == IrOperand::Kind::M0) has_const_m0 = false;
                } else {
                    ir.op = IrOp::Unknown;
                    ir.subop = d.mnemonic;
                    ++fn.unknown_count;
                }
                break;

            case Encoding::SOP2:
                ir.dst = make_sdst(d.dst);
                ir.src0 = decode_operand(d.src0, d.has_literal, d.literal);
                ir.src1 = decode_operand(d.src1, d.has_literal, d.literal);
                ir.subop = d.mnemonic;
                if (ir.dst.kind == IrOperand::Kind::M0) has_const_m0 = false;
                if (d.mnemonic == "s_add_u32" || d.mnemonic == "s_add_i32") ir.op = IrOp::SAddU32;
                else if (d.mnemonic == "s_sub_u32" || d.mnemonic == "s_sub_i32") ir.op = IrOp::SSubU32;
                else if (d.mnemonic == "s_addc_u32") ir.op = IrOp::SAddCU32;
                else if (d.mnemonic == "s_subb_u32") ir.op = IrOp::SSubBU32;
                else if (d.mnemonic == "s_cselect_b32") ir.op = IrOp::SCSelectB32;
                else if (d.mnemonic == "s_mul_i32") ir.op = IrOp::SMulI32;
                else if (d.mnemonic == "s_and_b32") ir.op = IrOp::SAndB32;
                else if (d.mnemonic == "s_or_b32") ir.op = IrOp::SOrB32;
                else if (d.mnemonic == "s_xor_b32") ir.op = IrOp::SXorB32;
                else if (d.mnemonic == "s_lshl_b32") ir.op = IrOp::SLshlB32;
                else if (d.mnemonic == "s_lshr_b32") ir.op = IrOp::SLshrB32;
                else if (d.mnemonic == "s_ashr_i32") ir.op = IrOp::SAshrI32;
                else { ir.op = IrOp::Unknown; ++fn.unknown_count; }
                break;

            case Encoding::SOPC:
                ir.op = IrOp::SCmp;
                ir.subop = d.mnemonic;
                ir.src0 = decode_operand(d.src0, d.has_literal, d.literal);
                ir.src1 = decode_operand(d.src1, d.has_literal, d.literal);
                break;

            case Encoding::SMEM:
                if (d.mnemonic == "s_load_dword" || d.mnemonic == "s_load_dwordx2" || d.mnemonic == "s_load_dwordx4") {
                    ir.op = IrOp::SLoadDword;
                    ir.subop = d.mnemonic;
                    ir.dst = make_sgpr(d.dst);
                    ir.src0 = make_sgpr(d.src0);
                    fn.uses_cbuf = true;
                    if (d.mnemonic == "s_load_dwordx2" && d.dst + 2 > fn.max_sgpr) fn.max_sgpr = d.dst + 2;
                    if (d.mnemonic == "s_load_dwordx4" && d.dst + 4 > fn.max_sgpr) fn.max_sgpr = d.dst + 4;
                } else {
                    ir.op = IrOp::Unknown;
                    ir.subop = d.mnemonic;
                    ++fn.unknown_count;
                }
                break;

            case Encoding::DS:
                if (d.mnemonic == "ds_write_b32") {
                    ir.op = IrOp::DsWriteB32;
                    ir.src0 = make_vgpr(d.src0);  // LDS byte address
                    ir.src1 = make_vgpr(d.src1);  // data0
                    fn.uses_lds = true;
                } else if (d.mnemonic == "ds_read_b32") {
                    ir.op = IrOp::DsReadB32;
                    ir.dst = make_vgpr(d.dst);
                    ir.src0 = make_vgpr(d.src0);  // LDS byte address
                    fn.uses_lds = true;
                } else {
                    ir.op = IrOp::Unknown;
                    ir.subop = d.mnemonic;
                    ++fn.unknown_count;
                }
                break;

            case Encoding::VOP1:
                ir.dst = make_vgpr(d.dst);
                ir.src0 = decode_operand(d.src0, d.has_literal, d.literal);
                if (d.mnemonic == "v_nop") ir.op = IrOp::Nop;
                else if (d.mnemonic == "v_mov_b32") ir.op = IrOp::VMov;
                else if (d.mnemonic == "v_readfirstlane_b32") {
                    ir.op = IrOp::VReadFirstLaneB32;
                    ir.dst = make_sgpr(d.dst);
                    fn.uses_wave_ops = true;
                }
                else if (d.mnemonic == "v_cvt_f32_i32") ir.op = IrOp::VCvtF32I32;
                else if (d.mnemonic == "v_cvt_f32_u32") ir.op = IrOp::VCvtF32U32;
                else if (d.mnemonic == "v_cvt_i32_f32") ir.op = IrOp::VCvtI32F32;
                else if (d.mnemonic == "v_cvt_u32_f32") ir.op = IrOp::VCvtU32F32;
                else if (d.mnemonic == "v_rcp_f32") ir.op = IrOp::VRcpF32;
                else if (d.mnemonic == "v_rsq_f32") ir.op = IrOp::VRsqF32;
                else if (d.mnemonic == "v_sqrt_f32") ir.op = IrOp::VSqrtF32;
                else if (d.mnemonic == "v_floor_f32") ir.op = IrOp::VFloorF32;
                else if (d.mnemonic == "v_ceil_f32") ir.op = IrOp::VCeilF32;
                else if (d.mnemonic == "v_fract_f32") ir.op = IrOp::VFractF32;
                else { ir.op = IrOp::Unknown; ir.subop = d.mnemonic; ++fn.unknown_count; }
                break;

            case Encoding::VOP2:
                ir.dst = make_vgpr(d.dst);
                ir.src0 = decode_operand(d.src0, d.has_literal, d.literal);
                ir.src1 = decode_operand(d.src1, d.has_literal, d.literal);
                if (d.mnemonic == "v_add_f32") ir.op = IrOp::VAddF32;
                else if (d.mnemonic == "v_sub_f32") ir.op = IrOp::VSubF32;
                else if (d.mnemonic == "v_subrev_f32") ir.op = IrOp::VSubRevF32;
                else if (d.mnemonic == "v_mul_f32") ir.op = IrOp::VMulF32;
                else if (d.mnemonic == "v_min_f32") ir.op = IrOp::VMinF32;
                else if (d.mnemonic == "v_max_f32") ir.op = IrOp::VMaxF32;
                else if (d.mnemonic == "v_min_i32") ir.op = IrOp::VMinI32;
                else if (d.mnemonic == "v_max_i32") ir.op = IrOp::VMaxI32;
                else if (d.mnemonic == "v_min_u32") ir.op = IrOp::VMinU32;
                else if (d.mnemonic == "v_max_u32") ir.op = IrOp::VMaxU32;
                else if (d.mnemonic == "v_add_nc_u32") ir.op = IrOp::VAddU32;
                else if (d.mnemonic == "v_sub_nc_u32") ir.op = IrOp::VSubU32;
                else if (d.mnemonic == "v_subrev_nc_u32") ir.op = IrOp::VSubRevU32;
                else if (d.mnemonic == "v_and_b32") ir.op = IrOp::VAndB32;
                else if (d.mnemonic == "v_or_b32") ir.op = IrOp::VOrB32;
                else if (d.mnemonic == "v_xor_b32") ir.op = IrOp::VXorB32;
                else if (d.mnemonic == "v_lshlrev_b32") ir.op = IrOp::VLshlRevB32;
                else if (d.mnemonic == "v_lshrrev_b32") ir.op = IrOp::VLshrRevB32;
                else if (d.mnemonic == "v_ashrrev_i32") ir.op = IrOp::VAshrRevI32;
                else if (d.mnemonic == "v_fmac_f32") { ir.op = IrOp::VFmaF32; ir.src2 = ir.dst; }
                else if (d.mnemonic == "v_fmaak_f32") {
                    ir.op = IrOp::VFmaF32;
                    ir.src2.kind = IrOperand::Kind::Literal;
                    ir.src2.lit_u32 = d.literal;
                } else if (d.mnemonic == "v_fmamk_f32") {
                    ir.op = IrOp::VFmaF32;
                    ir.src2 = ir.src1;
                    ir.src1.kind = IrOperand::Kind::Literal;
                    ir.src1.lit_u32 = d.literal;
                } else if (d.mnemonic == "v_cndmask_b32") {
                    ir.op = IrOp::VCndMask;
                } else {
                    ir.op = IrOp::Unknown;
                    ir.subop = d.mnemonic;
                    ++fn.unknown_count;
                }
                break;

            case Encoding::VOPC:
                ir.op = IrOp::VCmp;
                ir.subop = d.mnemonic;
                ir.src0 = decode_operand(d.src0, d.has_literal, d.literal);
                ir.src1 = decode_operand(d.src1, d.has_literal, d.literal);
                break;

            case Encoding::VOP3:
                ir.dst = make_vgpr(d.dst);
                ir.src0 = decode_operand(d.src0, d.has_literal, d.literal, (d.abs_mask & 1) != 0, (d.neg_mask & 1) != 0);
                ir.src1 = decode_operand(d.src1, d.has_literal, d.literal, (d.abs_mask & 2) != 0, (d.neg_mask & 2) != 0);
                ir.src2 = decode_operand(d.src2, d.has_literal, d.literal, (d.abs_mask & 4) != 0, (d.neg_mask & 4) != 0);
                if (d.mnemonic == "v_fma_f32" || d.mnemonic == "v_mad_f32") ir.op = IrOp::VFmaF32;
                else if (d.mnemonic == "v_mul_lo_u32") ir.op = IrOp::VMulLoU32;
                else if (d.mnemonic == "v_mad_u32_u24") ir.op = IrOp::VMadU32U24;
                else if (d.mnemonic == "v_bfe_u32") ir.op = IrOp::VBfeU32;
                else if (d.mnemonic == "v_bfi_b32") ir.op = IrOp::VBfiB32;
                else if (d.mnemonic == "v_add_f32") ir.op = IrOp::VAddF32;
                else if (d.mnemonic == "v_sub_f32") ir.op = IrOp::VSubF32;
                else if (d.mnemonic == "v_subrev_f32") ir.op = IrOp::VSubRevF32;
                else if (d.mnemonic == "v_mul_f32") ir.op = IrOp::VMulF32;
                else if (d.mnemonic == "v_add_nc_u32") ir.op = IrOp::VAddU32;
                else if (d.mnemonic == "v_sub_nc_u32") ir.op = IrOp::VSubU32;
                else if (d.mnemonic == "v_subrev_nc_u32") ir.op = IrOp::VSubRevU32;
                else if (d.mnemonic == "v_and_b32") ir.op = IrOp::VAndB32;
                else if (d.mnemonic == "v_or_b32") ir.op = IrOp::VOrB32;
                else if (d.mnemonic == "v_xor_b32") ir.op = IrOp::VXorB32;
                else if (d.mnemonic == "v_lshlrev_b32") ir.op = IrOp::VLshlRevB32;
                else if (d.mnemonic == "v_lshrrev_b32") ir.op = IrOp::VLshrRevB32;
                else if (d.mnemonic == "v_ashrrev_i32") ir.op = IrOp::VAshrRevI32;
                else if (d.mnemonic == "v_min_f32") ir.op = IrOp::VMinF32;
                else if (d.mnemonic == "v_max_f32") ir.op = IrOp::VMaxF32;
                else if (d.mnemonic == "v_min_i32") ir.op = IrOp::VMinI32;
                else if (d.mnemonic == "v_max_i32") ir.op = IrOp::VMaxI32;
                else if (d.mnemonic == "v_min_u32") ir.op = IrOp::VMinU32;
                else if (d.mnemonic == "v_max_u32") ir.op = IrOp::VMaxU32;
                else if (d.mnemonic == "v_cndmask_b32") ir.op = IrOp::VCndMask;
                else if (d.mnemonic == "v_med3_f32") ir.op = IrOp::VMed3F32;
                else if (d.mnemonic == "v_bfe_i32") ir.op = IrOp::VBfeI32;
                else if (d.mnemonic == "v_readlane_b32") {
                    ir.op = IrOp::VReadLaneB32;
                    ir.dst = make_sgpr(d.dst);
                    fn.uses_wave_ops = true;
                } else if (d.mnemonic == "v_writelane_b32") {
                    ir.op = IrOp::VWriteLaneB32;
                    fn.uses_wave_ops = true;
                } else if (d.mnemonic == "v_mbcnt_lo_u32_b32") {
                    ir.op = IrOp::VMbcntLoU32B32;
                    fn.uses_wave_ops = true;
                } else { ir.op = IrOp::Unknown; ir.subop = d.mnemonic; ++fn.unknown_count; }
                break;

            case Encoding::FLAT:
                if (d.mnemonic == "global_load_dword") {
                    ir.op = IrOp::GlobalLoadDword;
                    ir.dst = make_vgpr(d.dst);
                    ir.src0 = make_vgpr(d.src0);
                    fn.uses_mem = true;
                } else if (d.mnemonic == "global_load_dwordx2") {
                    ir.op = IrOp::GlobalLoadDwordx2;
                    ir.dst = make_vgpr(d.dst);
                    ir.src0 = make_vgpr(d.src0);
                    if (d.dst + 2 > fn.max_vgpr) fn.max_vgpr = d.dst + 2;
                    fn.uses_mem = true;
                } else if (d.mnemonic == "global_load_dwordx4") {
                    ir.op = IrOp::GlobalLoadDwordx4;
                    ir.dst = make_vgpr(d.dst);
                    ir.src0 = make_vgpr(d.src0);
                    if (d.dst + 4 > fn.max_vgpr) fn.max_vgpr = d.dst + 4;
                    fn.uses_mem = true;
                } else if (d.mnemonic == "global_store_dword") {
                    ir.op = IrOp::GlobalStoreDword;
                    ir.src0 = make_vgpr(d.src0);  // addr
                    ir.src1 = make_vgpr(d.src1);  // data
                    fn.uses_mem = true;
                } else if (d.mnemonic == "global_store_dwordx2") {
                    ir.op = IrOp::GlobalStoreDwordx2;
                    ir.src0 = make_vgpr(d.src0);  // addr
                    ir.src1 = make_vgpr(d.src1);  // data
                    if (d.src1 + 2 > fn.max_vgpr) fn.max_vgpr = d.src1 + 2;
                    fn.uses_mem = true;
                } else if (d.mnemonic == "global_store_dwordx4") {
                    ir.op = IrOp::GlobalStoreDwordx4;
                    ir.src0 = make_vgpr(d.src0);  // addr
                    ir.src1 = make_vgpr(d.src1);  // data
                    if (d.src1 + 4 > fn.max_vgpr) fn.max_vgpr = d.src1 + 4;
                    fn.uses_mem = true;
                } else {
                    ir.op = IrOp::Unknown;
                    ir.subop = d.mnemonic;
                    ++fn.unknown_count;
                }
                break;

            case Encoding::MUBUF:
            case Encoding::MTBUF: {
                auto track_vspan = [&](uint32_t base, uint32_t n) {
                    if (base + n > fn.max_vgpr) fn.max_vgpr = base + n;
                };
                if (d.mnemonic == "buffer_load_dword" || d.mnemonic == "tbuffer_load_format_x") {
                    ir.op = IrOp::BufferLoadDword;
                    ir.dst = make_vgpr(d.dst);
                    ir.src0 = make_vgpr(d.src0);
                    ir.src3 = decode_operand(d.src3, false, 0);
                    ir.rsrc_slot = alloc_buf_srv_slot(d.src2);
                    fn.uses_buf_srv = true;
                } else if (d.mnemonic == "buffer_load_dwordx2" || d.mnemonic == "tbuffer_load_format_xy") {
                    ir.op = IrOp::BufferLoadDwordx2;
                    ir.dst = make_vgpr(d.dst);
                    ir.src0 = make_vgpr(d.src0);
                    ir.src3 = decode_operand(d.src3, false, 0);
                    ir.rsrc_slot = alloc_buf_srv_slot(d.src2);
                    track_vspan(d.dst, 2);
                    fn.uses_buf_srv = true;
                } else if (d.mnemonic == "buffer_load_dwordx4" || d.mnemonic == "tbuffer_load_format_xyzw") {
                    ir.op = IrOp::BufferLoadDwordx4;
                    ir.dst = make_vgpr(d.dst);
                    ir.src0 = make_vgpr(d.src0);
                    ir.src3 = decode_operand(d.src3, false, 0);
                    ir.rsrc_slot = alloc_buf_srv_slot(d.src2);
                    track_vspan(d.dst, 4);
                    fn.uses_buf_srv = true;
                } else if (d.mnemonic == "buffer_store_dword" || d.mnemonic == "tbuffer_store_format_x") {
                    ir.op = IrOp::BufferStoreDword;
                    ir.src0 = make_vgpr(d.src0);
                    ir.src1 = make_vgpr(d.src1);
                    ir.src3 = decode_operand(d.src3, false, 0);
                    fn.uses_mem = true;
                } else if (d.mnemonic == "buffer_store_dwordx2") {
                    ir.op = IrOp::BufferStoreDwordx2;
                    ir.src0 = make_vgpr(d.src0);
                    ir.src1 = make_vgpr(d.src1);
                    ir.src3 = decode_operand(d.src3, false, 0);
                    track_vspan(d.src1, 2);
                    fn.uses_mem = true;
                } else if (d.mnemonic == "buffer_store_dwordx4" || d.mnemonic == "tbuffer_store_format_xyzw") {
                    ir.op = IrOp::BufferStoreDwordx4;
                    ir.src0 = make_vgpr(d.src0);
                    ir.src1 = make_vgpr(d.src1);
                    ir.src3 = decode_operand(d.src3, false, 0);
                    track_vspan(d.src1, 4);
                    fn.uses_mem = true;
                } else {
                    ir.op = IrOp::Unknown;
                    ir.subop = d.mnemonic;
                    ++fn.unknown_count;
                }
                break;
            }

            case Encoding::MIMG: {
                ir.dmask = d.dmask;
                ir.tfe = d.tfe;
                uint32_t out_regs = 0;
                for (int c = 0; c < 4; ++c) {
                    if (d.dmask & (1u << c)) ++out_regs;
                }
                if (out_regs == 0) out_regs = 1;
                const uint32_t total_out_regs = out_regs + (d.tfe ? 1u : 0u);
                if (d.mnemonic == "image_sample" || d.mnemonic == "image_sample_l") {
                    ir.op = IrOp::ImageSample;
                    ir.dst = make_vgpr(d.dst);
                    ir.src0 = make_vgpr(d.src0);
                    ir.rsrc_slot = alloc_tex_slot(d.src2);
                    ir.samp_slot = alloc_samp_slot(d.src3);
                    if (d.dst + total_out_regs > fn.max_vgpr) fn.max_vgpr = d.dst + total_out_regs;
                    if (d.src0 + 2 > fn.max_vgpr) fn.max_vgpr = d.src0 + 2;
                    fn.uses_tex = true;
                } else if (d.mnemonic == "image_load") {
                    ir.op = IrOp::ImageLoad;
                    ir.dst = make_vgpr(d.dst);
                    ir.src0 = make_vgpr(d.src0);
                    ir.rsrc_slot = alloc_tex_slot(d.src2);
                    if (d.dst + total_out_regs > fn.max_vgpr) fn.max_vgpr = d.dst + total_out_regs;
                    if (d.src0 + 2 > fn.max_vgpr) fn.max_vgpr = d.src0 + 2;
                    fn.uses_tex = true;
                } else if (d.mnemonic == "image_get_resinfo") {
                    ir.op = IrOp::ImageGetResInfo;
                    ir.dst = make_vgpr(d.dst);
                    ir.src0 = make_vgpr(d.src0);
                    ir.rsrc_slot = alloc_tex_slot(d.src2);
                    if (d.dst + out_regs > fn.max_vgpr) fn.max_vgpr = d.dst + out_regs;
                    fn.uses_tex = true;
                } else if (d.mnemonic == "image_bvh_intersect_ray" || d.mnemonic == "image_bvh64_intersect_ray") {
                    ir.op = IrOp::ImageBvhIntersectRay;
                    ir.dst = make_vgpr(d.dst);
                    ir.src0 = make_vgpr(d.src0);
                    ir.rsrc_slot = alloc_as_slot(d.src2);
                    if (d.dst + 4 > fn.max_vgpr) fn.max_vgpr = d.dst + 4;
                    if (d.src0 + 8 > fn.max_vgpr) fn.max_vgpr = d.src0 + 8;
                    fn.uses_ray_tracing = true;
                } else {
                    ir.op = IrOp::Unknown;
                    ir.subop = d.mnemonic;
                    ++fn.unknown_count;
                }
                break;
            }

            case Encoding::EXP:
                ir.exp_target = static_cast<uint8_t>(d.opcode);
                ir.exp_en = d.exp_en;
                ir.exp_done = d.exp_done;
                ir.src0 = make_vgpr(d.src0);
                ir.src1 = make_vgpr(d.src1);
                ir.src2 = make_vgpr(d.src2);
                ir.src3 = make_vgpr(d.src3);
                if (d.opcode == kExpTargetPrim0) {
                    ir.op = IrOp::MeshExportPrimitive;
                    ir.subop = "exp_prim0";
                } else if (is_mesh_shader && ((d.opcode >= 12 && d.opcode <= 15) || (d.opcode >= 32 && d.opcode <= 63))) {
                    ir.op = IrOp::MeshExportVertex;
                    ir.subop = (d.opcode <= 15) ? std::format("exp_pos{}", d.opcode - 12)
                                                : std::format("exp_param{}", d.opcode - 32);
                    if (d.opcode >= 32 && d.opcode <= 63) {
                        fn.mesh_param_mask |= (1u << (d.opcode - 32));
                    }
                } else {
                    ir.op = IrOp::Exp;
                    ir.subop = "exp";
                    if (d.opcode <= 7) fn.mrt_mask |= (1u << d.opcode);
                }
                break;

            default:
                ir.op = IrOp::Unknown;
                ir.subop = d.mnemonic;
                ++fn.unknown_count;
                break;
        }

        track_regs(ir.dst, fn.max_vgpr, fn.max_sgpr);
        track_regs(ir.src0, fn.max_vgpr, fn.max_sgpr);
        track_regs(ir.src1, fn.max_vgpr, fn.max_sgpr);
        track_regs(ir.src2, fn.max_vgpr, fn.max_sgpr);
        track_regs(ir.src3, fn.max_vgpr, fn.max_sgpr);

        fn.blocks[cur_blk].insts.push_back(std::move(ir));
        fn.blocks[cur_blk].end_pc = d.pc + d.size;
    }

    // 3. Build CFG successor / predecessor edges
    for (uint32_t b = 0; b < fn.blocks.size(); ++b) {
        auto& blk = fn.blocks[b];
        if (blk.insts.empty()) continue;
        const auto& last = blk.insts.back();
        if (last.op == IrOp::EndPgm) {
            continue;
        } else if (last.op == IrOp::Branch) {
            blk.successors.push_back(last.target_block);
        } else if (last.op == IrOp::CondBranch) {
            blk.successors.push_back(last.target_block);
            if (b + 1 < fn.blocks.size()) blk.successors.push_back(b + 1);
        } else if (b + 1 < fn.blocks.size()) {
            blk.successors.push_back(b + 1);
        }
        for (uint32_t succ : blk.successors) {
            if (succ < fn.blocks.size()) fn.blocks[succ].predecessors.push_back(b);
        }
    }

    return fn;
}

std::string dump_ir(const IrFunction& fn) {
    std::ostringstream oss;
    oss << std::format("; KestrelX IR: {} blocks, {} insts, max_v={}, max_s={}\n",
                       fn.blocks.size(), fn.inst_count, fn.max_vgpr, fn.max_sgpr);
    for (const auto& b : fn.blocks) {
        oss << std::format("bb_{} [pc={:#x}..{:#x}]:\n", b.id, b.start_pc, b.end_pc);
        for (const auto& inst : b.insts) {
            oss << std::format("  {:#06x}: op={} {}\n", inst.pc, static_cast<int>(inst.op), inst.subop);
        }
    }
    return oss.str();
}

}  // namespace kestrel::gpu
