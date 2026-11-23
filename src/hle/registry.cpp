#include "kestrel/hle/registry.hpp"
#include "kestrel/common/types.hpp"

#include <Windows.h>
#include <cstring>

namespace kestrel::hle {

Registry::Registry() = default;

Registry::~Registry() {
    if (thunk_page_) {
        VirtualFree(thunk_page_, 0, MEM_RELEASE);
        thunk_page_ = nullptr;
    }
}

void Registry::register_module(std::string_view dll_name) {
    std::lock_guard lock(mutex_);
    named_[to_lower_ascii(dll_name)];
}

void Registry::register_export(std::string_view dll_name, std::string_view symbol, void* fn) {
    std::lock_guard lock(mutex_);
    named_[to_lower_ascii(dll_name)][std::string(symbol)] = fn;
}

void Registry::register_ordinal(std::string_view dll_name, uint16_t ordinal, std::string_view alias, void* fn) {
    std::lock_guard lock(mutex_);
    const auto key = to_lower_ascii(dll_name);
    ordinals_[key][ordinal] = fn;
    if (!alias.empty()) named_[key][std::string(alias)] = fn;
}

bool Registry::has_module(std::string_view dll_name) const {
    std::lock_guard lock(mutex_);
    const auto key = to_lower_ascii(dll_name);
    return named_.contains(key) || ordinals_.contains(key);
}

void* Registry::resolve_name(std::string_view dll_name, std::string_view symbol) const {
    std::lock_guard lock(mutex_);
    auto mit = named_.find(to_lower_ascii(dll_name));
    if (mit == named_.end()) return nullptr;
    auto sit = mit->second.find(std::string(symbol));
    return sit == mit->second.end() ? nullptr : sit->second;
}

void* Registry::resolve_ordinal(std::string_view dll_name, uint16_t ordinal) const {
    std::lock_guard lock(mutex_);
    auto mit = ordinals_.find(to_lower_ascii(dll_name));
    if (mit == ordinals_.end()) return nullptr;
    auto sit = mit->second.find(ordinal);
    return sit == mit->second.end() ? nullptr : sit->second;
}

uint64_t __cdecl Registry::on_stub_hit(StubSlot* slot) {
    if (slot) ++slot->count;
    return 0;
}

void* Registry::get_or_create_stub(std::string_view dll_name, std::string_view symbol) {
    std::lock_guard lock(mutex_);
    const std::string key = to_lower_ascii(dll_name) + "!" + std::string(symbol);
    if (auto it = stubs_.find(key); it != stubs_.end()) return it->second->code;

    constexpr size_t kThunkSize = 32;
    if (!thunk_page_ || thunk_used_ + kThunkSize > thunk_capacity_) {
        thunk_capacity_ = 65536;
        thunk_used_ = 0;
        thunk_page_ = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, thunk_capacity_, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    }
    if (!thunk_page_) return nullptr;

    auto slot = std::make_unique<StubSlot>();
    slot->dll = std::string(dll_name);
    slot->symbol = std::string(symbol);
    uint8_t* code = thunk_page_ + thunk_used_;
    thunk_used_ += kThunkSize;

    const uint64_t slot_addr = reinterpret_cast<uint64_t>(slot.get());
    const uint64_t fn_addr = reinterpret_cast<uint64_t>(&Registry::on_stub_hit);
    size_t p = 0;
    code[p++] = 0x48; code[p++] = 0xB9;
    std::memcpy(code + p, &slot_addr, 8); p += 8;
    code[p++] = 0x48; code[p++] = 0xB8;
    std::memcpy(code + p, &fn_addr, 8); p += 8;
    code[p++] = 0xFF; code[p++] = 0xE0;
    while (p < kThunkSize) code[p++] = 0xCC;

    FlushInstructionCache(GetCurrentProcess(), code, kThunkSize);
    slot->code = code;
    stubs_[key] = std::move(slot);
    return code;
}

std::vector<StubCallRecord> Registry::called_stubs() const {
    std::lock_guard lock(mutex_);
    std::vector<StubCallRecord> out;
    for (const auto& [_, slot] : stubs_) {
        if (slot->count > 0) out.push_back({slot->dll, slot->symbol, slot->count});
    }
    return out;
}

}  // namespace kestrel::hle
