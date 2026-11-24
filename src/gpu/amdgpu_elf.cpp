#include "kestrel/gpu/amdgpu_elf.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <variant>

namespace kestrel::gpu {

namespace {

constexpr uint16_t kElfMachineAmdgpu = 224;
constexpr uint32_t kNtAmdgpuMetadata = 32;

uint16_t rd16(const uint8_t* p) {
    uint16_t v = 0;
    std::memcpy(&v, p, 2);
    return v;
}
uint32_t rd32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}
uint64_t rd64(const uint8_t* p) {
    uint64_t v = 0;
    std::memcpy(&v, p, 8);
    return v;
}
uint16_t rd16_be(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
uint32_t rd32_be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}
uint64_t rd64_be(const uint8_t* p) {
    return (static_cast<uint64_t>(rd32_be(p)) << 32) | rd32_be(p + 4);
}

struct ElfSection {
    std::string name;
    uint32_t type = 0;
    uint64_t flags = 0;
    uint64_t addr = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t link = 0;
    uint64_t entsize = 0;
};

struct ElfSymbol {
    std::string name;
    uint64_t value = 0;
    uint64_t size = 0;
    uint16_t shndx = 0;
    uint8_t type = 0;
};

// Lightweight read-only MsgPack parser for NT_AMDGPU_METADATA
struct MpValue;
using MpArray = std::vector<MpValue>;
using MpMap = std::vector<std::pair<std::string, MpValue>>;
struct MpValue {
    std::variant<std::monostate, bool, int64_t, uint64_t, std::string, MpArray, MpMap> v;

    const MpValue* find(std::string_view key) const {
        if (auto* m = std::get_if<MpMap>(&v)) {
            for (const auto& kv : *m) {
                if (kv.first == key) return &kv.second;
            }
        }
        return nullptr;
    }
    std::string as_str() const {
        if (auto* s = std::get_if<std::string>(&v)) return *s;
        return {};
    }
    uint64_t as_u64(uint64_t def = 0) const {
        if (auto* u = std::get_if<uint64_t>(&v)) return *u;
        if (auto* i = std::get_if<int64_t>(&v)) return (*i >= 0) ? static_cast<uint64_t>(*i) : def;
        return def;
    }
    bool as_bool(bool def = false) const {
        if (auto* b = std::get_if<bool>(&v)) return *b;
        return def;
    }
};

struct MpCursor {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;
    int depth = 0;

    bool read_bytes(size_t n, const uint8_t*& out) {
        if (static_cast<size_t>(end - p) < n) return false;
        out = p;
        p += n;
        return true;
    }

