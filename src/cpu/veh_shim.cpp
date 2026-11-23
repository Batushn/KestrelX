#include "kestrel/cpu/veh_shim.hpp"

#include <Windows.h>
#include <cstring>

namespace kestrel::cpu {

namespace {

std::atomic<uint64_t> g_clzero{0};
std::atomic<uint64_t> g_monitorx{0};
std::atomic<uint64_t> g_mwaitx{0};
std::atomic<uint64_t> g_extrq{0};
std::atomic<uint64_t> g_insertq{0};
std::atomic<uint64_t> g_unhandled{0};

M128A* get_xmm(CONTEXT* ctx, uint8_t idx) {
    if (!ctx) return nullptr;
    switch (idx & 15) {
        case 0:  return &ctx->Xmm0;
        case 1:  return &ctx->Xmm1;
        case 2:  return &ctx->Xmm2;
        case 3:  return &ctx->Xmm3;
        case 4:  return &ctx->Xmm4;
        case 5:  return &ctx->Xmm5;
        case 6:  return &ctx->Xmm6;
        case 7:  return &ctx->Xmm7;
        case 8:  return &ctx->Xmm8;
        case 9:  return &ctx->Xmm9;
        case 10: return &ctx->Xmm10;
        case 11: return &ctx->Xmm11;
        case 12: return &ctx->Xmm12;
        case 13: return &ctx->Xmm13;
        case 14: return &ctx->Xmm14;
        case 15: return &ctx->Xmm15;
        default: return nullptr;
    }
}

LONG CALLBACK kestrel_veh_handler(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ILLEGAL_INSTRUCTION) {
        const auto* ip = reinterpret_cast<const uint8_t*>(info->ContextRecord->Rip);
        const auto dec = decode_zen2_extension(ip, 15);
        if (dec.kind != Zen2Instruction::None) {
            uint64_t rax = info->ContextRecord->Rax;
            if (emulate_zen2_instruction(dec, rax, info->ContextRecord)) {
                info->ContextRecord->Rax = rax;
                info->ContextRecord->Rip += dec.length;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        g_unhandled.fetch_add(1, std::memory_order_relaxed);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

Zen2DecodeResult decode_zen2_extension(const uint8_t* code, size_t max_bytes) {
    if (!code || max_bytes < 3) return {};
    size_t i = 0;
    uint8_t mandatory_prefix = 0;
    uint8_t rex = 0;

    while (i < max_bytes && i < 4) {
        if (code[i] == 0x66 || code[i] == 0xF2) {
            mandatory_prefix = code[i++];
        } else if (code[i] == 0x67) {
            ++i;
        } else if (code[i] >= 0x40 && code[i] <= 0x4F) {
            rex = code[i++];
        } else {
            break;
        }
    }

    if (i + 3 > max_bytes) return {};

    // 1. Zen 2 CLZERO / MONITORX / MWAITX: 0F 01 FC/FA/FB
    if (mandatory_prefix == 0 && code[i] == 0x0F && code[i + 1] == 0x01) {
        switch (code[i + 2]) {
            case 0xFC: return {Zen2Instruction::Clzero, static_cast<uint32_t>(i + 3)};
            case 0xFA: return {Zen2Instruction::Monitorx, static_cast<uint32_t>(i + 3)};
            case 0xFB: return {Zen2Instruction::Mwaitx, static_cast<uint32_t>(i + 3)};
            default: break;
        }
    }

    // 2. AMD SSE4a EXTRQ / INSERTQ:
    // EXTRQ:   66 0F 78 /0 ib ib (imm) or 66 0F 79 /r (reg)
    // INSERTQ: F2 0F 78 /0 ib ib (imm) or F2 0F 79 /r (reg)
    if (code[i] == 0x0F && (code[i + 1] == 0x78 || code[i + 1] == 0x79)) {
        const uint8_t opcode = code[i + 1];
        if (i + 2 >= max_bytes) return {};
        const uint8_t modrm = code[i + 2];
        const uint8_t reg = (modrm >> 3) & 7;
        const uint8_t rm = modrm & 7;

        const uint8_t rex_r = (rex & 0x04) ? 8 : 0;
        const uint8_t rex_b = (rex & 0x01) ? 8 : 0;

        Zen2DecodeResult res;
        if (mandatory_prefix == 0x66) {
            res.kind = Zen2Instruction::Extrq;
        } else if (mandatory_prefix == 0xF2) {
            res.kind = Zen2Instruction::Insertq;
        } else {
            return {};
        }

        if (opcode == 0x78) {
            if (i + 4 >= max_bytes) return {};
            res.is_imm = true;
            res.dest_xmm = rm | rex_b;
            res.bit_len = code[i + 3] & 0x3F;
            res.bit_idx = code[i + 4] & 0x3F;
            res.length = static_cast<uint32_t>(i + 5);
            return res;
        } else if (opcode == 0x79) {
            res.is_imm = false;
            res.dest_xmm = rm | rex_b;
            res.src_xmm = reg | rex_r;
            res.length = static_cast<uint32_t>(i + 3);
            return res;
        }
    }

    return {};
}

bool emulate_zen2_instruction(Zen2Instruction kind, uint64_t& rax) {
    Zen2DecodeResult dec;
    dec.kind = kind;
    return emulate_zen2_instruction(dec, rax, nullptr);
}

bool emulate_zen2_instruction(const Zen2DecodeResult& dec, uint64_t& rax, void* context_record) {
    auto* ctx = reinterpret_cast<CONTEXT*>(context_record);

    switch (dec.kind) {
        case Zen2Instruction::Clzero: {
            const uint64_t line_addr = rax & ~uint64_t{63};
            auto* p = reinterpret_cast<void*>(line_addr);
            __try {
                std::memset(p, 0, 64);
                g_clzero.fetch_add(1, std::memory_order_relaxed);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
        case Zen2Instruction::Monitorx: {
            // Monitor setup: safe no-op on non-AMD / user-mode host
            g_monitorx.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        case Zen2Instruction::Mwaitx: {
            // Timed wait / pause hint: yield thread execution slice
            YieldProcessor();
            g_mwaitx.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        case Zen2Instruction::Extrq: {
            if (!ctx) return false;
            M128A* dest = get_xmm(ctx, dec.dest_xmm);
            if (!dest) return false;

            uint8_t len = dec.bit_len;
            uint8_t idx = dec.bit_idx;
            if (!dec.is_imm) {
                M128A* src = get_xmm(ctx, dec.src_xmm);
                if (!src) return false;
                len = static_cast<uint8_t>(src->Low & 0x3F);
                idx = static_cast<uint8_t>((src->Low >> 8) & 0x3F);
            }

            const uint64_t val = dest->Low;
            const uint32_t eff_len = (len == 0) ? 64 : len;
            uint64_t mask = (eff_len >= 64) ? ~uint64_t{0} : ((uint64_t{1} << eff_len) - 1);
            uint64_t extracted = (idx >= 64) ? 0 : ((val >> idx) & mask);

            dest->Low = extracted;
            dest->High = 0; // High 64 bits are zeroed by EXTRQ
            g_extrq.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        case Zen2Instruction::Insertq: {
            if (!ctx) return false;
            M128A* dest = get_xmm(ctx, dec.dest_xmm);
            if (!dest) return false;

            uint8_t len = dec.bit_len;
            uint8_t idx = dec.bit_idx;
            uint64_t src_val = 0;

            if (dec.is_imm) {
                src_val = dest->Low; // In imm form, source is low 64 bits of dest before insert
            } else {
                M128A* src = get_xmm(ctx, dec.src_xmm);
                if (!src) return false;
                len = static_cast<uint8_t>(src->High & 0x3F);
                idx = static_cast<uint8_t>((src->High >> 8) & 0x3F);
                src_val = src->Low;
            }

            const uint32_t eff_len = (len == 0) ? 64 : len;
            uint64_t src_mask = (eff_len >= 64) ? ~uint64_t{0} : ((uint64_t{1} << eff_len) - 1);
            uint64_t bits = src_val & src_mask;

            if (idx < 64) {
                uint64_t dest_mask = src_mask << idx;
                dest->Low = (dest->Low & ~dest_mask) | ((bits << idx) & dest_mask);
            }
            // High 64 bits remain unchanged in INSERTQ
            g_insertq.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        default:
            return false;
    }
}

ScopedVehShim::ScopedVehShim() {
    handle_ = AddVectoredExceptionHandler(1, kestrel_veh_handler);
}

ScopedVehShim::~ScopedVehShim() {
    if (handle_) {
        RemoveVectoredExceptionHandler(handle_);
        handle_ = nullptr;
    }
}

VehStats ScopedVehShim::stats() {
    return VehStats{
        .clzero_emulated = g_clzero.load(std::memory_order_relaxed),
        .monitorx_emulated = g_monitorx.load(std::memory_order_relaxed),
        .mwaitx_emulated = g_mwaitx.load(std::memory_order_relaxed),
        .extrq_emulated = g_extrq.load(std::memory_order_relaxed),
        .insertq_emulated = g_insertq.load(std::memory_order_relaxed),
        .unhandled_exceptions = g_unhandled.load(std::memory_order_relaxed),
    };
}

void ScopedVehShim::reset_stats() {
    g_clzero.store(0, std::memory_order_relaxed);
    g_monitorx.store(0, std::memory_order_relaxed);
    g_mwaitx.store(0, std::memory_order_relaxed);
    g_extrq.store(0, std::memory_order_relaxed);
    g_insertq.store(0, std::memory_order_relaxed);
    g_unhandled.store(0, std::memory_order_relaxed);
}

}  // namespace kestrel::cpu
