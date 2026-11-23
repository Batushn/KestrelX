#pragma once

#include "kestrel/gpu/rdna2_isa.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace kestrel::gpu {

enum class IrOp : uint8_t {
    Nop,
    // Scalar ops
    SMov,
    SMovB64,
    SNotB32,
    SNotB64,
    SCSelectB32,
    SAddU32,
    SSubU32,
    SAddCU32,
    SSubBU32,
    SMulI32,
    SAndB32,
    SOrB32,
    SXorB32,
    SLshlB32,
    SLshrB32,
    SAshrI32,
    SCmp,
    // Vector ALU
    VMov,
    VAddF32,
    VSubF32,
    VSubRevF32,
    VMulF32,
    VFmaF32,
    VMinF32,
    VMaxF32,
    VMinI32,
    VMaxI32,
    VMinU32,
    VMaxU32,
    VAddU32,
    VSubU32,
    VSubRevU32,
    VMulLoU32,
    VMadU32U24,
    VBfeU32,
    VBfiB32,
    VAndB32,
    VOrB32,
    VXorB32,
    VLshlRevB32,
    VLshrRevB32,
    VAshrRevI32,
    VCvtF32I32,
    VCvtF32U32,
    VCvtI32F32,
    VCvtU32F32,
    VRcpF32,
    VRsqF32,
    VSqrtF32,
    VFloorF32,
    VCeilF32,
    VFractF32,
    VMed3F32,
    VBfeI32,
    VCmp,
    VCndMask,
    // Wave / subgroup intrinsics (RDNA2 Wave32/Wave64 -> SM 6.6 Wave*)
    VReadFirstLaneB32,
    VReadLaneB32,
    VWriteLaneB32,
    VMbcntLoU32B32,
    SAndSaveExec,
    // Memory, Buffer, Texture, LDS & Graphics Export
    SLoadDword,
    GlobalLoadDword,
    GlobalLoadDwordx2,
    GlobalLoadDwordx4,
    GlobalStoreDword,
    GlobalStoreDwordx2,
    GlobalStoreDwordx4,
    BufferLoadDword,
    BufferLoadDwordx2,
    BufferLoadDwordx4,
    BufferStoreDword,
    BufferStoreDwordx2,
    BufferStoreDwordx4,
    ImageSample,
    ImageLoad,
    ImageGetResInfo,
    DsReadB32,
    DsWriteB32,
    Barrier,
    Exp,
    // Mesh & Amplification shader pipeline ops
    SetMeshOutputCounts,
    MeshExportVertex,
    MeshExportPrimitive,
    DispatchMeshPayload,
    // Control flow
    Branch,
    CondBranch,
    EndPgm,
    Unknown,
};

struct IrOperand {
    enum class Kind : uint8_t {
        None,
        Sgpr,
        Vgpr,
        InlineInt,
        InlineFloat,
        Literal,
        Scc,
        Vcc,
        Exec,
        M0,
    };
    Kind kind = Kind::None;
    uint32_t reg = 0;
    int32_t imm_i = 0;
    float imm_f = 0.0f;
    uint32_t lit_u32 = 0;
    bool abs = false;
    bool neg = false;
};

struct DescriptorSlot {
    uint32_t sgpr_base = 0;  // RDNA2 T#/V#/S# base SGPR index
    uint32_t hlsl_reg = 0;   // Remapped HLSL register index (tN or sN)
};

struct IrInst {
    uint32_t pc = 0;
    IrOp op = IrOp::Nop;
    std::string subop;  // comparison predicate or raw mnemonic
    IrOperand dst;
    IrOperand src0;
    IrOperand src1;
    IrOperand src2;
    IrOperand src3;
    uint32_t offset = 0;
    uint32_t rsrc_slot = 0;  // Index into tex_bindings or buf_srv_bindings
    uint32_t samp_slot = 0;  // Index into samp_bindings
    uint8_t exp_target = 0;
    uint8_t exp_en = 0;
    uint8_t dmask = 0xFu;
    bool exp_done = false;
    bool clamp = false;
    uint32_t target_block = 0;
    bool branch_on_true = true;
};

struct IrBlock {
    uint32_t id = 0;
    uint32_t start_pc = 0;
    uint32_t end_pc = 0;
    std::vector<IrInst> insts;
    std::vector<uint32_t> successors;
    std::vector<uint32_t> predecessors;
};

struct IrFunction {
    std::vector<IrBlock> blocks;
    uint32_t max_vgpr = 0;
    uint32_t max_sgpr = 0;
    uint32_t inst_count = 0;
    uint32_t unknown_count = 0;
    bool uses_lds = false;
    bool uses_mem = false;
    bool uses_cbuf = false;
    bool uses_buf_srv = false;
    bool uses_tex = false;
    bool uses_wave_ops = false;
    uint32_t mrt_mask = 0;
    uint32_t max_mesh_verts = 0;
    uint32_t max_mesh_prims = 0;
    uint32_t mesh_param_mask = 0;
    std::vector<DescriptorSlot> tex_bindings;
    std::vector<DescriptorSlot> samp_bindings;
    std::vector<DescriptorSlot> buf_srv_bindings;
};

IrFunction lift_rdna2_to_ir(const std::vector<DecodedInst>& decoded);
std::string dump_ir(const IrFunction& fn);

}  // namespace kestrel::gpu
