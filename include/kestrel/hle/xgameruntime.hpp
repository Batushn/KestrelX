#pragma once

#include "kestrel/hle/registry.hpp"
#include <cstdint>

namespace kestrel::hle {

void set_xgameruntime_title_id(uint32_t title_id);
void register_xgameruntime(Registry& reg);

}  // namespace kestrel::hle
