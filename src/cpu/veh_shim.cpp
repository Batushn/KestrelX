#include "kestrel/cpu/veh_shim.hpp"

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstring>

namespace kestrel::cpu {

namespace {

std::atomic<uint64_t> g_clzero{0};
std::atomic<uint64_t> g_monitorx{0};
std::atomic<uint64_t> g_mwaitx{0};
std::atomic<uint64_t> g_extrq{0};
std::atomic<uint64_t> g_insertq{0};
std::atomic<uint64_t> g_unhandled{0};
std::atomic<uint64_t> g_mcommit{0};
std::atomic<uint64_t> g_rdpru{0};
std::atomic<uint64_t> g_prefetchw{0};

bool is_writable_cache_line(uint64_t line_addr) {
    if (line_addr == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(line_addr), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD base_protect = mbi.Protect & 0xFFu;
    const bool writable =
        base_protect == PAGE_READWRITE ||
        base_protect == PAGE_EXECUTE_READWRITE ||
        base_protect == PAGE_WRITECOPY ||
        base_protect == PAGE_EXECUTE_WRITECOPY;
    if (!writable) {
        return false;
    }
    const auto region_base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const auto region_end = region_base + mbi.RegionSize;
    if (line_addr < region_base || line_addr > region_end || (region_end - line_addr) < 64) {
        return false;
    }
    return true;
}

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
    if (!code || max_bytes < 3) {
        return {};
    }
    const size_t limit = std::min<size_t>(max_bytes, 15);
    size_t i = 0;
    uint8_t mandatory_prefix = 0;
    uint8_t rex = 0;
    bool has_addr_size_override = false;
    bool has_f3 = false;

    // Consume legacy prefixes (address-size override 0x67, mandatory 0x66/0xF2/0xF3, segment overrides).
    while (i < limit) {
        const uint8_t b = code[i];
        if (b == 0x66 || b == 0xF2) {
            mandatory_prefix = b;
            ++i;
        } else if (b == 0x67) {
            has_addr_size_override = true;
            ++i;
        } else if (b == 0xF3) {
            has_f3 = true;
            ++i;
        } else if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65) {
            ++i;
        } else {
            break;
        }
    }

    // Optional REX prefix (0x40..0x4F) immediately preceding the 0x0F opcode escape.
    if (i < limit && code[i] >= 0x40 && code[i] <= 0x4F) {
        rex = code[i++];
    }

    if (i + 3 > limit || code[i] != 0x0F) {
        return {};
    }

    // 1. Zen 2 CLZERO / MONITORX / MWAITX / MCOMMIT / RDPRU: 0F 01 ...
    if (mandatory_prefix == 0 && code[i + 1] == 0x01) {
        Zen2DecodeResult res{};
        res.length = static_cast<uint32_t>(i + 3);
        res.has_addr_size_override = has_addr_size_override;
        switch (code[i + 2]) {
            case 0xFA:
                res.kind = has_f3 ? Zen2Instruction::Mcommit : Zen2Instruction::Monitorx;
                return res;
            case 0xFB:
                if (!has_f3) { res.kind = Zen2Instruction::Mwaitx; return res; }
                break;
            case 0xFC:
                if (!has_f3) { res.kind = Zen2Instruction::Clzero; return res; }
                break;
            case 0xFD:
                if (!has_f3) { res.kind = Zen2Instruction::Rdpru; return res; }
                break;
            default:
                break;
        }
        return {};
    }

    // 2. PREFETCHW / PREFETCHWT1: 0F 0D /1 or /2
    if (mandatory_prefix == 0 && !has_f3 && code[i + 1] == 0x0D) {
        const uint8_t modrm = code[i + 2];
        const uint8_t mod = (modrm >> 6) & 0x03u;
        const uint8_t reg = (modrm >> 3) & 0x07u;
        const uint8_t rm = modrm & 0x07u;

        // PREFETCHW (0F 0D /1) and PREFETCHWT1 (0F 0D /2) require a memory operand (mod != 3).
        if ((reg != 1u && reg != 2u) || mod == 3u) {
            return {};
        }

        size_t len = i + 3;  // prefixes + 0F 0D + ModRM
        if (mod == 0u) {
            if (rm == 4u) {
                if (len >= limit) return {};
                const uint8_t sib = code[len++];
                if ((sib & 0x07u) == 5u) len += 4;
            } else if (rm == 5u) {
                len += 4;
            }
        } else if (mod == 1u) {
            if (rm == 4u) ++len;
            len += 1;
        } else {
            if (rm == 4u) ++len;
            len += 4;
        }

        if (len > limit) return {};
        Zen2DecodeResult res{};
        res.kind = Zen2Instruction::Prefetchw;
        res.length = static_cast<uint32_t>(len);
        res.has_addr_size_override = has_addr_size_override;
        return res;
    }

