#include "kestrel/gpu/rdna2_isa.hpp"

#include <format>

namespace kestrel::gpu {

namespace {

const char* sopp_name(uint32_t op) {
    switch (op) {
        case 0: return "s_nop";
        case 1: return "s_endpgm";
        case 2: return "s_branch";
        case 3: return "s_wakeup";
        case 4: return "s_cbranch_scc0";
        case 5: return "s_cbranch_scc1";
        case 6: return "s_cbranch_vccz";
        case 7: return "s_cbranch_vccnz";
        case 8: return "s_cbranch_execz";
        case 9: return "s_cbranch_execnz";
        case 10: return "s_barrier";
        case 12: return "s_waitcnt";
        case 16: return "s_sendmsg";
        case 27: return "s_endpgm_saved";
        case 33: return "s_clause";
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
        case 63: return "s_andn2_saveexec_b32";
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
        case 20: return "s_andn2_b32";
        case 30: return "s_lshl_b32";
        case 32: return "s_lshr_b32";
        case 34: return "s_ashr_i32";
        case 38: return "s_mul_i32";
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
        case 2: return "v_readfirstlane_b32";
        case 5: return "v_cvt_f32_i32";
        case 6: return "v_cvt_f32_u32";
        case 7: return "v_cvt_u32_f32";
        case 8: return "v_cvt_i32_f32";
        case 32: return "v_fract_f32";
        case 33: return "v_trunc_f32";
        case 34: return "v_ceil_f32";
        case 35: return "v_rndne_f32";
        case 36: return "v_floor_f32";
        case 37: return "v_exp_f32";
        case 39: return "v_log_f32";
        case 42: return "v_rcp_f32";
        case 46: return "v_rsq_f32";
        case 51: return "v_sqrt_f32";
        case 53: return "v_sin_f32";
        case 54: return "v_cos_f32";
        case 63: return "v_frexp_exp_i32_f32";
        case 64: return "v_frexp_mant_f32";
        default: return nullptr;
    }
}

const char* vop2_name(uint32_t op) {
    switch (op) {
        case 1: return "v_cndmask_b32";
        case 3: return "v_add_f32";
        case 4: return "v_sub_f32";
        case 5: return "v_subrev_f32";
        case 7: return "v_mul_legacy_f32";
        case 8: return "v_mul_f32";
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
        case 40: return "v_add_co_ci_u32";
        case 41: return "v_sub_co_ci_u32";
        case 42: return "v_subrev_co_ci_u32";
        case 43: return "v_fmac_f32";
        case 44: return "v_fmamk_f32";
        case 45: return "v_fmaak_f32";
        default: return nullptr;
    }
}

const char* vopc_name(uint32_t op) {
    switch (op) {
        // Float V_CMP_*_F32 (GFX10.3: 0x00..0x0F)
        case 0x00: return "v_cmp_f_f32";
        case 0x01: return "v_cmp_lt_f32";
        case 0x02: return "v_cmp_eq_f32";
        case 0x03: return "v_cmp_le_f32";
        case 0x04: return "v_cmp_gt_f32";
        case 0x05: return "v_cmp_lg_f32";
        case 0x06: return "v_cmp_ge_f32";
        case 0x07: return "v_cmp_o_f32";
        case 0x08: return "v_cmp_u_f32";
        case 0x09: return "v_cmp_nge_f32";
        case 0x0A: return "v_cmp_nlg_f32";
        case 0x0B: return "v_cmp_ngt_f32";
        case 0x0C: return "v_cmp_nle_f32";
        case 0x0D: return "v_cmp_neq_f32";
        case 0x0E: return "v_cmp_nlt_f32";
        case 0x0F: return "v_cmp_t_f32";
        // Float V_CMPX_*_F32 (GFX10.3: 0x10..0x1F)
        case 0x10: return "v_cmpx_f_f32";
        case 0x11: return "v_cmpx_lt_f32";
        case 0x12: return "v_cmpx_eq_f32";
        case 0x13: return "v_cmpx_le_f32";
        case 0x14: return "v_cmpx_gt_f32";
        case 0x15: return "v_cmpx_lg_f32";
        case 0x16: return "v_cmpx_ge_f32";
        case 0x17: return "v_cmpx_o_f32";
        case 0x18: return "v_cmpx_u_f32";
        case 0x19: return "v_cmpx_nge_f32";
        case 0x1A: return "v_cmpx_nlg_f32";
        case 0x1B: return "v_cmpx_ngt_f32";
        case 0x1C: return "v_cmpx_nle_f32";
        case 0x1D: return "v_cmpx_neq_f32";
        case 0x1E: return "v_cmpx_nlt_f32";
        case 0x1F: return "v_cmpx_t_f32";
        // Signed int V_CMP_*_I32 + V_CMP_CLASS_F32
        case 0x80: return "v_cmp_f_i32";
        case 0x81: return "v_cmp_lt_i32";
        case 0x82: return "v_cmp_eq_i32";
        case 0x83: return "v_cmp_le_i32";
        case 0x84: return "v_cmp_gt_i32";
        case 0x85: return "v_cmp_ne_i32";
        case 0x86: return "v_cmp_ge_i32";
        case 0x87: return "v_cmp_t_i32";
        case 0x88: return "v_cmp_class_f32";
        // Signed int V_CMPX_*_I32 + V_CMPX_CLASS_F32
        case 0x90: return "v_cmpx_f_i32";
        case 0x91: return "v_cmpx_lt_i32";
        case 0x92: return "v_cmpx_eq_i32";
        case 0x93: return "v_cmpx_le_i32";
        case 0x94: return "v_cmpx_gt_i32";
        case 0x95: return "v_cmpx_ne_i32";
        case 0x96: return "v_cmpx_ge_i32";
        case 0x97: return "v_cmpx_t_i32";
        case 0x98: return "v_cmpx_class_f32";
        // Unsigned int V_CMP_*_U32
        case 0xC0: return "v_cmp_f_u32";
        case 0xC1: return "v_cmp_lt_u32";
        case 0xC2: return "v_cmp_eq_u32";
        case 0xC3: return "v_cmp_le_u32";
        case 0xC4: return "v_cmp_gt_u32";
        case 0xC5: return "v_cmp_ne_u32";
        case 0xC6: return "v_cmp_ge_u32";
        case 0xC7: return "v_cmp_t_u32";
        // Unsigned int V_CMPX_*_U32
        case 0xD0: return "v_cmpx_f_u32";
        case 0xD1: return "v_cmpx_lt_u32";
        case 0xD2: return "v_cmpx_eq_u32";
        case 0xD3: return "v_cmpx_le_u32";
        case 0xD4: return "v_cmpx_gt_u32";
        case 0xD5: return "v_cmpx_ne_u32";
        case 0xD6: return "v_cmpx_ge_u32";
        case 0xD7: return "v_cmpx_t_u32";
        default: return nullptr;
    }
}

bool is_vop3b_opcode(uint32_t op) {
    switch (op) {
        case 0x128:  // v_add_co_ci_u32_e64
        case 0x129:  // v_sub_co_ci_u32_e64
        case 0x12A:  // v_subrev_co_ci_u32_e64
        case 0x176:  // v_mad_u64_u32
        case 0x177:  // v_mad_i64_i32
        case 0x30F:  // v_add_co_u32
        case 0x310:  // v_sub_co_u32
        case 0x319:  // v_subrev_co_u32
            return true;
        default:
            return false;
    }
}

const char* vop3_name(uint32_t op) {
    switch (op) {
        case 0x143: return "v_mad_u32_u24";
        case 0x147: return "v_bfe_i32";
        case 0x148: return "v_bfe_u32";
        case 0x14A: return "v_bfi_b32";
        case 0x14B: return "v_fma_f32";
        case 0x151: return "v_med3_f32";
        case 0x169: return "v_mul_lo_u32";
        case 0x16B: return "v_mbcnt_lo_u32_b32";
        case 0x176: return "v_mad_u64_u32";
        case 0x177: return "v_mad_i64_i32";
        case 0x178: return "v_xor3_b32";
        case 0x2FF: return "v_lshlrev_b64";
        case 0x300: return "v_lshrrev_b64";
        case 0x301: return "v_ashrrev_i64";
        case 0x30F: return "v_add_co_u32";
        case 0x310: return "v_sub_co_u32";
        case 0x319: return "v_subrev_co_u32";
        case 0x344: return "v_perm_b32";
        case 0x346: return "v_lshl_add_u32";
        case 0x360: return "v_readlane_b32";
        case 0x361: return "v_writelane_b32";
        case 0x362: return "v_ldexp_f32";
        case 0x36D: return "v_add3_u32";
        case 0x36F: return "v_lshl_or_b32";
        default: break;
    }
    if (op < 0x100) return vopc_name(op);
    if (op >= 0x100 && op < 0x140) return vop2_name(op - 0x100);
    if (op >= 0x180 && op < 0x200) return vop1_name(op - 0x180);
    return nullptr;
}

const char* flat_name(uint32_t op, uint8_t seg) {
    if (seg == 2 || seg == 0) {
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
    return nullptr;
}

}  // namespace

std::string format_rdna2_operand(uint32_t op, bool has_lit, uint32_t lit) {
    if (op < 106) return std::format("s{}", op);
    if (op == 106) return "vcc_lo";
    if (op == 107) return "vcc_hi";
    if (op == 124) return "m0";
    if (op == 125) return "null";
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
            d.is_vop3b = is_vop3b_opcode(d.opcode);
            if (d.is_vop3b) {
                d.sdst = (w0 >> 8) & 0x7Fu;
                d.abs_mask = 0;
            } else {
                d.abs_mask = static_cast<uint8_t>((w0 >> 8) & 0x7u);
                if (d.opcode < 0x100u) {
                    d.sdst = w0 & 0x7Fu;
                }
            }
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
            d.flat_seg = static_cast<uint8_t>((w0 >> 14) & 0x3u);
            d.soffset = static_cast<int32_t>(w0 << 20) >> 20;  // 12-bit signed offset
            d.offset = static_cast<uint32_t>(d.soffset) & 0xFFFu;
            d.dst = (w1 >> 24) & 0xFFu;
            d.src2 = (w1 >> 16) & 0x7Fu;  // saddr (125 = null/off)
            d.src1 = (w1 >> 8) & 0xFFu;   // data
            d.src0 = w1 & 0xFFu;          // vaddr
            const char* n = flat_name(d.opcode, d.flat_seg);
            d.mnemonic = n ? n : std::format("flat_{}", d.opcode);
        } else if ((w0 >> 26) == 0x3Du && i + 1 < count) {
            const uint32_t w1 = words[i + 1];
            d.size = 8;
            d.enc = Encoding::SMEM;
            d.opcode = (w0 >> 18) & 0xFFu;
            d.dst = (w0 >> 6) & 0x7Fu;
            d.src0 = (w0 & 0x3Fu) << 1;
            d.src1 = (w1 >> 25) & 0x7Fu;  // soffset (125 = null)
            d.soffset = static_cast<int32_t>(w1 << 11) >> 11;
            d.offset = w1 & 0x1FFFFFu;
            if (d.opcode == 0) d.mnemonic = "s_load_dword";
            else if (d.opcode == 1) d.mnemonic = "s_load_dwordx2";
            else if (d.opcode == 2) d.mnemonic = "s_load_dwordx4";
            else if (d.opcode == 3) d.mnemonic = "s_load_dwordx8";
            else if (d.opcode == 4) d.mnemonic = "s_load_dwordx16";
            else d.mnemonic = std::format("smem_{}", d.opcode);
        } else if ((w0 >> 26) == 0x38u && i + 1 < count) {
            const uint32_t w1 = words[i + 1];
            d.size = 8;
            d.enc = Encoding::MUBUF;
            d.opcode = (w0 >> 18) & 0x7Fu;
            d.offset = w0 & 0xFFFu;
            d.src0 = w1 & 0xFFu;                  // vaddr
            d.dst = (w1 >> 8) & 0xFFu;            // vdata dst
            d.src1 = d.dst;                       // vdata src (for stores)
            d.src2 = ((w1 >> 16) & 0x1Fu) << 2;   // srsrc (V# SGPR base)
            d.src3 = (w1 >> 24) & 0xFFu;          // soffset
            switch (d.opcode) {
                case 12: d.mnemonic = "buffer_load_dword"; break;
                case 13: d.mnemonic = "buffer_load_dwordx2"; break;
                case 14: d.mnemonic = "buffer_load_dwordx4"; break;
                case 28: d.mnemonic = "buffer_store_dword"; break;
                case 29: d.mnemonic = "buffer_store_dwordx2"; break;
                case 30: d.mnemonic = "buffer_store_dwordx4"; break;
                default: d.mnemonic = std::format("mubuf_{}", d.opcode); break;
            }
        } else if ((w0 >> 26) == 0x3Au && i + 1 < count) {
            const uint32_t w1 = words[i + 1];
            d.size = 8;
            d.enc = Encoding::MTBUF;
            d.mtbuf_format = static_cast<uint8_t>((w0 >> 19) & 0x7Fu);
            d.opcode = (w0 >> 16) & 0x7u;
            d.offset = w0 & 0xFFFu;
            d.src0 = w1 & 0xFFu;
            d.dst = (w1 >> 8) & 0xFFu;
            d.src1 = d.dst;
            d.src2 = ((w1 >> 16) & 0x1Fu) << 2;
            d.src3 = (w1 >> 24) & 0xFFu;
            switch (d.opcode) {
                case 0: d.mnemonic = "tbuffer_load_format_x"; break;
                case 1: d.mnemonic = "tbuffer_load_format_xy"; break;
                case 3: d.mnemonic = "tbuffer_load_format_xyzw"; break;
                case 4: d.mnemonic = "tbuffer_store_format_x"; break;
                case 7: d.mnemonic = "tbuffer_store_format_xyzw"; break;
                default: d.mnemonic = std::format("mtbuf_{}", d.opcode); break;
            }
        } else if ((w0 >> 26) == 0x3Cu && i + 1 < count) {
            const uint32_t w1 = words[i + 1];
            d.size = 8;
            d.enc = Encoding::MIMG;
            d.opcode = (w0 >> 18) & 0x7Fu;
            d.tfe = ((w0 >> 16) & 1u) != 0;
            d.dmask = static_cast<uint8_t>((w0 >> 8) & 0xFu);
            d.img_dim = static_cast<uint8_t>((w0 >> 3) & 0x7u);
            d.src0 = w1 & 0xFFu;                  // vaddr (UV / coords)
            d.dst = (w1 >> 8) & 0xFFu;            // vdata (RGBA dst)
            d.src1 = d.dst;
            d.src2 = ((w1 >> 16) & 0x1Fu) << 2;   // srsrc (T# SGPR base)
            d.src3 = ((w1 >> 21) & 0x1Fu) << 2;   // ssamp (S# SGPR base)
            switch (d.opcode) {
                case 0x00: d.mnemonic = "image_load"; break;
                case 0x08: d.mnemonic = "image_store"; break;
                case 0x0E: d.mnemonic = "image_get_resinfo"; break;
                case 0x20: d.mnemonic = "image_sample"; break;
                case 0x24: d.mnemonic = "image_sample_l"; break;
                case 0x50: d.mnemonic = "image_bvh_intersect_ray"; break;
                case 0x51: d.mnemonic = "image_bvh64_intersect_ray"; break;
                default: d.mnemonic = std::format("mimg_{:#x}", d.opcode); break;
            }
        } else if ((w0 >> 26) == 0x36u && i + 1 < count) {
            const uint32_t w1 = words[i + 1];
            d.size = 8;
            d.enc = Encoding::DS;
            d.opcode = (w0 >> 18) & 0xFFu;
            d.offset = w0 & 0xFFFFu;
            d.dst = (w1 >> 24) & 0xFFu;
            d.src2 = (w1 >> 16) & 0xFFu;  // data1
            d.src1 = (w1 >> 8) & 0xFFu;   // data0
            d.src0 = w1 & 0xFFu;          // addr
            if (d.opcode == 13) d.mnemonic = "ds_write_b32";
            else if (d.opcode == 54) d.mnemonic = "ds_read_b32";
            else d.mnemonic = std::format("ds_{}", d.opcode);
        } else if ((w0 >> 26) == 0x3Eu && i + 1 < count) {
            const uint32_t w1 = words[i + 1];
            d.size = 8;
            d.enc = Encoding::EXP;
            d.opcode = (w0 >> 4) & 0x3Fu;  // target
            d.exp_en = static_cast<uint8_t>(w0 & 0xFu);
            d.exp_done = ((w0 >> 11) & 1u) != 0;
            d.src0 = w1 & 0xFFu;
            d.src1 = (w1 >> 8) & 0xFFu;
            d.src2 = (w1 >> 16) & 0xFFu;
            d.src3 = (w1 >> 24) & 0xFFu;
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
            if (d.mnemonic == "s_sendmsg") {
                const uint16_t raw = static_cast<uint16_t>(d.simm16);
                const uint16_t msg_id = raw & 0xFu;
                if (msg_id == kSendMsgGsAllocReq) return "s_sendmsg sendmsg(GS_ALLOC_REQ)";
                if (msg_id == kSendMsgDispatchMesh) return "s_sendmsg sendmsg(DISPATCH_MESH)";
                if (msg_id == kSendMsgGsDone) return "s_sendmsg sendmsg(GS_DONE)";
                if (msg_id == kSendMsgGs) return "s_sendmsg sendmsg(GS)";
                return std::format("s_sendmsg {:#x}", raw);
            }
            return d.is_branch ? std::format("{} -> {:#x}", d.mnemonic, d.branch_target)
                               : (d.simm16 ? std::format("{} {}", d.mnemonic, d.simm16) : d.mnemonic);
        case Encoding::SOP1:
            return std::format("{} {}, {}", d.mnemonic, format_rdna2_operand(d.dst, false, 0),
                               format_rdna2_operand(d.src0, d.has_literal, d.literal));
        case Encoding::SOP2:
            return std::format("{} {}, {}, {}", d.mnemonic, format_rdna2_operand(d.dst, false, 0),
                               format_rdna2_operand(d.src0, d.has_literal, d.literal),
                               format_rdna2_operand(d.src1, d.has_literal, d.literal));
        case Encoding::SOPK:
            return std::format("{} {}, {}", d.mnemonic, format_rdna2_operand(d.dst, false, 0), d.simm16);
        case Encoding::SOPC:
            return std::format("{} {}, {}", d.mnemonic,
                               format_rdna2_operand(d.src0, d.has_literal, d.literal),
                               format_rdna2_operand(d.src1, d.has_literal, d.literal));
        case Encoding::SMEM:
            return std::format("{} {}, s[{}:{}], {}, off:{:#x}", d.mnemonic,
                               format_rdna2_operand(d.dst, false, 0),
                               d.src0, d.src0 + 1,
                               format_rdna2_operand(d.src1, false, 0),
                               d.soffset);
        case Encoding::VOP1:
            return std::format("{} v{}, {}", d.mnemonic, d.dst, format_rdna2_operand(d.src0, d.has_literal, d.literal));
        case Encoding::VOP2:
            return std::format("{} v{}, {}, v{}", d.mnemonic, d.dst,
                               format_rdna2_operand(d.src0, d.has_literal, d.literal), d.src1 - 256);
        case Encoding::VOPC:
            return std::format("{} vcc, {}, v{}", d.mnemonic,
                               format_rdna2_operand(d.src0, d.has_literal, d.literal), d.src1 - 256);
        case Encoding::VOP3:
            if (d.is_vop3b) {
                return std::format("{} v{}, {}, {}, {}, {}", d.mnemonic, d.dst,
                                   format_rdna2_operand(d.sdst, false, 0),
                                   format_rdna2_operand(d.src0, d.has_literal, d.literal),
                                   format_rdna2_operand(d.src1, d.has_literal, d.literal),
                                   format_rdna2_operand(d.src2, d.has_literal, d.literal));
            }
            if (d.opcode < 0x100u) {
                return std::format("{} {}, {}, {}", d.mnemonic,
                                   format_rdna2_operand(d.sdst, false, 0),
                                   format_rdna2_operand(d.src0, d.has_literal, d.literal),
                                   format_rdna2_operand(d.src1, d.has_literal, d.literal));
            }
            return std::format("{} v{}, {}, {}, {}", d.mnemonic, d.dst,
                               format_rdna2_operand(d.src0, d.has_literal, d.literal),
                               format_rdna2_operand(d.src1, d.has_literal, d.literal),
                               format_rdna2_operand(d.src2, d.has_literal, d.literal));
        case Encoding::FLAT:
            return std::format("{} v{}, v{}, {}, off:{}", d.mnemonic, d.dst, d.src0,
                               format_rdna2_operand(d.src2, false, 0), d.soffset);
        case Encoding::MUBUF:
        case Encoding::MTBUF:
            return std::format("{} v{}, v{}, s[{}:{}], off:{:#x}",
                               d.mnemonic, d.dst, d.src0, d.src2, d.src2 + 3, d.offset);
        case Encoding::MIMG:
            return std::format("{} v{}, v{}, s[{}:{}], s[{}:{}] dmask:{:#x}{}",
                               d.mnemonic, d.dst, d.src0, d.src2, d.src2 + 7, d.src3, d.src3 + 3, d.dmask,
                               d.tfe ? " tfe" : "");
        case Encoding::EXP: {
            std::string tgt = (d.opcode <= 7) ? std::format("mrt{}", d.opcode)
                              : (d.opcode >= 12 && d.opcode <= 15) ? std::format("pos{}", d.opcode - 12)
                              : (d.opcode == kExpTargetPrim0) ? "prim0"
                              : (d.opcode >= 32) ? std::format("param{}", d.opcode - 32)
                              : std::format("tgt{}", d.opcode);
            return std::format("exp {} v{}, v{}, v{}, v{}{}", tgt, d.src0, d.src1, d.src2, d.src3,
                               d.exp_done ? " done" : "");
        }
        default:
            return d.mnemonic;
    }
}

}  // namespace kestrel::gpu