    bool parse(MpValue& out) {
        if (p >= end || depth > 32) return false;
        const uint8_t tag = *p++;
        const uint8_t* ptr = nullptr;

        if (tag <= 0x7Fu) {
            out.v = static_cast<uint64_t>(tag);
            return true;
        }
        if (tag >= 0xE0u) {
            out.v = static_cast<int64_t>(static_cast<int8_t>(tag));
            return true;
        }
        if ((tag & 0xE0u) == 0xA0u) {
            const size_t len = tag & 0x1Fu;
            if (!read_bytes(len, ptr)) return false;
            out.v = std::string(reinterpret_cast<const char*>(ptr), len);
            return true;
        }
        if ((tag & 0xF0u) == 0x90u) {
            const size_t cnt = tag & 0x0Fu;
            MpArray arr;
            arr.reserve(cnt);
            ++depth;
            for (size_t i = 0; i < cnt; ++i) {
                MpValue elem;
                if (!parse(elem)) { --depth; return false; }
                arr.push_back(std::move(elem));
            }
            --depth;
            out.v = std::move(arr);
            return true;
        }
        if ((tag & 0xF0u) == 0x80u) {
            const size_t cnt = tag & 0x0Fu;
            MpMap m;
            m.reserve(cnt);
            ++depth;
            for (size_t i = 0; i < cnt; ++i) {
                MpValue k, val;
                if (!parse(k) || !parse(val)) { --depth; return false; }
                m.emplace_back(k.as_str(), std::move(val));
            }
            --depth;
            out.v = std::move(m);
            return true;
        }

        switch (tag) {
            case 0xC0: out.v = std::monostate{}; return true;
            case 0xC2: out.v = false; return true;
            case 0xC3: out.v = true; return true;
            case 0xC4: { // bin 8
                if (!read_bytes(1, ptr)) return false;
                const size_t len = ptr[0];
                if (!read_bytes(len, ptr)) return false;
                out.v = std::monostate{};
                return true;
            }
            case 0xC5: { // bin 16
                if (!read_bytes(2, ptr)) return false;
                const size_t len = rd16_be(ptr);
                if (!read_bytes(len, ptr)) return false;
                out.v = std::monostate{};
                return true;
            }
            case 0xCA: { // float32
                if (!read_bytes(4, ptr)) return false;
                out.v = std::monostate{};
                return true;
            }
            case 0xCB: { // float64
                if (!read_bytes(8, ptr)) return false;
                out.v = std::monostate{};
                return true;
            }
            case 0xCC: if (!read_bytes(1, ptr)) return false; out.v = static_cast<uint64_t>(ptr[0]); return true;
            case 0xCD: if (!read_bytes(2, ptr)) return false; out.v = static_cast<uint64_t>(rd16_be(ptr)); return true;
            case 0xCE: if (!read_bytes(4, ptr)) return false; out.v = static_cast<uint64_t>(rd32_be(ptr)); return true;
            case 0xCF: if (!read_bytes(8, ptr)) return false; out.v = rd64_be(ptr); return true;
            case 0xD0: if (!read_bytes(1, ptr)) return false; out.v = static_cast<int64_t>(static_cast<int8_t>(ptr[0])); return true;
            case 0xD1: if (!read_bytes(2, ptr)) return false; out.v = static_cast<int64_t>(static_cast<int16_t>(rd16_be(ptr))); return true;
            case 0xD2: if (!read_bytes(4, ptr)) return false; out.v = static_cast<int64_t>(static_cast<int32_t>(rd32_be(ptr))); return true;
            case 0xD3: if (!read_bytes(8, ptr)) return false; out.v = static_cast<int64_t>(rd64_be(ptr)); return true;
            case 0xD9: { // str 8
                if (!read_bytes(1, ptr)) return false;
                const size_t len = ptr[0];
                if (!read_bytes(len, ptr)) return false;
                out.v = std::string(reinterpret_cast<const char*>(ptr), len);
                return true;
            }
            case 0xDA: { // str 16
                if (!read_bytes(2, ptr)) return false;
                const size_t len = rd16_be(ptr);
                if (!read_bytes(len, ptr)) return false;
                out.v = std::string(reinterpret_cast<const char*>(ptr), len);
                return true;
            }
            case 0xDB: { // str 32
                if (!read_bytes(4, ptr)) return false;
                const size_t len = rd32_be(ptr);
                if (!read_bytes(len, ptr)) return false;
                out.v = std::string(reinterpret_cast<const char*>(ptr), len);
                return true;
            }
            case 0xDC: { // array 16
                if (!read_bytes(2, ptr)) return false;
                const size_t cnt = rd16_be(ptr);
                MpArray arr;
                arr.reserve(cnt);
                ++depth;
                for (size_t i = 0; i < cnt; ++i) {
                    MpValue elem;
                    if (!parse(elem)) { --depth; return false; }
                    arr.push_back(std::move(elem));
                }
                --depth;
                out.v = std::move(arr);
                return true;
            }
            case 0xDE: { // map 16
                if (!read_bytes(2, ptr)) return false;
                const size_t cnt = rd16_be(ptr);
                MpMap m;
                m.reserve(cnt);
                ++depth;
                for (size_t i = 0; i < cnt; ++i) {
                    MpValue k, val;
                    if (!parse(k) || !parse(val)) { --depth; return false; }
                    m.emplace_back(k.as_str(), std::move(val));
                }
                --depth;
                out.v = std::move(m);
                return true;
            }
            default:
                return false;
        }
    }
};

std::string read_cstr(std::span<const uint8_t> table, uint32_t idx) {
    if (idx >= table.size()) return {};
    const char* s = reinterpret_cast<const char*>(table.data() + idx);
    size_t max_len = table.size() - idx;
    size_t len = 0;
    while (len < max_len && s[len] != '\0') ++len;
    return std::string(s, len);
}

void decode_kernel_descriptor_sgprs(std::span<const uint8_t> kd_bytes, AmdgpuKernelInfo& k) {
    if (kd_bytes.size() < 64) return;
    k.group_segment_bytes = std::max(k.group_segment_bytes, rd32(kd_bytes.data() + 0x00));
    k.private_segment_bytes = std::max(k.private_segment_bytes, rd32(kd_bytes.data() + 0x04));
    const uint32_t ka_sz = rd32(kd_bytes.data() + 0x08);
    if (k.kernarg_size == 0) k.kernarg_size = ka_sz;

    const uint32_t rsrc2 = rd32(kd_bytes.data() + 0x34);
    const uint16_t props = rd16(kd_bytes.data() + 0x38);

    const uint32_t user_sgpr_cnt = (rsrc2 >> 1) & 0x1Fu;
    k.user_sgpr_count = user_sgpr_cnt;

    uint32_t sgpr_cursor = 0;
    if ((props & (1u << 0)) != 0) sgpr_cursor += 4;  // PRIVATE_SEGMENT_BUFFER
    if ((props & (1u << 1)) != 0) sgpr_cursor += 2;  // DISPATCH_PTR
    if ((props & (1u << 2)) != 0) sgpr_cursor += 2;  // QUEUE_PTR
    if ((props & (1u << 3)) != 0) {                  // KERNARG_SEGMENT_PTR
        k.has_kernarg_ptr = true;
        k.kernarg_sgpr = sgpr_cursor;
        sgpr_cursor += 2;
    }
    if ((props & (1u << 4)) != 0) sgpr_cursor += 2;  // DISPATCH_ID

    if ((props & (1u << 10)) != 0) k.wavefront_size = 32;

    uint32_t sys_sgpr = std::max(user_sgpr_cnt, sgpr_cursor);
    if ((rsrc2 & (1u << 7)) != 0) {
        k.has_wg_id_x = true;
        k.wg_id_x_sgpr = sys_sgpr++;
    }
    if ((rsrc2 & (1u << 8)) != 0) {
        k.has_wg_id_y = true;
        k.wg_id_y_sgpr = sys_sgpr++;
    }
    if ((rsrc2 & (1u << 9)) != 0) {
        k.has_wg_id_z = true;
        k.wg_id_z_sgpr = sys_sgpr++;
    }
}

}  // namespace

