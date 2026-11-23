#include "kestrel/gpu/rdna2_isa.hpp"

#include <format>

namespace kestrel::gpu {

namespace {

const char* sopp_name(uint32_t op) {
    switch (op) {
        case 0: return "s_nop";
        case 1: return "s_endpgm";
        case 2: return "s_branch";
        case 4: return "s_cbranch_scc0";
        case 5: return "s_cbranch_scc1";
        case 6: return "s_cbranch_vccz";
        case 7: return "s_cbranch_vccnz";
        case 8: return "s_cbranch_execz";
        case 9: return "s_cbranch_execnz";
        case 10: return "s_barrier";
        case 12: return "s_waitcnt";
        case 27: return "s_endpgm_saved";
        default: return nullptr;
    }
}

const char* sop1_name(uint32_t op) {
    switch (op) {
        case 3: return "s_mov_b32";
        case 4: return "s_mov_b64";
        case 7: return "s_not_b32";
        case 8: return "s_not_b64";
        case 28: return "s_getpc_b64";
        case 32: return "s_and_saveexec_b64";
        case 60: return "s_and_saveexec_b32";
        default: return nullptr;
    }
}

const char* sop2_name(uint32_t op) {
    switch (op) {
        case 0: return "s_add_u32";
        case 1: return "s_sub_u32";
        case 2: return "s_add_i32";
        case 3: return "s_sub_i32";
        case 4: return "s_addc_u32";
        case 5: return "s_subb_u32";
        case 10: return "s_cselect_b32";
        case 14: return "s_and_b32";
        case 16: return "s_or_b32";
        case 18: return "s_xor_b32";
        case 30: return "s_lshl_b32";
        case 32: return "s_lshr_b32";
        case 34: return "s_ashr_i32";
        case 44: return "s_mul_i32";
        default: return nullptr;
    }
}

const char* sopk_name(uint32_t op) {
    switch (op) {
        case 0: return "s_movk_i32";
        case 15: return "s_addk_i32";
        case 16: return "s_mulk_i32";
        default: return nullptr;
    }
}

const char* sopc_name(uint32_t op) {
    switch (op) {
        case 0: return "s_cmp_eq_i32";
        case 1: return "s_cmp_lg_i32";
        case 2: return "s_cmp_gt_i32";
        case 3: return "s_cmp_ge_i32";
        case 4: return "s_cmp_lt_i32";
        case 5: return "s_cmp_le_i32";
        case 6: return "s_cmp_eq_u32";
        case 7: return "s_cmp_lg_u32";
        case 8: return "s_cmp_gt_u32";
        case 9: return "s_cmp_ge_u32";
        case 10: return "s_cmp_lt_u32";
        case 11: return "s_cmp_le_u32";
        default: return nullptr;
    }
}

const char* vop1_name(uint32_t op) {
    switch (op) {
        case 0: return "v_nop";
        case 1: return "v_mov_b32";
        case 5: return "v_cvt_f32_i32";
        case 6: return "v_cvt_f32_u32";
        case 7: return "v_cvt_u32_f32";
        case 8: return "v_cvt_i32_f32";
        case 32: return "v_fract_f32";
        case 34: return "v_ceil_f32";
        case 36: return "v_floor_f32";
        case 37: return "v_exp_f32";
        case 39: return "v_log_f32";
        case 42: return "v_rcp_f32";
        case 46: return "v_rsq_f32";
        case 51: return "v_sqrt_f32";
        default: return nullptr;
    }
}

const char* vop2_name(uint32_t op) {
    switch (op) {
        case 1: return "v_cndmask_b32";
        case 3: return "v_add_f32";
        case 4: return "v_sub_f32";
        case 5: return "v_subrev_f32";
        case 7: return "v_mul_f32";
        case 15: return "v_min_f32";
        case 16: return "v_max_f32";
        case 17: return "v_min_i32";
        case 18: return "v_max_i32";
        case 19: return "v_min_u32";
        case 20: return "v_max_u32";
        case 22: return "v_lshrrev_b32";
        case 24: return "v_ashrrev_i32";
        case 26: return "v_lshlrev_b32";
        case 27: return "v_and_b32";
        case 28: return "v_or_b32";
        case 29: return "v_xor_b32";
        case 37: return "v_add_nc_u32";
        case 38: return "v_sub_nc_u32";
        case 39: return "v_subrev_nc_u32";
        case 43: return "v_fmac_f32";
        case 44: return "v_fmamk_f32";
        case 45: return "v_fmaak_f32";
        default: return nullptr;
    }
}

const char* vopc_name(uint32_t op) {
    switch (op) {
        case 0x11: return "v_cmp_lt_f32";
        case 0x12: return "v_cmp_eq_f32";
        case 0x13: return "v_cmp_le_f32";
        case 0x14: return "v_cmp_gt_f32";
        case 0x15: return "v_cmp_lg_f32";
        case 0x16: return "v_cmp_ge_f32";
        case 0x81: return "v_cmp_lt_i32";
        case 0x82: return "v_cmp_eq_i32";
        case 0x83: return "v_cmp_le_i32";
        case 0x84: return "v_cmp_gt_i32";
        case 0x85: return "v_cmp_ne_i32";
        case 0x86: return "v_cmp_ge_i32";
        case 0xC1: return "v_cmp_lt_u32";
        case 0xC2: return "v_cmp_eq_u32";
        case 0xC3: return "v_cmp_le_u32";
        case 0xC4: return "v_cmp_gt_u32";
        case 0xC5: return "v_cmp_ne_u32";
        case 0xC6: return "v_cmp_ge_u32";
        default: return nullptr;
    }
}

const char* vop3_name(uint32_t op) {
    switch (op) {
        case 0x14B: return "v_fma_f32";
        case 0x148: return "v_bfe_u32";
        case 0x14A: return "v_bfi_b32";
        case 0x151: return "v_med3_f32";
        case 0x169: return "v_mul_lo_u32";
        case 0x143: return "v_mad_u32_u24";
        default: break;
    }
    if (op >= 0x100 && op < 0x140) return vop2_name(op - 0x100);
    if (op >= 0x180 && op < 0x200) return vop1_name(op - 0x180);
    return nullptr;
}

const char* flat_name(uint32_t op) {
    switch (op) {
        case 12: return "global_load_dword";
        case 13: return "global_load_dwordx2";
        case 14: return "global_load_dwordx4";
        case 28: return "global_store_dword";
        case 29: return "global_store_dwordx2";
        case 30: return "global_store_dwordx4";
        default: return nullptr;
    }
}

}  // namespace

std::string format_rdna2_operand(uint32_t op, bool has_lit, uint32_t lit) {
    if (op < 106) return std::format("s{}", op);
    if (op == 106) return "vcc_lo";
    if (op == 107) return "vcc_hi";
    if (op == 124) return "m0";
    if (op == 126) return "exec_lo";
    if (op == 127) return "exec_hi";
    if (op >= 128 && op <= 192) return std::format("{}", static_cast<int>(op) - 128);
    if (op >= 193 && op <= 208) return std::format("{}", 192 - static_cast<int>(op));
    if (op == 240) return "0.5";
    if (op == 241) return "-0.5";
    if (op == 242) return "1.0";
    if (op == 243) return "-1.0";
    if (op == 244) return "2.0";
    if (op == 245) return "-2.0";
    if (op == 246) return "4.0";
    if (op == 247) return "-4.0";
    if (op == 248) return "0.15915494";
    if (op == 255 && has_lit) return hex32(lit);
    if (op >= 256 && op < 512) return std::format("v{}", op - 256);
    return std::format("op_{}", op);
}

std::vector<DecodedInst> decode_rdna2(const uint32_t* words, size_t count) {
    std::vector<DecodedInst> out;
    size_t i = 0;
    while (i < count) {
        const uint32_t w0 = words[i];
        DecodedInst d;
        d.pc = static_cast<uint32_t>(i * 4);
        d.size = 4;

        auto consume_lit = [&]() {
            if (i + 1 < count) {
                d.has_literal = true;
                d.literal = words[i + 1];
                d.size += 4;
            }
        };

        if ((w0 >> 23) == 0x17Fu) {
            d.enc = Encoding::SOPP;
            d.opcode = (w0 >> 16) & 0x7Fu;
            d.simm16 = static_cast<int16_t>(w0 & 0xFFFFu);
            const char* n = sopp_name(d.opcode);
            d.mnemonic = n ? n : std::format("sopp_{}", d.opcode);
            if (d.opcode == 1 || d.opcode == 27) d.is_terminator = true;
            if (d.opcode == 2 || (d.opcode >= 4 && d.opcode <= 9)) {
                d.is_branch = true;
                d.branch_target = static_cast<uint32_t>(static_cast<int64_t>(d.pc) + 4 + static_cast<int64_t>(d.simm16) * 4);
            }
        } else if ((w0 >> 23) == 0x17Du) {
            d.enc = Encoding::SOP1;
            d.dst = (w0 >> 16) & 0x7Fu;
            d.opcode = (w0 >> 8) & 0xFFu;
            d.src0 = w0 & 0xFFu;
            const char* n = sop1_name(d.opcode);
            d.mnemonic = n ? n : std::format("sop1_{}", d.opcode);
            if (d.src0 == 255) consume_lit();
        } else if ((w0 >> 23) == 0x17Eu) {
            d.enc = Encoding::SOPC;
            d.opcode = (w0 >> 16) & 0x7Fu;
            d.src1 = (w0 >> 8) & 0xFFu;
            d.src0 = w0 & 0xFFu;
            const char* n = sopc_name(d.opcode);
            d.mnemonic = n ? n : std::format("sopc_{}", d.opcode);
            if (d.src0 == 255 || d.src1 == 255) consume_lit();
        } else if ((w0 >> 28) == 0xBu) {
            d.enc = Encoding::SOPK;
            d.opcode = (w0 >> 23) & 0x1Fu;
            d.dst = (w0 >> 16) & 0x7Fu;
            d.simm16 = static_cast<int16_t>(w0 & 0xFFFFu);
            const char* n = sopk_name(d.opcode);
            d.mnemonic = n ? n : std::format("sopk_{}", d.opcode);
        } else if ((w0 >> 30) == 0x2u) {
            d.enc = Encoding::SOP2;
            d.opcode = (w0 >> 23) & 0x7Fu;
            d.dst = (w0 >> 16) & 0x7Fu;
            d.src1 = (w0 >> 8) & 0xFFu;
            d.src0 = w0 & 0xFFu;
            const char* n = sop2_name(d.opcode);
            d.mnemonic = n ? n : std::format("sop2_{}", d.opcode);
            if (d.src0 == 255 || d.src1 == 255) consume_lit();
        } else if ((w0 >> 26) == 0x35u && i + 1 < count) {
            const uint32_t w1 = words[i + 1];
            d.size = 8;
            d.enc = Encoding::VOP3;
            d.opcode = (w0 >> 16) & 0x3FFu;
            d.clamp = ((w0 >> 15) & 1u) != 0;
            d.abs_mask = static_cast<uint8_t>((w0 >> 8) & 0x7u);
            d.dst = w0 & 0xFFu;
            d.neg_mask = static_cast<uint8_t>((w1 >> 29) & 0x7u);
            d.src2 = (w1 >> 18) & 0x1FFu;
            d.src1 = (w1 >> 9) & 0x1FFu;
            d.src0 = w1 & 0x1FFu;
            const char* n = vop3_name(d.opcode);
            d.mnemonic = n ? n : std::format("vop3_{:#x}", d.opcode);
            if ((d.src0 == 255 || d.src1 == 255 || d.src2 == 255) && i + 2 < count) {
                d.has_literal = true;
                d.literal = words[i + 2];
                d.size = 12;
            }
        } else if ((w0 >> 26) == 0x37u && i + 1 < count) {
            const uint32_t w1 = words[i + 1];
            d.size = 8;
            d.enc = Encoding::FLAT;
            d.opcode = (w0 >> 18) & 0x7Fu;
            d.offset = w0 & 0x1FFFu;
            d.dst = (w1 >> 24) & 0xFFu;
            d.src2 = (w1 >> 16) & 0x7Fu;  // saddr
            d.src1 = (w1 >> 8) & 0xFFu;   // data
            d.src0 = w1 & 0xFFu;          // vaddr
            const char* n = flat_name(d.opcode);
            d.mnemonic = n ? n : std::format("flat_{}", d.opcode);
        } else if ((w0 >> 26) == 0x3Cu && i + 1 < count) {
            d.size = 8;
            d.enc = Encoding::SMEM;
            d.opcode = (w0 >> 18) & 0xFFu;
            d.dst = (w0 >> 6) & 0x7Fu;
            d.src0 = w0 & 0x3Fu;
            d.offset = words[i + 1] & 0x1FFFFFu;
            d.mnemonic = std::format("smem_{}", d.opcode);
        } else if ((w0 >> 26) == 0x38u && i + 1 < count) {
            d.size = 8;
            d.enc = Encoding::MUBUF;
            d.opcode = (w0 >> 18) & 0x7Fu;
            d.offset = w0 & 0xFFFu;
            d.mnemonic = std::format("mubuf_{}", d.opcode);
        } else if ((w0 >> 26) == 0x36u && i + 1 < count) {
            d.size = 8;
            d.enc = Encoding::DS;
            d.opcode = (w0 >> 18) & 0xFFu;
            d.mnemonic = std::format("ds_{}", d.opcode);
        } else if ((w0 >> 26) == 0x3Eu && i + 1 < count) {
            d.size = 8;
            d.enc = Encoding::EXP;
            d.opcode = (w0 >> 4) & 0x3Fu;
            d.mnemonic = "exp";
        } else if ((w0 >> 25) == 0x3Fu) {
            d.enc = Encoding::VOP1;
            d.dst = (w0 >> 17) & 0xFFu;
            d.opcode = (w0 >> 9) & 0xFFu;
            d.src0 = w0 & 0x1FFu;
            const char* n = vop1_name(d.opcode);
            d.mnemonic = n ? n : std::format("vop1_{}", d.opcode);
            if (d.src0 == 255) consume_lit();
        } else if ((w0 >> 25) == 0x3Eu) {
            d.enc = Encoding::VOPC;
            d.opcode = (w0 >> 17) & 0xFFu;
            d.src1 = 256u + ((w0 >> 9) & 0xFFu);
            d.src0 = w0 & 0x1FFu;
            const char* n = vopc_name(d.opcode);
            d.mnemonic = n ? n : std::format("vopc_{:#x}", d.opcode);
            if (d.src0 == 255) consume_lit();
        } else if ((w0 >> 31) == 0u) {
            d.enc = Encoding::VOP2;
            d.opcode = (w0 >> 25) & 0x3Fu;
            d.dst = (w0 >> 17) & 0xFFu;
            d.src1 = 256u + ((w0 >> 9) & 0xFFu);
            d.src0 = w0 & 0x1FFu;
            const char* n = vop2_name(d.opcode);
            d.mnemonic = n ? n : std::format("vop2_{}", d.opcode);
            if (d.src0 == 255 || d.opcode == 44 || d.opcode == 45) consume_lit();
        } else {
            d.mnemonic = std::format(".dword {}", hex32(w0));
        }

        out.push_back(d);
        i += d.size / 4;
    }
    return out;
}

std::string disassemble_inst(const DecodedInst& d) {
    switch (d.enc) {
        case Encoding::SOPP:
            return d.is_branch ? std::format("{} -> {:#x}", d.mnemonic, d.branch_target)
                               : (d.simm16 ? std::format("{} {}", d.mnemonic, d.simm16) : d.mnemonic);
        case Encoding::SOP1:
            return std::format("{} s{}, {}", d.mnemonic, d.dst, format_rdna2_operand(d.src0, d.has_literal, d.literal));
        case Encoding::SOP2:
            return std::format("{} s{}, {}, {}", d.mnemonic, d.dst,
                               format_rdna2_operand(d.src0, d.has_literal, d.literal),
                               format_rdna2_operand(d.src1, d.has_literal, d.literal));
        case Encoding::SOPK:
            return std::format("{} s{}, {}", d.mnemonic, d.dst, d.simm16);
        case Encoding::SOPC:
            return std::format("{} {}, {}", d.mnemonic,
                               format_rdna2_operand(d.src0, d.has_literal, d.literal),
                               format_rdna2_operand(d.src1, d.has_literal, d.literal));
        case Encoding::VOP1:
            return std::format("{} v{}, {}", d.mnemonic, d.dst, format_rdna2_operand(d.src0, d.has_literal, d.literal));
        case Encoding::VOP2:
            return std::format("{} v{}, {}, v{}", d.mnemonic, d.dst,
                               format_rdna2_operand(d.src0, d.has_literal, d.literal), d.src1 - 256);
        case Encoding::VOPC:
            return std::format("{} vcc, {}, v{}", d.mnemonic,
                               format_rdna2_operand(d.src0, d.has_literal, d.literal), d.src1 - 256);
        case Encoding::VOP3:
            return std::format("{} v{}, {}, {}, {}", d.mnemonic, d.dst,
                               format_rdna2_operand(d.src0, d.has_literal, d.literal),
                               format_rdna2_operand(d.src1, d.has_literal, d.literal),
                               format_rdna2_operand(d.src2, d.has_literal, d.literal));
        case Encoding::FLAT:
            return std::format("{} v{}, v{}, off:{:#x}", d.mnemonic, d.dst, d.src0, d.offset);
        default:
            return d.mnemonic;
    }
}

}  // namespace kestrel::gpu
