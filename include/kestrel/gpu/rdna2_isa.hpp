#pragma once

#include "kestrel/common/types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace kestrel::gpu {

enum class Encoding : uint8_t {
    Unknown,
    SOP1,
    SOP2,
    SOPC,
    SOPK,
    SOPP,
    SMEM,
    VOP1,
    VOP2,
    VOPC,
    VOP3,
    MUBUF,
    DS,
    FLAT,
    EXP,
};

struct DecodedInst {
    uint32_t pc = 0;           // Byte offset in shader stream
    uint32_t size = 4;         // 4, 8, or 12 bytes
    Encoding enc = Encoding::Unknown;
    uint32_t opcode = 0;
    std::string mnemonic;

    uint32_t dst = 0;
    uint32_t src0 = 0;
    uint32_t src1 = 0;
    uint32_t src2 = 0;
    uint32_t src3 = 0;
    int32_t simm16 = 0;
    bool has_literal = false;
    uint32_t literal = 0;
    uint32_t offset = 0;
    uint8_t abs_mask = 0;
    uint8_t neg_mask = 0;
    uint8_t exp_en = 0;
    bool exp_done = false;
    bool clamp = false;
    bool is_branch = false;
    bool is_terminator = false;
    uint32_t branch_target = 0;
};

// Decode a stream of GFX10.3 (RDNA2) little-endian 32-bit instruction dwords.
std::vector<DecodedInst> decode_rdna2(const uint32_t* words, size_t count);
std::string format_rdna2_operand(uint32_t op, bool has_lit, uint32_t lit);
std::string disassemble_inst(const DecodedInst& inst);

// Clean-room GFX10.3 microcode encoders (for self-tests and GPU instruction verification)
namespace encode {
constexpr uint32_t sopp(uint32_t op, int16_t simm16 = 0) {
    return (0x17Fu << 23) | ((op & 0x7Fu) << 16) | static_cast<uint16_t>(simm16);
}
constexpr uint32_t sop1(uint32_t op, uint32_t sdst, uint32_t ssrc0) {
    return (0x17Du << 23) | ((sdst & 0x7Fu) << 16) | ((op & 0xFFu) << 8) | (ssrc0 & 0xFFu);
}
constexpr uint32_t sop2(uint32_t op, uint32_t sdst, uint32_t ssrc0, uint32_t ssrc1) {
    return (0x2u << 30) | ((op & 0x7Fu) << 23) | ((sdst & 0x7Fu) << 16) | ((ssrc1 & 0xFFu) << 8) | (ssrc0 & 0xFFu);
}
constexpr uint32_t sopk(uint32_t op, uint32_t sdst, int16_t simm16) {
    return (0xBu << 28) | ((op & 0x1Fu) << 23) | ((sdst & 0x7Fu) << 16) | static_cast<uint16_t>(simm16);
}
constexpr uint32_t sopc(uint32_t op, uint32_t ssrc0, uint32_t ssrc1) {
    return (0x17Eu << 23) | ((op & 0x7Fu) << 16) | ((ssrc1 & 0xFFu) << 8) | (ssrc0 & 0xFFu);
}
constexpr uint32_t vop1(uint32_t op, uint32_t vdst, uint32_t src0) {
    return (0x3Fu << 25) | ((vdst & 0xFFu) << 17) | ((op & 0xFFu) << 9) | (src0 & 0x1FFu);
}
constexpr uint32_t vop2(uint32_t op, uint32_t vdst, uint32_t src0, uint32_t vsrc1) {
    return ((op & 0x3Fu) << 25) | ((vdst & 0xFFu) << 17) | ((vsrc1 & 0xFFu) << 9) | (src0 & 0x1FFu);
}
constexpr uint32_t vopc(uint32_t op, uint32_t src0, uint32_t vsrc1) {
    return (0x3Eu << 25) | ((op & 0xFFu) << 17) | ((vsrc1 & 0xFFu) << 9) | (src0 & 0x1FFu);
}
inline void vop3(std::vector<uint32_t>& out, uint32_t op, uint32_t vdst, uint32_t src0, uint32_t src1, uint32_t src2,
                 uint8_t abs_mask = 0, uint8_t neg_mask = 0, bool clamp = false) {
    const uint32_t w0 = (0x35u << 26) | ((op & 0x3FFu) << 16) | (clamp ? (1u << 15) : 0u) |
                        ((abs_mask & 0x7u) << 8) | (vdst & 0xFFu);
    const uint32_t w1 = ((neg_mask & 0x7u) << 29) | ((src2 & 0x1FFu) << 18) |
                        ((src1 & 0x1FFu) << 9) | (src0 & 0x1FFu);
    out.push_back(w0);
    out.push_back(w1);
}
inline void global_mem(std::vector<uint32_t>& out, uint32_t op, uint32_t vdst_or_data, uint32_t vaddr, uint32_t saddr, int16_t offset = 0) {
    const uint32_t w0 = (0x37u << 26) | ((op & 0x7Fu) << 18) | (2u << 14) | (static_cast<uint32_t>(offset) & 0x1FFFu);
    const uint32_t w1 = ((vdst_or_data & 0xFFu) << 24) | ((saddr & 0x7Fu) << 16) | ((vdst_or_data & 0xFFu) << 8) | (vaddr & 0xFFu);
    out.push_back(w0);
    out.push_back(w1);
}
inline void smem(std::vector<uint32_t>& out, uint32_t op, uint32_t sdst, uint32_t sbase_pair, uint32_t offset = 0) {
    const uint32_t w0 = (0x3Cu << 26) | ((op & 0xFFu) << 18) | ((sdst & 0x7Fu) << 6) | ((sbase_pair >> 1) & 0x3Fu);
    const uint32_t w1 = offset & 0x1FFFFFu;
    out.push_back(w0);
    out.push_back(w1);
}
inline void ds(std::vector<uint32_t>& out, uint32_t op, uint32_t vdst, uint32_t addr, uint32_t data0 = 0, uint32_t data1 = 0, uint16_t offset = 0) {
    const uint32_t w0 = (0x36u << 26) | ((op & 0xFFu) << 18) | offset;
    const uint32_t w1 = ((vdst & 0xFFu) << 24) | ((data1 & 0xFFu) << 16) | ((data0 & 0xFFu) << 8) | (addr & 0xFFu);
    out.push_back(w0);
    out.push_back(w1);
}
// GFX10.3 EXP target constants: 0..7 = mrt0..7, 12..15 = pos0..3, 32..63 = param0..31
inline void exp(std::vector<uint32_t>& out, uint8_t target, uint8_t en_mask,
                uint32_t vsrc0, uint32_t vsrc1, uint32_t vsrc2, uint32_t vsrc3,
                bool done = false, bool compr = false) {
    const uint32_t w0 = (0x3Eu << 26) | (done ? (1u << 11) : 0u) | (compr ? (1u << 10) : 0u) |
                        ((static_cast<uint32_t>(target) & 0x3Fu) << 4) | (static_cast<uint32_t>(en_mask) & 0xFu);
    const uint32_t w1 = ((vsrc3 & 0xFFu) << 24) | ((vsrc2 & 0xFFu) << 16) |
                        ((vsrc1 & 0xFFu) << 8) | (vsrc0 & 0xFFu);
    out.push_back(w0);
    out.push_back(w1);
}
}  // namespace encode

}  // namespace kestrel::gpu
