#include "kestrel/gpu/ir.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

namespace kestrel::gpu {

namespace {

// ---------------------------------------------------------------------------
// Register & operand helpers
// ---------------------------------------------------------------------------

struct RegKey {
    bool is_vgpr = false;
    uint32_t reg = 0;

    bool operator==(const RegKey& o) const {
        return is_vgpr == o.is_vgpr && reg == o.reg;
    }
};

struct RegKeyHash {
    size_t operator()(const RegKey& k) const noexcept {
        return (static_cast<size_t>(k.is_vgpr ? 1u : 0u) << 32) | k.reg;
    }
};

bool is_reg_operand(const IrOperand& op) {
    return op.kind == IrOperand::Kind::Sgpr || op.kind == IrOperand::Kind::Vgpr;
}

RegKey to_reg_key(const IrOperand& op) {
    return RegKey{op.kind == IrOperand::Kind::Vgpr, op.reg};
}

IrOperand make_reg_operand(const RegKey& k) {
    IrOperand op{};
    op.kind = k.is_vgpr ? IrOperand::Kind::Vgpr : IrOperand::Kind::Sgpr;
    op.reg = k.reg;
    return op;
}

IrOperand make_literal_u32(uint32_t val) {
    IrOperand op{};
    op.kind = IrOperand::Kind::Literal;
    op.lit_u32 = val;
    return op;
}

bool try_get_const_u32(const IrOperand& op, uint32_t& out_bits) {
    switch (op.kind) {
        case IrOperand::Kind::InlineInt:
            out_bits = static_cast<uint32_t>(op.imm_i);
            return true;
        case IrOperand::Kind::InlineFloat:
            out_bits = std::bit_cast<uint32_t>(op.imm_f);
            return true;
        case IrOperand::Kind::Literal:
            out_bits = op.lit_u32;
            return true;
        default:
            return false;
    }
}

bool try_get_const_f32(const IrOperand& op, float& out_f) {
    uint32_t bits = 0;
    if (!try_get_const_u32(op, bits)) return false;
    if (op.abs) bits &= 0x7FFFFFFFu;
    if (op.neg) bits ^= 0x80000000u;
    out_f = std::bit_cast<float>(bits);
    return true;
}

bool is_normal_or_zero_f32(float f) {
    const int cls = std::fpclassify(f);
    return cls == FP_ZERO || cls == FP_NORMAL;
}

uint32_t s_load_reg_count(const IrInst& inst) {
    if (inst.subop == "s_load_dwordx4") return 4u;
    if (inst.subop == "s_load_dwordx2") return 2u;
    return 1u;
}

uint32_t buf_reg_count(IrOp op) {
    switch (op) {
        case IrOp::BufferLoadDwordx2:
        case IrOp::BufferStoreDwordx2:
            return 2u;
        case IrOp::BufferLoadDwordx4:
        case IrOp::BufferStoreDwordx4:
            return 4u;
        default:
            return 1u;
    }
}

uint32_t global_reg_count(IrOp op) {
    switch (op) {
        case IrOp::GlobalLoadDwordx2:
        case IrOp::GlobalStoreDwordx2:
            return 2u;
        case IrOp::GlobalLoadDwordx4:
        case IrOp::GlobalStoreDwordx4:
            return 4u;
        default:
            return 1u;
    }
}

uint32_t image_out_reg_count(uint8_t dmask) {
    uint32_t n = 0;
    for (int c = 0; c < 4; ++c) {
        if (dmask & (1u << c)) ++n;
    }
    return n == 0 ? 1u : n;
}

bool has_side_effects(const IrInst& inst) {
    if (inst.dst.kind == IrOperand::Kind::Exec) return true;
    switch (inst.op) {
        case IrOp::Nop:
        case IrOp::SMov:
        case IrOp::SAddU32:
        case IrOp::SSubU32:
        case IrOp::SMulI32:
        case IrOp::SAndB32:
        case IrOp::SOrB32:
        case IrOp::SXorB32:
        case IrOp::SLshlB32:
        case IrOp::SLshrB32:
        case IrOp::SAshrI32:
        case IrOp::SCmp:
        case IrOp::VMov:
        case IrOp::VAddF32:
        case IrOp::VSubF32:
        case IrOp::VSubRevF32:
        case IrOp::VMulF32:
        case IrOp::VFmaF32:
        case IrOp::VMinF32:
        case IrOp::VMaxF32:
        case IrOp::VAddU32:
        case IrOp::VSubU32:
        case IrOp::VMulLoU32:
        case IrOp::VAndB32:
        case IrOp::VOrB32:
        case IrOp::VXorB32:
        case IrOp::VLshlRevB32:
        case IrOp::VLshrRevB32:
        case IrOp::VAshrRevI32:
        case IrOp::VCvtF32I32:
        case IrOp::VCvtF32U32:
        case IrOp::VCvtI32F32:
        case IrOp::VCvtU32F32:
        case IrOp::VRcpF32:
        case IrOp::VRsqF32:
        case IrOp::VSqrtF32:
        case IrOp::VFloorF32:
        case IrOp::VCeilF32:
        case IrOp::VFractF32:
        case IrOp::VMed3F32:
        case IrOp::VBfeI32:
        case IrOp::VCmp:
        case IrOp::VCndMask:
            return false;
        default:
            return true;
    }
}

bool can_substitute_src(IrOp op, int src_idx) {
    switch (op) {
        case IrOp::Unknown:
            return false;
        case IrOp::BufferStoreDword:
        case IrOp::BufferStoreDwordx2:
        case IrOp::BufferStoreDwordx4:
            return src_idx != 1;  // src1 uses .reg directly as v[src1.reg ..]
        case IrOp::ImageSample:
        case IrOp::ImageLoad:
        case IrOp::ImageGetResInfo:
        case IrOp::ImageBvhIntersectRay:
            return src_idx != 0;  // src0 uses .reg directly as v[src0.reg ..]
        default:
            return true;
    }
}

// ---------------------------------------------------------------------------
// Pass 1: Unreachable basic-block pruning
// ---------------------------------------------------------------------------

uint32_t prune_unreachable_blocks(IrFunction& fn) {
    const uint32_t n = static_cast<uint32_t>(fn.blocks.size());
    if (n <= 1) return 0;

    std::vector<bool> reachable(n, false);
    std::queue<uint32_t> q;
    reachable[0] = true;
    q.push(0);

    while (!q.empty()) {
        const uint32_t cur = q.front();
        q.pop();
        for (uint32_t succ : fn.blocks[cur].successors) {
            if (succ < n && !reachable[succ]) {
                reachable[succ] = true;
                q.push(succ);
            }
        }
    }

    uint32_t reach_count = 0;
    for (bool r : reachable) {
        if (r) ++reach_count;
    }
    if (reach_count == n) return 0;

    // Preserve emit_hlsl_from_ir's invariant: if only block 0 remains and it
    // still contains a Branch/CondBranch (e.g. a 1-block self-loop), keep CFG
    // multi-block emission intact by skipping pruning in that degenerate case.
    if (reach_count == 1) {
        for (const auto& inst : fn.blocks[0].insts) {
            if (inst.op == IrOp::Branch || inst.op == IrOp::CondBranch) {
                return 0;
            }
        }
    }

    std::vector<uint32_t> old_to_new(n, UINT32_MAX);
    std::vector<IrBlock> new_blocks;
    new_blocks.reserve(reach_count);

    for (uint32_t b = 0; b < n; ++b) {
        if (!reachable[b]) continue;
        old_to_new[b] = static_cast<uint32_t>(new_blocks.size());
        new_blocks.push_back(std::move(fn.blocks[b]));
    }

    for (uint32_t nb = 0; nb < new_blocks.size(); ++nb) {
        auto& blk = new_blocks[nb];
        blk.id = nb;
        blk.predecessors.clear();
        std::vector<uint32_t> remapped_succs;
        remapped_succs.reserve(blk.successors.size());
        for (uint32_t old_s : blk.successors) {
            if (old_s < n && old_to_new[old_s] != UINT32_MAX) {
                remapped_succs.push_back(old_to_new[old_s]);
            }
        }
        blk.successors = std::move(remapped_succs);

        for (auto& inst : blk.insts) {
            if (inst.op == IrOp::Branch || inst.op == IrOp::CondBranch) {
                if (inst.target_block < n && old_to_new[inst.target_block] != UINT32_MAX) {
                    inst.target_block = old_to_new[inst.target_block];
                }
            }
        }
    }

    for (uint32_t nb = 0; nb < new_blocks.size(); ++nb) {
        for (uint32_t succ : new_blocks[nb].successors) {
            if (succ < new_blocks.size()) {
                new_blocks[succ].predecessors.push_back(nb);
            }
        }
    }

    fn.blocks = std::move(new_blocks);
    return n - reach_count;
}

// ---------------------------------------------------------------------------
// Pass 2 & 3: Intra-block copy propagation + constant tracking & folding
// ---------------------------------------------------------------------------

struct TrackedValue {
    enum class Kind : uint8_t {
        Unknown,
        ConstU32,
        CopyOf,
    };
    Kind kind = Kind::Unknown;
    uint32_t const_u32 = 0;
    RegKey copy_src{};
};

bool try_fold_inst(IrInst& inst, uint32_t& folded_bits) {
    if (inst.dst.kind != IrOperand::Kind::Sgpr && inst.dst.kind != IrOperand::Kind::Vgpr) {
        return false;
    }

    // Integer / bitwise ops require unmodified constant bits (no abs/neg).
    auto get_int2 = [&](uint32_t& a, uint32_t& b) -> bool {
        if (inst.src0.abs || inst.src0.neg || inst.src1.abs || inst.src1.neg) return false;
        return try_get_const_u32(inst.src0, a) && try_get_const_u32(inst.src1, b);
    };

    uint32_t a = 0, b = 0, c = 0;
    float fa = 0.0f, fb = 0.0f, fc = 0.0f;

    switch (inst.op) {
        case IrOp::SAddU32:
        case IrOp::VAddU32:
            if (!get_int2(a, b)) return false;
            folded_bits = a + b;
            return true;
        case IrOp::SSubU32:
        case IrOp::VSubU32:
            if (!get_int2(a, b)) return false;
            folded_bits = a - b;
            return true;
        case IrOp::SMulI32:
        case IrOp::VMulLoU32:
            if (!get_int2(a, b)) return false;
            folded_bits = a * b;
            return true;
        case IrOp::SAndB32:
        case IrOp::VAndB32:
            if (!get_int2(a, b)) return false;
            folded_bits = a & b;
            return true;
        case IrOp::SOrB32:
        case IrOp::VOrB32:
            if (!get_int2(a, b)) return false;
            folded_bits = a | b;
            return true;
        case IrOp::SXorB32:
        case IrOp::VXorB32:
            if (!get_int2(a, b)) return false;
            folded_bits = a ^ b;
            return true;
        case IrOp::SLshlB32:
            if (!get_int2(a, b)) return false;
            folded_bits = a << (b & 31u);
            return true;
        case IrOp::SLshrB32:
            if (!get_int2(a, b)) return false;
            folded_bits = a >> (b & 31u);
            return true;
        case IrOp::SAshrI32:
            if (!get_int2(a, b)) return false;
            folded_bits = static_cast<uint32_t>(static_cast<int32_t>(a) >> (b & 31u));
            return true;
        case IrOp::VLshlRevB32:
            if (!get_int2(a, b)) return false;
            folded_bits = b << (a & 31u);
            return true;
        case IrOp::VLshrRevB32:
            if (!get_int2(a, b)) return false;
            folded_bits = b >> (a & 31u);
            return true;
        case IrOp::VAshrRevI32:
            if (!get_int2(a, b)) return false;
            folded_bits = static_cast<uint32_t>(static_cast<int32_t>(b) >> (a & 31u));
            return true;
        case IrOp::VBfeI32: {
            if (inst.src0.abs || inst.src0.neg || inst.src1.abs || inst.src1.neg ||
                inst.src2.abs || inst.src2.neg) {
                return false;
            }
            if (!try_get_const_u32(inst.src0, a) ||
                !try_get_const_u32(inst.src1, b) ||
                !try_get_const_u32(inst.src2, c)) {
                return false;
            }
            const int32_t val = static_cast<int32_t>(a);
            const uint32_t off = b & 31u;
            const uint32_t w = c & 63u;
            if (w == 0u) {
                folded_bits = 0u;
            } else if (off + w >= 32u) {
                folded_bits = static_cast<uint32_t>(val >> off);
            } else {
                const uint32_t sh = 32u - (off + w);
                const uint32_t shifted = static_cast<uint32_t>(val) << sh;
                folded_bits = static_cast<uint32_t>(static_cast<int32_t>(shifted) >> (sh + off));
            }
            return true;
        }
        case IrOp::VCvtF32I32: {
            if (inst.src0.abs || inst.src0.neg || !try_get_const_u32(inst.src0, a)) return false;
            const float f = static_cast<float>(static_cast<int32_t>(a));
            folded_bits = std::bit_cast<uint32_t>(f);
            return true;
        }
        case IrOp::VCvtF32U32: {
            if (inst.src0.abs || inst.src0.neg || !try_get_const_u32(inst.src0, a)) return false;
            const float f = static_cast<float>(a);
            folded_bits = std::bit_cast<uint32_t>(f);
            return true;
        }
        case IrOp::VCvtI32F32: {
            if (!try_get_const_f32(inst.src0, fa)) return false;
            if (!std::isfinite(fa) || fa < -2147483648.0f || fa >= 2147483648.0f) return false;
            folded_bits = static_cast<uint32_t>(static_cast<int32_t>(fa));
            return true;
        }
        case IrOp::VCvtU32F32: {
            if (!try_get_const_f32(inst.src0, fa)) return false;
            if (!std::isfinite(fa) || fa < 0.0f || fa >= 4294967296.0f) return false;
            folded_bits = static_cast<uint32_t>(fa);
            return true;
        }
        case IrOp::VAddF32:
        case IrOp::VSubF32:
        case IrOp::VSubRevF32:
        case IrOp::VMulF32:
        case IrOp::VMinF32:
        case IrOp::VMaxF32: {
            if (!try_get_const_f32(inst.src0, fa) || !try_get_const_f32(inst.src1, fb)) return false;
            if (!is_normal_or_zero_f32(fa) || !is_normal_or_zero_f32(fb)) return false;
            float res = 0.0f;
            if (inst.op == IrOp::VAddF32) res = fa + fb;
            else if (inst.op == IrOp::VSubF32) res = fa - fb;
            else if (inst.op == IrOp::VSubRevF32) res = fb - fa;
            else if (inst.op == IrOp::VMulF32) res = fa * fb;
            else if (inst.op == IrOp::VMinF32) res = std::fminf(fa, fb);
            else res = std::fmaxf(fa, fb);
            if (!is_normal_or_zero_f32(res)) return false;
            if (inst.clamp) res = std::clamp(res, 0.0f, 1.0f);
            folded_bits = std::bit_cast<uint32_t>(res);
            return true;
        }
        case IrOp::VFloorF32:
        case IrOp::VCeilF32: {
            if (!try_get_const_f32(inst.src0, fa) || !is_normal_or_zero_f32(fa)) return false;
            const float res = (inst.op == IrOp::VFloorF32) ? std::floor(fa) : std::ceil(fa);
            if (!is_normal_or_zero_f32(res)) return false;
            folded_bits = std::bit_cast<uint32_t>(res);
            return true;
        }
        case IrOp::VFractF32: {
            if (!try_get_const_f32(inst.src0, fa) || !is_normal_or_zero_f32(fa)) return false;
            const float fr = fa - std::floor(fa);
            if (fr >= 1.0f) {
                folded_bits = 0x3F7FFFFFu;  // RDNA2 hardware clamp parity
            } else {
                if (!is_normal_or_zero_f32(fr)) return false;
                folded_bits = std::bit_cast<uint32_t>(fr);
            }
            return true;
        }
        case IrOp::VMed3F32: {
            if (!try_get_const_f32(inst.src0, fa) ||
                !try_get_const_f32(inst.src1, fb) ||
                !try_get_const_f32(inst.src2, fc)) {
                return false;
            }
            // Match RDNA2 / HLSL parity: if any input is NaN, min3 returns the
            // non-NaN minimum; otherwise median of three.
            float res = 0.0f;
            if (std::isnan(fa) || std::isnan(fb) || std::isnan(fc)) {
                res = std::fminf(std::fminf(fa, fb), fc);
                if (std::isnan(res)) return false;
            } else {
                if (!is_normal_or_zero_f32(fa) || !is_normal_or_zero_f32(fb) || !is_normal_or_zero_f32(fc)) {
                    return false;
                }
                res = std::fmaxf(std::fminf(fa, fb), std::fminf(std::fmaxf(fa, fb), fc));
            }
            if (!is_normal_or_zero_f32(res)) return false;
            folded_bits = std::bit_cast<uint32_t>(res);
            return true;
        }
        case IrOp::VCndMask: {
            if (inst.src0.abs || inst.src0.neg || inst.src1.abs || inst.src1.neg) return false;
            if (try_get_const_u32(inst.src0, a) && try_get_const_u32(inst.src1, b) && a == b) {
                folded_bits = a;
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}

void propagate_and_fold_block(IrBlock& blk, IrOptStats& stats) {
    std::unordered_map<RegKey, TrackedValue, RegKeyHash> env;

    auto invalidate_reg = [&](const RegKey& key) {
        env.erase(key);
        for (auto it = env.begin(); it != env.end();) {
            if (it->second.kind == TrackedValue::Kind::CopyOf && it->second.copy_src == key) {
                it = env.erase(it);
            } else {
                ++it;
            }
        }
    };

    auto invalidate_span = [&](bool is_vgpr, uint32_t base, uint32_t count) {
        for (uint32_t k = 0; k < count; ++k) {
            invalidate_reg(RegKey{is_vgpr, base + k});
        }
    };

    auto substitute_operand = [&](IrOperand& op) {
        if (!is_reg_operand(op)) return;
        const auto it = env.find(to_reg_key(op));
        if (it == env.end()) return;
        const bool orig_abs = op.abs;
        const bool orig_neg = op.neg;
        if (it->second.kind == TrackedValue::Kind::ConstU32) {
            op = make_literal_u32(it->second.const_u32);
            op.abs = orig_abs;
            op.neg = orig_neg;
            ++stats.copies_propagated;
        } else if (it->second.kind == TrackedValue::Kind::CopyOf) {
            op = make_reg_operand(it->second.copy_src);
            op.abs = orig_abs;
            op.neg = orig_neg;
            ++stats.copies_propagated;
        }
    };

    for (auto& inst : blk.insts) {
        // 1. Substitute known copies/constants into source operands
        if (can_substitute_src(inst.op, 0)) substitute_operand(inst.src0);
        if (can_substitute_src(inst.op, 1)) substitute_operand(inst.src1);
        if (can_substitute_src(inst.op, 2)) substitute_operand(inst.src2);
        if (can_substitute_src(inst.op, 3)) substitute_operand(inst.src3);

        // 2. Constant folding
        uint32_t folded_bits = 0;
        if (try_fold_inst(inst, folded_bits)) {
            const IrOperand dst = inst.dst;
            const uint32_t pc = inst.pc;
            inst = IrInst{};
            inst.pc = pc;
            inst.op = (dst.kind == IrOperand::Kind::Sgpr) ? IrOp::SMov : IrOp::VMov;
            inst.dst = dst;
            inst.src0 = make_literal_u32(folded_bits);
            ++stats.constants_folded;
        }

        // 3. Update tracked state for writes / clobbers
        if (inst.op == IrOp::Unknown || inst.dst.kind == IrOperand::Kind::Exec) {
            env.clear();
            continue;
        }

        if (inst.op == IrOp::SAndSaveExec) {
            invalidate_span(false, inst.dst.reg, (inst.subop == "s_and_saveexec_b64") ? 2u : 1u);
            // Exec mask changes: conservatively clear vector tracking.
            for (auto it = env.begin(); it != env.end();) {
                if (it->first.is_vgpr ||
                    (it->second.kind == TrackedValue::Kind::CopyOf && it->second.copy_src.is_vgpr)) {
                    it = env.erase(it);
                } else {
                    ++it;
                }
            }
            continue;
        }

        if (inst.op == IrOp::SLoadDword) {
            invalidate_span(false, inst.dst.reg, s_load_reg_count(inst));
            continue;
        }
        if (inst.op == IrOp::BufferLoadDword ||
            inst.op == IrOp::BufferLoadDwordx2 ||
            inst.op == IrOp::BufferLoadDwordx4) {
            invalidate_span(true, inst.dst.reg, buf_reg_count(inst.op));
            continue;
        }
        if (inst.op == IrOp::GlobalLoadDword ||
            inst.op == IrOp::GlobalLoadDwordx2 ||
            inst.op == IrOp::GlobalLoadDwordx4) {
            invalidate_span(true, inst.dst.reg, global_reg_count(inst.op));
            continue;
        }
        if (inst.op == IrOp::SMovB64 || inst.op == IrOp::SNotB64) {
            invalidate_span(false, inst.dst.reg, 2u);
            continue;
        }
        if (inst.op == IrOp::ImageSample ||
            inst.op == IrOp::ImageLoad ||
            inst.op == IrOp::ImageGetResInfo) {
            const uint32_t span = image_out_reg_count(inst.dmask) + (inst.tfe ? 1u : 0u);
            invalidate_span(true, inst.dst.reg, span);
            continue;
        }
        if (inst.op == IrOp::ImageBvhIntersectRay) {
            invalidate_span(true, inst.dst.reg, 4u);
            continue;
        }

        if ((inst.op == IrOp::SMov || inst.op == IrOp::VMov) && is_reg_operand(inst.dst)) {
            const RegKey dst_key = to_reg_key(inst.dst);
            uint32_t cval = 0;
            if (!inst.src0.abs && !inst.src0.neg && try_get_const_u32(inst.src0, cval)) {
                invalidate_reg(dst_key);
                env[dst_key] = TrackedValue{TrackedValue::Kind::ConstU32, cval, {}};
            } else if (!inst.src0.abs && !inst.src0.neg && is_reg_operand(inst.src0)) {
                const RegKey src_key = to_reg_key(inst.src0);
                if (dst_key == src_key) {
                    // Self-move: no change to tracked state
                } else {
                    invalidate_reg(dst_key);
                    env[dst_key] = TrackedValue{TrackedValue::Kind::CopyOf, 0, src_key};
                }
            } else {
                invalidate_reg(dst_key);
            }
            continue;
        }

        if (is_reg_operand(inst.dst)) {
            invalidate_reg(to_reg_key(inst.dst));
        }
    }
}

// ---------------------------------------------------------------------------
// Pass 4: Backward-liveness DCE & unused scc/vcc flag pruning
// ---------------------------------------------------------------------------

struct LiveSet {
    std::vector<uint64_t> sgpr;
    std::vector<uint64_t> vgpr;
    bool scc = false;
    bool vcc = false;

    void init(uint32_t max_s, uint32_t max_v) {
        sgpr.assign((max_s + 63u) / 64u, 0ull);
        vgpr.assign((max_v + 63u) / 64u, 0ull);
        scc = false;
        vcc = false;
    }

    void set_reg(bool is_vgpr, uint32_t reg, bool val) {
        auto& vec = is_vgpr ? vgpr : sgpr;
        const size_t word = reg / 64u;
        if (word >= vec.size()) return;
        const uint64_t bit = 1ull << (reg % 64u);
        if (val) vec[word] |= bit;
        else vec[word] &= ~bit;
    }

    bool get_reg(bool is_vgpr, uint32_t reg) const {
        const auto& vec = is_vgpr ? vgpr : sgpr;
        const size_t word = reg / 64u;
        if (word >= vec.size()) return false;
        return (vec[word] & (1ull << (reg % 64u))) != 0ull;
    }

    void add_operand(const IrOperand& op) {
        switch (op.kind) {
            case IrOperand::Kind::Sgpr:
                set_reg(false, op.reg, true);
                break;
            case IrOperand::Kind::Vgpr:
                set_reg(true, op.reg, true);
                break;
            case IrOperand::Kind::Scc:
                scc = true;
                break;
            case IrOperand::Kind::Vcc:
                vcc = true;
                break;
            default:
                break;
        }
    }

    bool union_with(const LiveSet& other) {
        bool changed = false;
        if (other.scc && !scc) { scc = true; changed = true; }
        if (other.vcc && !vcc) { vcc = true; changed = true; }
        for (size_t i = 0; i < sgpr.size(); ++i) {
            const uint64_t before = sgpr[i];
            sgpr[i] |= other.sgpr[i];
            if (sgpr[i] != before) changed = true;
        }
        for (size_t i = 0; i < vgpr.size(); ++i) {
            const uint64_t before = vgpr[i];
            vgpr[i] |= other.vgpr[i];
            if (vgpr[i] != before) changed = true;
        }
        return changed;
    }

    bool operator==(const LiveSet& o) const {
        return scc == o.scc && vcc == o.vcc && sgpr == o.sgpr && vgpr == o.vgpr;
    }
};

uint32_t compute_max_reg(const IrFunction& fn, bool is_vgpr) {
    uint32_t m = is_vgpr ? fn.max_vgpr : fn.max_sgpr;
    auto bump = [&](const IrOperand& op, uint32_t span = 1u) {
        if ((is_vgpr && op.kind == IrOperand::Kind::Vgpr) ||
            (!is_vgpr && op.kind == IrOperand::Kind::Sgpr)) {
            if (op.reg + span > m) m = op.reg + span;
        }
    };
    for (const auto& blk : fn.blocks) {
        for (const auto& inst : blk.insts) {
            bump(inst.dst, 4u);
            bump(inst.src0, 2u);
            bump(inst.src1, 4u);
            bump(inst.src2);
            bump(inst.src3);
        }
    }
    return m;
}

bool is_inst_dead(const IrInst& inst, const LiveSet& live) {
    if (has_side_effects(inst)) return false;
    if (inst.op == IrOp::Nop) return true;
    if (inst.op == IrOp::SCmp) return !live.scc;
    if (inst.op == IrOp::VCmp) return !live.vcc;
    if (inst.dst.kind == IrOperand::Kind::Sgpr) return !live.get_reg(false, inst.dst.reg);
    if (inst.dst.kind == IrOperand::Kind::Vgpr) return !live.get_reg(true, inst.dst.reg);
    if (inst.dst.kind == IrOperand::Kind::Scc) return !live.scc;
    if (inst.dst.kind == IrOperand::Kind::Vcc) return !live.vcc;
    return false;
}

void transfer_inst_backward(const IrInst& inst, LiveSet& live) {
    // 1. Kill defined registers/flags (except partial lane writes like VWriteLaneB32)
    switch (inst.op) {
        case IrOp::SCmp:
            live.scc = false;
            break;
        case IrOp::VCmp:
            live.vcc = false;
            break;
        case IrOp::SAndSaveExec:
            live.scc = false;
            live.set_reg(false, inst.dst.reg, false);
            if (inst.subop == "s_and_saveexec_b64") {
                live.set_reg(false, inst.dst.reg + 1u, false);
            }
            break;
        case IrOp::SLoadDword:
            for (uint32_t k = 0; k < s_load_reg_count(inst); ++k) {
                live.set_reg(false, inst.dst.reg + k, false);
            }
            break;
        case IrOp::BufferLoadDword:
        case IrOp::BufferLoadDwordx2:
        case IrOp::BufferLoadDwordx4:
            for (uint32_t k = 0; k < buf_reg_count(inst.op); ++k) {
                live.set_reg(true, inst.dst.reg + k, false);
            }
            break;
        case IrOp::GlobalLoadDword:
        case IrOp::GlobalLoadDwordx2:
        case IrOp::GlobalLoadDwordx4:
            for (uint32_t k = 0; k < global_reg_count(inst.op); ++k) {
                live.set_reg(true, inst.dst.reg + k, false);
            }
            break;
        case IrOp::SMovB64:
        case IrOp::SNotB64:
            live.set_reg(false, inst.dst.reg, false);
            live.set_reg(false, inst.dst.reg + 1u, false);
            break;
        case IrOp::ImageSample:
        case IrOp::ImageLoad:
        case IrOp::ImageGetResInfo:
            for (uint32_t k = 0; k < image_out_reg_count(inst.dmask) + (inst.tfe ? 1u : 0u); ++k) {
                live.set_reg(true, inst.dst.reg + k, false);
            }
            break;
        case IrOp::ImageBvhIntersectRay:
            for (uint32_t k = 0; k < 4; ++k) {
                live.set_reg(true, inst.dst.reg + k, false);
            }
            break;
        case IrOp::VWriteLaneB32:
            // Partial per-lane write: dst remains live if live after.
            break;
        default:
            if (inst.dst.kind == IrOperand::Kind::Sgpr) live.set_reg(false, inst.dst.reg, false);
            else if (inst.dst.kind == IrOperand::Kind::Vgpr) live.set_reg(true, inst.dst.reg, false);
            else if (inst.dst.kind == IrOperand::Kind::Scc) live.scc = false;
            else if (inst.dst.kind == IrOperand::Kind::Vcc) live.vcc = false;
            break;
    }

    // 2. Add used registers/flags
    live.add_operand(inst.src0);
    live.add_operand(inst.src1);
    live.add_operand(inst.src2);
    live.add_operand(inst.src3);

    if (inst.op == IrOp::VCndMask) {
        live.vcc = true;
    } else if (inst.op == IrOp::SCSelectB32 || inst.op == IrOp::SAddCU32 || inst.op == IrOp::SSubBU32) {
        live.scc = true;
    } else if (inst.op == IrOp::CondBranch) {
        if (inst.subop.find("vcc") != std::string::npos) live.vcc = true;
        else live.scc = true;
    } else if (inst.op == IrOp::BufferStoreDwordx2 || inst.op == IrOp::BufferStoreDwordx4) {
        const uint32_t cnt = buf_reg_count(inst.op);
        for (uint32_t k = 0; k < cnt; ++k) {
            live.set_reg(true, inst.src1.reg + k, true);
        }
    } else if (inst.op == IrOp::GlobalStoreDwordx2 || inst.op == IrOp::GlobalStoreDwordx4) {
        const uint32_t cnt = global_reg_count(inst.op);
        for (uint32_t k = 0; k < cnt; ++k) {
            live.set_reg(true, inst.src1.reg + k, true);
        }
    } else if (inst.op == IrOp::SMovB64 || inst.op == IrOp::SNotB64) {
        if (inst.src0.kind == IrOperand::Kind::Sgpr) {
            live.set_reg(false, inst.src0.reg + 1u, true);
        }
    } else if (inst.op == IrOp::ImageSample || inst.op == IrOp::ImageLoad) {
        live.set_reg(true, inst.src0.reg, true);
        live.set_reg(true, inst.src0.reg + 1u, true);
    } else if (inst.op == IrOp::ImageBvhIntersectRay) {
        for (uint32_t k = 0; k < 8; ++k) {
            live.set_reg(true, inst.src0.reg + k, true);
        }
    } else if (inst.op == IrOp::VWriteLaneB32 && inst.dst.kind == IrOperand::Kind::Vgpr) {
        live.set_reg(true, inst.dst.reg, true);
    }
}

bool run_dce_pass(IrFunction& fn, IrOptStats& stats) {
    const uint32_t n = static_cast<uint32_t>(fn.blocks.size());
    if (n == 0) return false;

    const uint32_t max_s = compute_max_reg(fn, false);
    const uint32_t max_v = compute_max_reg(fn, true);

    std::vector<LiveSet> live_in(n);
    std::vector<LiveSet> live_out(n);
    for (uint32_t b = 0; b < n; ++b) {
        live_in[b].init(max_s, max_v);
        live_out[b].init(max_s, max_v);
    }

    bool changed_dataflow = true;
    while (changed_dataflow) {
        changed_dataflow = false;
        for (int b = static_cast<int>(n) - 1; b >= 0; --b) {
            LiveSet out;
            out.init(max_s, max_v);
            for (uint32_t succ : fn.blocks[b].successors) {
                if (succ < n) out.union_with(live_in[succ]);
            }
            live_out[b] = out;

            LiveSet cur = std::move(out);
            const auto& insts = fn.blocks[b].insts;
            for (auto it = insts.rbegin(); it != insts.rend(); ++it) {
                if (is_inst_dead(*it, cur)) continue;
                transfer_inst_backward(*it, cur);
            }
            if (!(cur == live_in[b])) {
                live_in[b] = std::move(cur);
                changed_dataflow = true;
            }
        }
    }

    bool removed_any = false;
    for (uint32_t b = 0; b < n; ++b) {
        auto& insts = fn.blocks[b].insts;
        if (insts.empty()) continue;

        LiveSet cur = live_out[b];
        std::vector<bool> keep(insts.size(), true);
        for (size_t idx = insts.size(); idx-- > 0;) {
            const auto& inst = insts[idx];
            if (is_inst_dead(inst, cur)) {
                keep[idx] = false;
                if (inst.op == IrOp::SCmp || inst.op == IrOp::VCmp) {
                    ++stats.dead_flags_removed;
                    removed_any = true;
                } else if (inst.op != IrOp::Nop) {
                    ++stats.dead_insts_removed;
                    removed_any = true;
                }
            } else {
                transfer_inst_backward(inst, cur);
            }
        }

        std::vector<IrInst> filtered;
        filtered.reserve(insts.size());
        for (size_t idx = 0; idx < insts.size(); ++idx) {
            if (keep[idx]) filtered.push_back(std::move(insts[idx]));
        }
        insts = std::move(filtered);
    }

    return removed_any;
}

}  // namespace

IrOptStats optimize_ir(IrFunction& fn) {
    IrOptStats stats{};
    if (fn.blocks.empty()) return stats;

    stats.unreachable_blocks_removed += prune_unreachable_blocks(fn);

    constexpr int kMaxRounds = 4;
    for (int round = 0; round < kMaxRounds; ++round) {
        const uint32_t prev_props = stats.copies_propagated;
        const uint32_t prev_folds = stats.constants_folded;

        for (auto& blk : fn.blocks) {
            propagate_and_fold_block(blk, stats);
        }
        const bool dce_removed = run_dce_pass(fn, stats);

        if (!dce_removed &&
            stats.copies_propagated == prev_props &&
            stats.constants_folded == prev_folds) {
            break;
        }
    }

    uint32_t total_insts = 0;
    for (const auto& blk : fn.blocks) {
        total_insts += static_cast<uint32_t>(blk.insts.size());
    }
    fn.inst_count = total_insts;
    return stats;
}

}  // namespace kestrel::gpu
