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
    MTBUF,
    MIMG,
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
    uint32_t sdst = 0;         // VOP3B / VOP3-VOPC scalar destination (7-bit)
    int32_t simm16 = 0;
    int32_t soffset = 0;       // Signed byte offset (FLAT/GLOBAL/SMEM)
    bool has_literal = false;
    uint32_t literal = 0;
    uint32_t offset = 0;
    uint8_t abs_mask = 0;
    uint8_t neg_mask = 0;
    uint8_t exp_en = 0;
    uint8_t dmask = 0xFu;      // MIMG channel write mask (RGBA)
    uint8_t img_dim = 1;       // MIMG dimension (0=1D, 1=2D, 2=3D, 3=Cube)
    uint8_t mtbuf_format = 0;  // MTBUF data/number format
    uint8_t flat_seg = 0;      // FLAT segment: 0=flat, 1=scratch, 2=global
    bool is_vop3b = false;     // VOP3B uses w0[14:8] as sdst instead of abs modifiers
    bool exp_done = false;
    bool clamp = false;
    bool tfe = false;
    bool is_branch = false;
    bool is_terminator = false;
    uint32_t branch_target = 0;
};

// Decode a stream of GFX10.3 (RDNA2) little-endian 32-bit instruction dwords.
std::vector<DecodedInst> decode_rdna2(const uint32_t* words, size_t count);
std::string format_rdna2_operand(uint32_t op, bool has_lit, uint32_t lit);
std::string disassemble_inst(const DecodedInst& inst);

// Clean-room GFX10.3 s_sendmsg message IDs (simm16[3:0])
constexpr uint16_t kSendMsgGs = 2u;
constexpr uint16_t kSendMsgGsDone = 3u;
constexpr uint16_t kSendMsgGsAllocReq = 9u;     // Mesh/NGG output count allocation (m0[10:0]=verts, m0[22:12]=prims)
constexpr uint16_t kSendMsgDispatchMesh = 10u;  // Amplification shader DispatchMesh payload dispatch (m0 packed x/y/z)

// Clean-room GFX10.3 EXP target constants
constexpr uint8_t kExpTargetMrt0 = 0u;
constexpr uint8_t kExpTargetPos0 = 12u;
constexpr uint8_t kExpTargetPrim0 = 20u;
constexpr uint8_t kExpTargetParam0 = 32u;