bool is_amdgpu_elf(std::span<const uint8_t> bytes) {
    if (bytes.size() < 64) return false;
    if (bytes[0] != 0x7Fu || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') return false;
    if (bytes[4] != 2 || bytes[5] != 1) return false;  // ELF64 little-endian
    return rd16(bytes.data() + 18) == kElfMachineAmdgpu;
}

Result<AmdgpuElfImage> parse_amdgpu_elf(std::span<const uint8_t> bytes) {
    if (!is_amdgpu_elf(bytes)) {
        return fail("Not a valid little-endian ELF64-AMDGPU image");
    }

    const uint64_t shoff = rd64(bytes.data() + 40);
    const uint16_t shentsize = rd16(bytes.data() + 58);
    const uint16_t shnum = rd16(bytes.data() + 60);
    const uint16_t shstrndx = rd16(bytes.data() + 62);

    if (shoff == 0 || shentsize < 64 || shnum == 0 ||
        shoff + static_cast<uint64_t>(shnum) * shentsize > bytes.size()) {
        return fail("Corrupt ELF64 section header table");
    }

    std::vector<ElfSection> sections(shnum);
    for (uint16_t i = 0; i < shnum; ++i) {
        const uint8_t* sh = bytes.data() + shoff + static_cast<size_t>(i) * shentsize;
        sections[i].type = rd32(sh + 4);
        sections[i].flags = rd64(sh + 8);
        sections[i].addr = rd64(sh + 16);
        sections[i].offset = rd64(sh + 24);
        sections[i].size = rd64(sh + 32);
        sections[i].link = rd32(sh + 40);
        sections[i].entsize = rd64(sh + 56);
    }

    std::span<const uint8_t> shstrtab;
    if (shstrndx < shnum && sections[shstrndx].offset + sections[shstrndx].size <= bytes.size()) {
        shstrtab = bytes.subspan(static_cast<size_t>(sections[shstrndx].offset),
                                 static_cast<size_t>(sections[shstrndx].size));
    }
    for (uint16_t i = 0; i < shnum; ++i) {
        const uint8_t* sh = bytes.data() + shoff + static_cast<size_t>(i) * shentsize;
        sections[i].name = read_cstr(shstrtab, rd32(sh + 0));
    }

    auto find_section = [&](std::string_view nm) -> const ElfSection* {
        for (const auto& s : sections) {
            if (s.name == nm) return &s;
        }
        return nullptr;
    };

    // Read symbol tables (.symtab preferred, fallback .dynsym)
    std::vector<ElfSymbol> symbols;
    for (const auto& s : sections) {
        if (s.type != 2 && s.type != 11) continue;  // SHT_SYMTAB or SHT_DYNSYM
        if (s.offset + s.size > bytes.size() || s.entsize < 24 || s.link >= shnum) continue;
        const auto& strsec = sections[s.link];
        if (strsec.offset + strsec.size > bytes.size()) continue;
        const auto strtab = bytes.subspan(static_cast<size_t>(strsec.offset), static_cast<size_t>(strsec.size));
        const size_t count = static_cast<size_t>(s.size / s.entsize);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* sym = bytes.data() + s.offset + i * s.entsize;
            ElfSymbol es{};
            es.name = read_cstr(strtab, rd32(sym + 0));
            es.type = sym[4] & 0x0Fu;
            es.shndx = rd16(sym + 6);
            es.value = rd64(sym + 8);
            es.size = rd64(sym + 16);
            if (!es.name.empty()) symbols.push_back(std::move(es));
        }
    }

    auto find_symbol = [&](std::string_view nm) -> const ElfSymbol* {
        for (const auto& sym : symbols) {
            if (sym.name == nm) return &sym;
        }
        return nullptr;
    };

    AmdgpuElfImage image{};

    // Parse NT_AMDGPU_METADATA in SHT_NOTE (.note)
    for (const auto& s : sections) {
        if (s.type != 7 || s.offset + s.size > bytes.size()) continue;
        size_t pos = static_cast<size_t>(s.offset);
        const size_t end = static_cast<size_t>(s.offset + s.size);
        while (pos + 12 <= end) {
            const uint32_t namesz = rd32(bytes.data() + pos);
            const uint32_t descsz = rd32(bytes.data() + pos + 4);
            const uint32_t ntype = rd32(bytes.data() + pos + 8);
            pos += 12;
            const size_t namesz_al = (namesz + 3u) & ~3u;
            const size_t descsz_al = (descsz + 3u) & ~3u;
            if (pos + namesz_al + descsz_al > end) break;
            std::string owner(reinterpret_cast<const char*>(bytes.data() + pos),
                              namesz > 0 ? namesz - 1 : 0);
            const uint8_t* desc = bytes.data() + pos + namesz_al;
            pos += namesz_al + descsz_al;

            if (owner == "AMDGPU" && ntype == kNtAmdgpuMetadata && descsz > 0) {
                MpCursor cur{desc, desc + descsz};
                MpValue root;
                if (cur.parse(root)) {
                    if (const auto* t = root.find("amdhsa.target")) {
                        image.target = t->as_str();
                    }
                    if (const auto* ks = root.find("amdhsa.kernels")) {
                        if (const auto* arr = std::get_if<MpArray>(&ks->v)) {
                            for (const auto& kv : *arr) {
                                AmdgpuKernelInfo ki{};
                                ki.target = image.target;
                                if (const auto* p = kv.find(".name")) ki.name = p->as_str();
                                if (const auto* p = kv.find(".symbol")) ki.kd_symbol = p->as_str();
                                if (const auto* p = kv.find(".kernarg_segment_size")) ki.kernarg_size = static_cast<uint32_t>(p->as_u64());
                                if (const auto* p = kv.find(".group_segment_fixed_size")) ki.group_segment_bytes = static_cast<uint32_t>(p->as_u64());
                                if (const auto* p = kv.find(".private_segment_fixed_size")) ki.private_segment_bytes = static_cast<uint32_t>(p->as_u64());
                                if (const auto* p = kv.find(".wavefront_size")) ki.wavefront_size = static_cast<uint32_t>(p->as_u64(32));
                                if (const auto* p = kv.find(".sgpr_count")) ki.sgpr_count = static_cast<uint32_t>(p->as_u64());
                                if (const auto* p = kv.find(".vgpr_count")) ki.vgpr_count = static_cast<uint32_t>(p->as_u64());
                                if (const auto* args = kv.find(".args")) {
                                    if (const auto* aarr = std::get_if<MpArray>(&args->v)) {
                                        for (const auto& av : *aarr) {
                                            AmdgpuKernelArg arg{};
                                            if (const auto* f = av.find(".value_kind")) arg.value_kind = f->as_str();
                                            if (const auto* f = av.find(".type_name")) arg.type_name = f->as_str();
                                            if (const auto* f = av.find(".offset")) arg.offset = static_cast<uint32_t>(f->as_u64());
                                            if (const auto* f = av.find(".size")) arg.size = static_cast<uint32_t>(f->as_u64());
                                            if (const auto* f = av.find(".is_const")) arg.is_const = f->as_bool();
                                            ki.args.push_back(std::move(arg));
                                        }
                                    }
                                }
                                image.kernels.push_back(std::move(ki));
                            }
                        }
                    }
                }
            }
        }
    }

    const ElfSection* text_sec = find_section(".text");
    if (!text_sec || text_sec->offset + text_sec->size > bytes.size() || text_sec->size < 4) {
        return fail("AMDGPU ELF contains no valid .text section");
    }

    // Helper to extract code words starting at VA inside .text
    auto extract_text_at_va = [&](uint64_t va, uint64_t sym_size, std::vector<uint32_t>& out_words) -> bool {
        if (va < text_sec->addr || va >= text_sec->addr + text_sec->size) return false;
        const uint64_t rel = va - text_sec->addr;
        const uint64_t max_bytes = text_sec->size - rel;
        uint64_t copy_bytes = (sym_size > 0 && sym_size <= max_bytes) ? sym_size : max_bytes;
        const size_t nwords = static_cast<size_t>(copy_bytes / 4);
        if (nwords == 0) return false;
        out_words.resize(nwords);
        std::memcpy(out_words.data(), bytes.data() + text_sec->offset + rel, nwords * 4);
        return true;
    };

    // Match kernels with .rodata Kernel Descriptors & .text symbols
    for (auto& ki : image.kernels) {
        const ElfSymbol* kd_sym = !ki.kd_symbol.empty() ? find_symbol(ki.kd_symbol) : nullptr;
        const ElfSymbol* fn_sym = !ki.name.empty() ? find_symbol(ki.name) : nullptr;

        if (kd_sym && kd_sym->shndx < shnum) {
            const auto& kd_sec = sections[kd_sym->shndx];
            if (kd_sym->value >= kd_sec.addr && kd_sec.offset + kd_sec.size <= bytes.size()) {
                const uint64_t rel = kd_sym->value - kd_sec.addr;
                if (rel + 64 <= kd_sec.size) {
                    const auto kd_span = bytes.subspan(static_cast<size_t>(kd_sec.offset + rel), 64);
                    decode_kernel_descriptor_sgprs(kd_span, ki);
                    int64_t entry_off = 0;
                    std::memcpy(&entry_off, kd_span.data() + 0x10, 8);
                    const uint64_t entry_va = static_cast<uint64_t>(static_cast<int64_t>(kd_sym->value) + entry_off);
                    ki.code_va = entry_va;
                    extract_text_at_va(entry_va, fn_sym ? fn_sym->size : 0, ki.code_words);
                }
            }
        }
        if (ki.code_words.empty() && fn_sym) {
            ki.code_va = fn_sym->value;
            extract_text_at_va(fn_sym->value, fn_sym->size, ki.code_words);
        }
    }

    // Fallback if ELF had no NT_AMDGPU_METADATA (e.g., raw object file .o)
    if (image.kernels.empty()) {
        AmdgpuKernelInfo ki{};
        ki.name = "text_entry";
        ki.code_va = text_sec->addr;
        extract_text_at_va(text_sec->addr, text_sec->size, ki.code_words);
        image.kernels.push_back(std::move(ki));
    }

    return image;
}