    // 3. AMD SSE4a EXTRQ / INSERTQ:
    // EXTRQ:   66 0F 78 /0 ib ib (imm) or 66 0F 79 /r (reg)
    // INSERTQ: F2 0F 78 /0 ib ib (imm) or F2 0F 79 /r (reg)
    if (!has_f3 && (code[i + 1] == 0x78 || code[i + 1] == 0x79)) {
        const uint8_t opcode = code[i + 1];
        const uint8_t modrm = code[i + 2];
        const uint8_t reg = (modrm >> 3) & 7;
        const uint8_t rm = modrm & 7;

        const uint8_t rex_r = (rex & 0x04) ? 8 : 0;
        const uint8_t rex_b = (rex & 0x01) ? 8 : 0;

        Zen2DecodeResult res{};
        res.has_addr_size_override = has_addr_size_override;
        if (mandatory_prefix == 0x66) {
            res.kind = Zen2Instruction::Extrq;
        } else if (mandatory_prefix == 0xF2) {
            res.kind = Zen2Instruction::Insertq;
        } else {
            return {};
        }

        if (opcode == 0x78) {
            if (i + 5 > limit) return {};
            res.is_imm = true;
            res.dest_xmm = rm | rex_b;
            res.bit_len = code[i + 3] & 0x3F;
            res.bit_idx = code[i + 4] & 0x3F;
            res.length = static_cast<uint32_t>(i + 5);
            return res;
        } else {
            res.is_imm = false;
            res.dest_xmm = rm | rex_b;
            res.src_xmm = reg | rex_r;
            res.length = static_cast<uint32_t>(i + 3);
            return res;
        }
    }

    return {};
}

namespace {

bool emulate_zen2_impl(const Zen2DecodeResult& dec,
                       uint64_t& rax,
                       uint64_t rcx,
                       uint64_t& rdx,
                       CONTEXT* ctx) {
    switch (dec.kind) {
        case Zen2Instruction::Clzero: {
            const uint64_t addr = dec.has_addr_size_override
                ? static_cast<uint64_t>(static_cast<uint32_t>(rax))
                : rax;
            const uint64_t line_addr = addr & ~uint64_t{63};
            if (!is_writable_cache_line(line_addr)) {
                return false;
            }
            std::memset(reinterpret_cast<void*>(line_addr), 0, 64);
            g_clzero.fetch_add(1, std::memory_order_relaxed);
            return true;
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
        case Zen2Instruction::Mcommit:
            std::atomic_thread_fence(std::memory_order_seq_cst);
            MemoryBarrier();
            g_mcommit.fetch_add(1, std::memory_order_relaxed);
            return true;
        case Zen2Instruction::Rdpru: {
            const uint32_t reg_id = static_cast<uint32_t>(rcx);
            uint64_t counter = 0;
            if (reg_id == 0u) {
                LARGE_INTEGER qpc{};
                if (QueryPerformanceCounter(&qpc) && qpc.QuadPart != 0) {
                    counter = static_cast<uint64_t>(qpc.QuadPart);
                } else {
                    counter = static_cast<uint64_t>(__rdtsc());
                }
            } else if (reg_id == 1u) {
                counter = static_cast<uint64_t>(__rdtsc());
            } else {
                return false;
            }
            rax = counter & 0xFFFFFFFFULL;
            rdx = (counter >> 32) & 0xFFFFFFFFULL;
            g_rdpru.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        case Zen2Instruction::Prefetchw:
            g_prefetchw.fetch_add(1, std::memory_order_relaxed);
            return true;
        case Zen2Instruction::None:
        default:
            return false;
    }
}

}  // namespace

bool emulate_zen2_instruction(const Zen2DecodeResult& decoded,
                              uint64_t& rax,
                              uint64_t rcx,
                              uint64_t& rdx) {
    return emulate_zen2_impl(decoded, rax, rcx, rdx, nullptr);
}

bool emulate_zen2_instruction(const Zen2DecodeResult& dec, uint64_t& rax, void* context_record) {
    auto* ctx = reinterpret_cast<CONTEXT*>(context_record);
    const uint64_t rcx = ctx ? ctx->Rcx : 0;
    uint64_t rdx = ctx ? ctx->Rdx : 0;
    const bool ok = emulate_zen2_impl(dec, rax, rcx, rdx, ctx);
    if (ok && ctx && dec.kind == Zen2Instruction::Rdpru) {
        ctx->Rdx = rdx;
    }
    return ok;
}

bool emulate_zen2_instruction(Zen2Instruction kind, uint64_t& rax) {
    uint64_t rdx = 0;
    Zen2DecodeResult dec{};
    dec.kind = kind;
    return emulate_zen2_impl(dec, rax, 0, rdx, nullptr);
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
        .mcommit_emulated = g_mcommit.load(std::memory_order_relaxed),
        .rdpru_emulated = g_rdpru.load(std::memory_order_relaxed),
        .prefetchw_emulated = g_prefetchw.load(std::memory_order_relaxed),
    };
}

void ScopedVehShim::reset_stats() {
    g_clzero.store(0, std::memory_order_relaxed);
    g_monitorx.store(0, std::memory_order_relaxed);
    g_mwaitx.store(0, std::memory_order_relaxed);
    g_extrq.store(0, std::memory_order_relaxed);
    g_insertq.store(0, std::memory_order_relaxed);
    g_unhandled.store(0, std::memory_order_relaxed);
    g_mcommit.store(0, std::memory_order_relaxed);
    g_rdpru.store(0, std::memory_order_relaxed);
    g_prefetchw.store(0, std::memory_order_relaxed);
}

}  // namespace kestrel::cpu
