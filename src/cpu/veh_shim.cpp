#include "kestrel/cpu/veh_shim.hpp"

#include <Windows.h>
#include <cstring>

namespace kestrel::cpu {

namespace {

std::atomic<uint64_t> g_clzero{0};
std::atomic<uint64_t> g_monitorx{0};
std::atomic<uint64_t> g_mwaitx{0};
std::atomic<uint64_t> g_unhandled{0};

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
            if (emulate_zen2_instruction(dec.kind, rax)) {
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
    // Skip optional REX (0x40..0x4F) or address-size override (0x67) prefixes
    while (i < max_bytes && i < 4 && (code[i] == 0x67 || (code[i] >= 0x40 && code[i] <= 0x4F))) {
        ++i;
    }
    if (i + 3 > max_bytes) return {};
    if (code[i] == 0x0F && code[i + 1] == 0x01) {
        switch (code[i + 2]) {
            case 0xFC: return {Zen2Instruction::Clzero, static_cast<uint32_t>(i + 3)};
            case 0xFA: return {Zen2Instruction::Monitorx, static_cast<uint32_t>(i + 3)};
            case 0xFB: return {Zen2Instruction::Mwaitx, static_cast<uint32_t>(i + 3)};
            default: break;
        }
    }
    return {};
}

bool emulate_zen2_instruction(Zen2Instruction kind, uint64_t& rax) {
    switch (kind) {
        case Zen2Instruction::Clzero: {
            const uint64_t line_addr = rax & ~uint64_t{63};
            if (line_addr == 0) return false;
            std::memset(reinterpret_cast<void*>(line_addr), 0, 64);
            g_clzero.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        case Zen2Instruction::Monitorx:
            g_monitorx.fetch_add(1, std::memory_order_relaxed);
            return true;
        case Zen2Instruction::Mwaitx:
            SwitchToThread();
            g_mwaitx.fetch_add(1, std::memory_order_relaxed);
            return true;
        case Zen2Instruction::None:
            return false;
    }
    return false;
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
    return {
        g_clzero.load(std::memory_order_relaxed),
        g_monitorx.load(std::memory_order_relaxed),
        g_mwaitx.load(std::memory_order_relaxed),
        g_unhandled.load(std::memory_order_relaxed),
    };
}

void ScopedVehShim::reset_stats() {
    g_clzero.store(0, std::memory_order_relaxed);
    g_monitorx.store(0, std::memory_order_relaxed);
    g_mwaitx.store(0, std::memory_order_relaxed);
    g_unhandled.store(0, std::memory_order_relaxed);
}

}  // namespace kestrel::cpu
