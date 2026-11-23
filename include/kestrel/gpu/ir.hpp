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
    SAddU32,
    SSubU32,
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
    VAddU32,
    VSubU32,
    VMulLoU32,
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
    VCmp,
    VCndMask,
    // Memory & LDS
    SLoadDword,
    GlobalLoadDword,
    GlobalStoreDword,
    DsReadB32,
    DsWriteB32,
    Barrier,
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
    };
    Kind kind = Kind::None;
    uint32_t reg = 0;
    int32_t imm_i = 0;
    float imm_f = 0.0f;
    uint32_t lit_u32 = 0;
    bool abs = false;
    bool neg = false;
};

struct IrInst {
    uint32_t pc = 0;
    IrOp op = IrOp::Nop;
    std::string subop;  // comparison predicate or raw mnemonic
    IrOperand dst;
    IrOperand src0;
    IrOperand src1;
    IrOperand src2;
    uint32_t offset = 0;
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
};

IrFunction lift_rdna2_to_ir(const std::vector<DecodedInst>& decoded);
std::string dump_ir(const IrFunction& fn);

}  // namespace kestrel::gpu
