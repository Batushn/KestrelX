#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kestrel::hle {

struct StubCallRecord {
    std::string dll;
    std::string symbol;
    uint64_t calls = 0;
};

class Registry {
public:
    Registry();
    ~Registry();
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    void register_module(std::string_view dll_name, bool allow_host_fallback = false);
    void register_export(std::string_view dll_name, std::string_view symbol, void* fn);
    void register_ordinal(std::string_view dll_name, uint16_t ordinal, std::string_view alias, void* fn);

    // Host overlay exports intercept specific Win32/CRT APIs (e.g., GetModuleHandle, ExitProcess,
    // CreateFile) without marking the host DLL as a console-only HLE module.
    void register_host_overlay(std::string_view dll_name, std::string_view symbol, void* fn);
    void* resolve_host_overlay(std::string_view dll_name, std::string_view symbol) const;

    bool has_module(std::string_view dll_name) const;
    bool allows_host_fallback(std::string_view dll_name) const;
    void* resolve_name(std::string_view dll_name, std::string_view symbol) const;
    void* resolve_ordinal(std::string_view dll_name, uint16_t ordinal) const;

    void* get_or_create_stub(std::string_view dll_name, std::string_view symbol);
    std::vector<StubCallRecord> called_stubs() const;
    size_t thunk_page_count() const;

private:
    struct StubSlot {
        std::string dll;
        std::string symbol;
        uint64_t count = 0;
        void* code = nullptr;
    };

    static uint64_t __cdecl on_stub_hit(StubSlot* slot);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unordered_map<std::string, void*>> named_;
    std::unordered_map<std::string, std::unordered_map<uint16_t, void*>> ordinals_;
    std::unordered_map<std::string, std::unordered_map<std::string, void*>> host_overlays_;
    std::unordered_map<std::string, bool> host_fallback_;
    std::unordered_map<std::string, std::unique_ptr<StubSlot>> stubs_;
    std::vector<uint8_t*> thunk_pages_;
    size_t thunk_used_ = 0;
    size_t thunk_capacity_ = 0;
};

}  // namespace kestrel::hle
