#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace kestrel::cpu {

enum class Zen2Instruction {
    None,
    Clzero,    // 0F 01 FC -- zero 64-byte cache line at [RAX & ~63] (or [EAX & ~63] with 0x67)
    Monitorx,  // 0F 01 FA -- AMD user-mode monitor setup (safe no-op)
    Mwaitx,    // 0F 01 FB -- AMD user-mode timed wait (yield/pause)
    Extrq,     // 66 0F 78 / 66 0F 79 -- AMD SSE4a bit extraction (faults on Intel CPUs)
    Insertq,   // F2 0F 78 / F2 0F 79 -- AMD SSE4a bit insertion (faults on Intel CPUs)
    Mcommit,   // F3 0F 01 FA -- AMD memory commit barrier
    Rdpru,     // 0F 01 FD -- AMD read processor register (ECX=0 MPERF, ECX=1 APERF -> EDX:EAX)
    Prefetchw, // 0F 0D /1 (PREFETCHW) or 0F 0D /2 (PREFETCHWT1) -- prefetch hint (no-op)
};

struct Zen2DecodeResult {
    Zen2Instruction kind = Zen2Instruction::None;
    uint32_t length = 0;
    uint8_t dest_xmm = 0;
    uint8_t src_xmm = 0;
    uint8_t bit_len = 0;
    uint8_t bit_idx = 0;
    bool is_imm = false;
    bool has_addr_size_override = false;
};

struct VehStats {
    uint64_t clzero_emulated = 0;
    uint64_t monitorx_emulated = 0;
    uint64_t mwaitx_emulated = 0;
    uint64_t extrq_emulated = 0;
    uint64_t insertq_emulated = 0;
    uint64_t unhandled_exceptions = 0;
    uint64_t mcommit_emulated = 0;
    uint64_t rdpru_emulated = 0;
    uint64_t prefetchw_emulated = 0;
};

// Pure decoder for AMD Zen 2 console-only and SSE4a instructions that fault with
// STATUS_ILLEGAL_INSTRUCTION (0xC000001D) on Intel x86-64 processors.
Zen2DecodeResult decode_zen2_extension(const uint8_t* code, size_t max_bytes);

// Emulate a decoded Zen 2 instruction against guest memory / registers.
// Returns true if handled.
bool emulate_zen2_instruction(const Zen2DecodeResult& decoded,
                              uint64_t& rax,
                              uint64_t rcx,
                              uint64_t& rdx);

// Convenience overloads.
bool emulate_zen2_instruction(Zen2Instruction kind, uint64_t& rax);
bool emulate_zen2_instruction(const Zen2DecodeResult& dec, uint64_t& rax, void* context_record = nullptr);

class ScopedVehShim {
public:
    ScopedVehShim();
    ~ScopedVehShim();
    ScopedVehShim(const ScopedVehShim&) = delete;
    ScopedVehShim& operator=(const ScopedVehShim&) = delete;

    static VehStats stats();
    static void reset_stats();

private:
    void* handle_ = nullptr;
};

}  // namespace kestrel::cpu