// Clean-room GFX10.3 microcode encoders (for self-tests and GPU instruction verification)
namespace encode {
constexpr uint32_t sopp(uint32_t op, int16_t simm16 = 0) {
    return (0x17Fu << 23) | ((op & 0x7Fu) << 16) | static_cast<uint16_t>(simm16);
}
constexpr uint32_t s_sendmsg(uint16_t msg_id) {
    return sopp(16, static_cast<int16_t>(msg_id));
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
// GFX10.3 SMEM encoding prefix: 0x3D (111101)
inline void smem(std::vector<uint32_t>& out, uint32_t op, uint32_t sdst, uint32_t sbase_pair, uint32_t offset = 0) {
    const uint32_t w0 = (0x3Du << 26) | ((op & 0xFFu) << 18) | ((sdst & 0x7Fu) << 6) | ((sbase_pair >> 1) & 0x3Fu);
    const uint32_t w1 = offset & 0x1FFFFFu;
    out.push_back(w0);
    out.push_back(w1);
}
// GFX10.3 MUBUF encoding prefix: 0x38 (111000)
// op: 12=buffer_load_dword, 13=x2, 14=x4, 28=buffer_store_dword, 29=x2, 30=x4
inline void mubuf(std::vector<uint32_t>& out, uint32_t op, uint32_t vdata, uint32_t vaddr,
                  uint32_t srsrc = 0, uint32_t soffset = 128, uint16_t offset = 0) {
    const uint32_t w0 = (0x38u << 26) | ((op & 0x7Fu) << 18) | (offset & 0xFFFu);
    const uint32_t w1 = ((soffset & 0xFFu) << 24) | (((srsrc >> 2) & 0x1Fu) << 16) |
                        ((vdata & 0xFFu) << 8) | (vaddr & 0xFFu);
    out.push_back(w0);
    out.push_back(w1);
}
// GFX10.3 MTBUF encoding prefix: 0x3A (111010)
// op: 0=tbuffer_load_format_x, 1=xy, 3=xyzw, 4=tbuffer_store_format_x, 7=xyzw
inline void mtbuf(std::vector<uint32_t>& out, uint32_t op, uint32_t vdata, uint32_t vaddr,
                  uint32_t srsrc = 0, uint32_t soffset = 128, uint8_t format = 0, uint16_t offset = 0) {
    const uint32_t w0 = (0x3Au << 26) | ((static_cast<uint32_t>(format) & 0x7Fu) << 19) |
                        ((op & 0x7u) << 16) | (offset & 0xFFFu);
    const uint32_t w1 = ((soffset & 0xFFu) << 24) | (((srsrc >> 2) & 0x1Fu) << 16) |
                        ((vdata & 0xFFu) << 8) | (vaddr & 0xFFu);
    out.push_back(w0);
    out.push_back(w1);
}
// GFX10.3 MIMG encoding prefix: 0x3C (111100)
// op: 0x00=image_load, 0x0E=image_get_resinfo, 0x20=image_sample, 0x24=image_sample_l
inline void mimg(std::vector<uint32_t>& out, uint32_t op, uint32_t vdata, uint32_t vaddr,
                 uint32_t srsrc = 0, uint32_t ssamp = 0, uint8_t dmask = 0xFu, uint8_t dim = 1,
                 bool tfe = false) {
    const uint32_t w0 = (0x3Cu << 26) | ((op & 0x7Fu) << 18) |
                        (tfe ? (1u << 16) : 0u) |
                        ((static_cast<uint32_t>(dmask) & 0xFu) << 8) |
                        ((static_cast<uint32_t>(dim) & 0x7u) << 3);
    const uint32_t w1 = (((ssamp >> 2) & 0x1Fu) << 21) | (((srsrc >> 2) & 0x1Fu) << 16) |
                        ((vdata & 0xFFu) << 8) | (vaddr & 0xFFu);
    out.push_back(w0);
    out.push_back(w1);
}
inline void image_bvh_intersect_ray(std::vector<uint32_t>& out, uint32_t vdata, uint32_t vaddr,
                                    uint32_t srsrc = 0, bool is_64 = false) {
    mimg(out, is_64 ? 0x51u : 0x50u, vdata, vaddr, srsrc, 0, 0xFu, 0);
}
inline void ds(std::vector<uint32_t>& out, uint32_t op, uint32_t vdst, uint32_t addr, uint32_t data0 = 0, uint32_t data1 = 0, uint16_t offset = 0) {
    const uint32_t w0 = (0x36u << 26) | ((op & 0xFFu) << 18) | offset;
    const uint32_t w1 = ((vdst & 0xFFu) << 24) | ((data1 & 0xFFu) << 16) | ((data0 & 0xFFu) << 8) | (addr & 0xFFu);
    out.push_back(w0);
    out.push_back(w1);
}
// GFX10.3 EXP target constants: 0..7 = mrt0..7, 12..15 = pos0..3, 20 = prim0, 32..63 = param0..31
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
// Mesh/NGG vertex + primitive count allocation via m0 + s_sendmsg(GS_ALLOC_REQ)
inline void mesh_alloc(std::vector<uint32_t>& out, uint32_t verts, uint32_t prims) {
    const uint32_t packed = ((prims & 0x7FFu) << 12) | (verts & 0x7FFu);
    out.push_back(sop1(3, 124, 255));  // s_mov_b32 m0, literal
    out.push_back(packed);
    out.push_back(s_sendmsg(kSendMsgGsAllocReq));
}
// Amplification shader DispatchMesh(x, y, z, payload) via m0 + s_sendmsg(DISPATCH_MESH)
inline void dispatch_mesh(std::vector<uint32_t>& out, uint32_t group_x, uint32_t group_y = 1, uint32_t group_z = 1) {
    const uint32_t packed = ((group_z & 0x3FFu) << 20) | ((group_y & 0x3FFu) << 10) | (group_x & 0x3FFu);
    out.push_back(sop1(3, 124, 255));  // s_mov_b32 m0, literal
    out.push_back(packed);
    out.push_back(s_sendmsg(kSendMsgDispatchMesh));
}
// Mesh primitive triangle index export (EXP target 20 = prim0, unpacked 3-channel uint3 indices)
inline void exp_prim0(std::vector<uint32_t>& out, uint32_t v_i0, uint32_t v_i1, uint32_t v_i2, bool done = false) {
    exp(out, kExpTargetPrim0, 0x7u, v_i0, v_i1, v_i2, 0, done, false);
}
// Mesh primitive triangle index export (EXP target 20 = prim0, GFX10.3 packed 1-channel [0]=null [9:1]=i0 [18:10]=i1 [27:19]=i2)
inline void exp_prim0_packed(std::vector<uint32_t>& out, uint32_t v_packed, bool done = false) {
    exp(out, kExpTargetPrim0, 0x1u, v_packed, 0, 0, 0, done, false);
}
}  // namespace encode

}  // namespace kestrel::gpu