Result<AmdgpuElfImage> load_amdgpu_elf_file(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        return fail(std::format("Failed to open ELF file: {}", path.string()));
    }
    const auto sz = ifs.tellg();
    if (sz <= 0) {
        return fail(std::format("Empty ELF file: {}", path.string()));
    }
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    if (!ifs.read(reinterpret_cast<char*>(buf.data()), sz)) {
        return fail(std::format("Failed reading ELF file: {}", path.string()));
    }
    return parse_amdgpu_elf(buf);
}

std::vector<uint8_t> build_amdhsa_kernarg_blob(
    const AmdgpuKernelInfo& kernel,
    std::span<const uint64_t> user_arg_values,
    uint32_t block_count_x,
    uint32_t group_size_x,
    uint32_t block_count_y,
    uint32_t block_count_z,
    uint32_t group_size_y,
    uint32_t group_size_z,
    uint64_t global_offset_x,
    uint64_t global_offset_y,
    uint64_t global_offset_z) {
    size_t max_end = std::max<size_t>(kernel.kernarg_size, 128u);
    for (const auto& a : kernel.args) {
        max_end = std::max(max_end, static_cast<size_t>(a.offset) + a.size);
    }
    std::vector<uint8_t> blob(max_end, 0);

    size_t user_idx = 0;
    for (const auto& a : kernel.args) {
        uint64_t val = 0;
        bool write = false;
        if (a.value_kind.rfind("hidden_", 0) != 0) {
            if (user_idx < user_arg_values.size()) {
                val = user_arg_values[user_idx++];
                write = true;
            }
        } else if (a.value_kind == "hidden_block_count_x") {
            val = block_count_x;
            write = true;
        } else if (a.value_kind == "hidden_block_count_y") {
            val = block_count_y;
            write = true;
        } else if (a.value_kind == "hidden_block_count_z") {
            val = block_count_z;
            write = true;
        } else if (a.value_kind == "hidden_group_size_x") {
            val = group_size_x;
            write = true;
        } else if (a.value_kind == "hidden_group_size_y") {
            val = group_size_y;
            write = true;
        } else if (a.value_kind == "hidden_group_size_z") {
            val = group_size_z;
            write = true;
        } else if (a.value_kind == "hidden_global_offset_x") {
            val = global_offset_x;
            write = true;
        } else if (a.value_kind == "hidden_global_offset_y") {
            val = global_offset_y;
            write = true;
        } else if (a.value_kind == "hidden_global_offset_z") {
            val = global_offset_z;
            write = true;
        } else if (a.value_kind == "hidden_grid_dims") {
            val = (block_count_z > 1) ? 3u : ((block_count_y > 1) ? 2u : 1u);
            write = true;
        }

        if (write && static_cast<size_t>(a.offset) + a.size <= blob.size()) {
            const size_t nbytes = std::min<size_t>(a.size, sizeof(val));
            std::memcpy(blob.data() + a.offset, &val, nbytes);
        }
    }
    return blob;
}

}  // namespace kestrel::gpu
