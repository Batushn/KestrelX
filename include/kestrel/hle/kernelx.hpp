#pragma once

#include "kestrel/hle/registry.hpp"
#include "kestrel/vfs/overlay_vfs.hpp"

#include <cstdint>

namespace kestrel::hle {

struct KernelXStats {
    uint64_t alloc_count = 0;
    uint64_t free_count = 0;
    uint64_t live_bytes = 0;
    uint64_t peak_bytes = 0;
};

void set_kernelx_vfs(vfs::OverlayVfs* vfs);
void register_kernelx(Registry& reg);
KernelXStats kernelx_stats();
void reset_kernelx_stats();

}  // namespace kestrel::hle
