#include "kestrel/loader/pe_image.hpp"

#include <Windows.h>
#include <algorithm>
#include <cstring>

namespace kestrel::loader {

namespace {

template <typename T>
bool read_pod(const std::vector<uint8_t>& buf, size_t off, T& out) {
    if (off > buf.size() || sizeof(T) > buf.size() - off) return false;
    std::memcpy(&out, buf.data() + off, sizeof(T));
    return true;
}

}  // namespace

std::optional<size_t> PeImage::rva_to_file_offset(uint32_t rva) const {
    if (rva < size_of_headers && rva < raw.size()) {
        return rva;
    }
    for (const auto& s : sections) {
        const uint32_t span = std::max(s.virtual_size, s.raw_size);
        if (rva >= s.virtual_address && (rva - s.virtual_address) < span) {
            const uint32_t delta = rva - s.virtual_address;
            if (delta < s.raw_size && static_cast<size_t>(s.raw_offset) + delta < raw.size()) {
                return static_cast<size_t>(s.raw_offset) + delta;
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::string> PeImage::read_cstring_at_rva(uint32_t rva, size_t max_len) const {
    auto off = rva_to_file_offset(rva);
    if (!off) return std::nullopt;
    std::string out;
    for (size_t i = *off; i < raw.size() && out.size() < max_len; ++i) {
        if (raw[i] == 0) return out;
        out.push_back(static_cast<char>(raw[i]));
    }
    return std::nullopt;
}

Result<PeImage> parse_pe64(std::vector<uint8_t> bytes, std::string filename) {
    PeImage img;
    img.filename = std::move(filename);
    img.raw = std::move(bytes);

    IMAGE_DOS_HEADER dos{};
    if (!read_pod(img.raw, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        return fail("Not a valid PE file (missing MZ header)");
    }
    if (dos.e_lfanew < 0) {
        return fail("Invalid e_lfanew offset");
    }
    const size_t nt_off = static_cast<size_t>(dos.e_lfanew);
    uint32_t pe_sig = 0;
    if (!read_pod(img.raw, nt_off, pe_sig) || pe_sig != IMAGE_NT_SIGNATURE) {
        return fail("Missing PE\0\0 signature");
    }

    IMAGE_FILE_HEADER fh{};
    if (!read_pod(img.raw, nt_off + 4, fh)) {
        return fail("Truncated COFF file header");
    }
    if (fh.Machine != IMAGE_FILE_MACHINE_AMD64) {
        return fail("Only x86-64 (PE32+) images are supported; got machine " + hex32(fh.Machine));
    }

    IMAGE_OPTIONAL_HEADER64 opt{};
    const size_t opt_off = nt_off + 4 + sizeof(IMAGE_FILE_HEADER);
    if (!read_pod(img.raw, opt_off, opt) || opt.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return fail("Not a PE32+ (64-bit) optional header");
    }

    img.machine = fh.Machine;
    img.characteristics = fh.Characteristics;
    img.subsystem = opt.Subsystem;
    img.image_base = opt.ImageBase;
    img.size_of_image = opt.SizeOfImage;
    img.size_of_headers = opt.SizeOfHeaders;
    img.entry_point_rva = opt.AddressOfEntryPoint;
    img.is_dll = (fh.Characteristics & IMAGE_FILE_DLL) != 0;

    const uint32_t num_dirs = std::min<uint32_t>(opt.NumberOfRvaAndSizes, IMAGE_NUMBEROF_DIRECTORY_ENTRIES);
    img.data_directories.resize(IMAGE_NUMBEROF_DIRECTORY_ENTRIES);
    for (uint32_t i = 0; i < num_dirs; ++i) {
        img.data_directories[i] = {opt.DataDirectory[i].VirtualAddress, opt.DataDirectory[i].Size};
    }

    const size_t sec_off = opt_off + fh.SizeOfOptionalHeader;
    img.sections.reserve(fh.NumberOfSections);
    for (uint16_t i = 0; i < fh.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER sh{};
        if (!read_pod(img.raw, sec_off + i * sizeof(IMAGE_SECTION_HEADER), sh)) {
            return fail("Truncated section header table");
        }
        PeSection s;
        size_t nlen = 0;
        while (nlen < 8 && sh.Name[nlen] != 0) ++nlen;
        s.name.assign(reinterpret_cast<const char*>(sh.Name), nlen);
        s.virtual_size = sh.Misc.VirtualSize;
        s.virtual_address = sh.VirtualAddress;
        s.raw_size = sh.SizeOfRawData;
        s.raw_offset = sh.PointerToRawData;
        s.characteristics = sh.Characteristics;
        img.sections.push_back(std::move(s));
    }

    // Parse standard imports
    const auto imp_dir = img.data_directories[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imp_dir.rva != 0 && imp_dir.size != 0) {
        for (uint32_t idx = 0;; ++idx) {
            const uint64_t desc_off_rva = static_cast<uint64_t>(idx) * sizeof(IMAGE_IMPORT_DESCRIPTOR);
            if (desc_off_rva > 0xFFFFFFFFull - imp_dir.rva) break;
            const uint32_t desc_rva = static_cast<uint32_t>(imp_dir.rva + desc_off_rva);
            auto desc_off = img.rva_to_file_offset(desc_rva);
            if (!desc_off) break;
            IMAGE_IMPORT_DESCRIPTOR desc{};
            if (!read_pod(img.raw, *desc_off, desc)) break;
            if (desc.Name == 0 && desc.FirstThunk == 0) break;

            auto dll_name = img.read_cstring_at_rva(desc.Name);
            if (!dll_name) return fail("Unreadable import DLL name at RVA " + hex32(desc.Name));

            PeImportModule mod;
            mod.dll_name = *dll_name;
            mod.is_delay_load = false;

            const uint32_t ilt_rva = desc.OriginalFirstThunk ? desc.OriginalFirstThunk : desc.FirstThunk;
            const uint32_t iat_rva = desc.FirstThunk;
            for (uint32_t k = 0;; ++k) {
                const uint64_t stride = static_cast<uint64_t>(k) * 8u;
                if (stride > 0xFFFFFFFFull - ilt_rva || stride > 0xFFFFFFFFull - iat_rva) break;
                auto t_off = img.rva_to_file_offset(static_cast<uint32_t>(ilt_rva + stride));
                if (!t_off) break;
                uint64_t thunk = 0;
                if (!read_pod(img.raw, *t_off, thunk) || thunk == 0) break;

                PeImportSymbol sym;
                sym.iat_rva = static_cast<uint32_t>(iat_rva + stride);
                if (thunk & IMAGE_ORDINAL_FLAG64) {
                    sym.by_ordinal = true;
                    sym.ordinal = static_cast<uint16_t>(thunk & 0xFFFFu);
                } else {
                    sym.by_ordinal = false;
                    if (thunk > 0xFFFFFFFDull) {
                        return fail("Out-of-range import hint/name RVA");
                    }
                    const uint32_t hint_rva = static_cast<uint32_t>(thunk);
                    auto sname = img.read_cstring_at_rva(hint_rva + 2);
                    if (!sname) return fail("Unreadable import symbol name at RVA " + hex32(hint_rva));
                    sym.name = *sname;
                }
                mod.symbols.push_back(std::move(sym));
            }
            img.imports.push_back(std::move(mod));
        }
    }

    // Parse delay-load imports (IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT / ImgDelayDescr)
    const auto delay_dir = img.data_directories[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delay_dir.rva != 0 && delay_dir.size != 0) {
        for (uint32_t idx = 0;; ++idx) {
            const uint64_t desc_byte_off = static_cast<uint64_t>(idx) * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
            if (desc_byte_off + sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) > delay_dir.size) break;
            if (desc_byte_off > 0xFFFFFFFFull - delay_dir.rva) break;
            const uint32_t desc_rva = static_cast<uint32_t>(delay_dir.rva + desc_byte_off);
            auto desc_off = img.rva_to_file_offset(desc_rva);
            if (!desc_off) break;
            IMAGE_DELAYLOAD_DESCRIPTOR desc{};
            if (!read_pod(img.raw, *desc_off, desc)) break;
            if (desc.DllNameRVA == 0 && desc.ImportAddressTableRVA == 0 && desc.ImportNameTableRVA == 0) {
                break;
            }
            if (desc.DllNameRVA == 0) break;

            const bool is_rva_based = (desc.Attributes.AllAttributes & 1u) != 0;
            auto field_to_rva = [&](uint32_t val) -> std::optional<uint32_t> {
                if (val == 0) return std::nullopt;
                if (is_rva_based) return val;
                const uint64_t val64 = static_cast<uint64_t>(val);
                if (val64 >= img.image_base) {
                    const uint64_t diff = val64 - img.image_base;
                    if (diff <= 0xFFFFFFFFull) return static_cast<uint32_t>(diff);
                    return std::nullopt;
                }
                if (img.rva_to_file_offset(val).has_value()) return val;
                return std::nullopt;
            };
            auto thunk_to_rva = [&](uint64_t val) -> std::optional<uint32_t> {
                if (val == 0) return std::nullopt;
                if (is_rva_based) {
                    if (val > 0xFFFFFFFFull) return std::nullopt;
                    return static_cast<uint32_t>(val);
                }
                if (val >= img.image_base) {
                    const uint64_t diff = val - img.image_base;
                    if (diff <= 0xFFFFFFFFull) return static_cast<uint32_t>(diff);
                    return std::nullopt;
                }
                if (val <= 0xFFFFFFFFull && img.rva_to_file_offset(static_cast<uint32_t>(val)).has_value()) {
                    return static_cast<uint32_t>(val);
                }
                return std::nullopt;
            };

            auto dll_name_rva = field_to_rva(desc.DllNameRVA);
            if (!dll_name_rva) {
                return fail("Invalid delay-load import DLL name address: " + hex32(desc.DllNameRVA));
            }
            auto dll_name = img.read_cstring_at_rva(*dll_name_rva);
            if (!dll_name) {
                return fail("Unreadable delay-load import DLL name at RVA " + hex32(*dll_name_rva));
            }

            auto iat_opt = field_to_rva(desc.ImportAddressTableRVA);
            auto int_opt = field_to_rva(desc.ImportNameTableRVA);
            if (!iat_opt && !int_opt) continue;
            if (!iat_opt) {
                return fail("Delay-load import descriptor missing IAT for " + *dll_name);
            }
            const uint32_t iat_rva = *iat_opt;
            const uint32_t ilt_rva = int_opt.value_or(iat_rva);

            PeImportModule mod;
            mod.dll_name = *dll_name;
            mod.is_delay_load = true;

            for (uint32_t k = 0;; ++k) {
                const uint64_t stride = static_cast<uint64_t>(k) * 8u;
                if (stride > 0xFFFFFFFFull - ilt_rva || stride > 0xFFFFFFFFull - iat_rva) break;
                auto t_off = img.rva_to_file_offset(static_cast<uint32_t>(ilt_rva + stride));
                if (!t_off) break;
                uint64_t thunk = 0;
                if (!read_pod(img.raw, *t_off, thunk) || thunk == 0) break;

                PeImportSymbol sym;
                sym.iat_rva = static_cast<uint32_t>(iat_rva + stride);
                if (thunk & IMAGE_ORDINAL_FLAG64) {
                    sym.by_ordinal = true;
                    sym.ordinal = static_cast<uint16_t>(thunk & 0xFFFFu);
                } else {
                    sym.by_ordinal = false;
                    auto hint_rva = thunk_to_rva(thunk);
                    if (!hint_rva || *hint_rva > 0xFFFFFFFDu) {
                        return fail("Out-of-range delay-load import hint/name RVA in " + *dll_name);
                    }
                    auto sname = img.read_cstring_at_rva(*hint_rva + 2);
                    if (!sname) {
                        return fail("Unreadable delay-load import symbol name at RVA " + hex32(*hint_rva));
                    }
                    sym.name = *sname;
                }
                mod.symbols.push_back(std::move(sym));
            }
            img.imports.push_back(std::move(mod));
        }
    }

    // Parse exports
    const auto exp_dir = img.data_directories[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exp_dir.rva != 0 && exp_dir.size != 0) {
        auto exp_off = img.rva_to_file_offset(exp_dir.rva);
        IMAGE_EXPORT_DIRECTORY ed{};
        if (exp_off && read_pod(img.raw, *exp_off, ed) &&
            static_cast<uint64_t>(ed.NumberOfFunctions) * 4u <= std::max<uint64_t>(img.size_of_image, img.raw.size())) {
            std::vector<std::string> names_by_idx(ed.NumberOfFunctions);
            for (uint32_t i = 0; i < ed.NumberOfNames; ++i) {
                const uint64_t name_off_delta = static_cast<uint64_t>(i) * 4u;
                const uint64_t ord_off_delta = static_cast<uint64_t>(i) * 2u;
                if (name_off_delta > 0xFFFFFFFFull - ed.AddressOfNames ||
                    ord_off_delta > 0xFFFFFFFFull - ed.AddressOfNameOrdinals) {
                    break;
                }
                auto nrva_off = img.rva_to_file_offset(static_cast<uint32_t>(ed.AddressOfNames + name_off_delta));
                auto ord_off = img.rva_to_file_offset(static_cast<uint32_t>(ed.AddressOfNameOrdinals + ord_off_delta));
                uint32_t nrva = 0;
                uint16_t func_idx = 0;
                if (!nrva_off || !ord_off || !read_pod(img.raw, *nrva_off, nrva) || !read_pod(img.raw, *ord_off, func_idx)) {
                    continue;
                }
                if (func_idx < names_by_idx.size()) {
                    if (auto n = img.read_cstring_at_rva(nrva)) {
                        names_by_idx[func_idx] = *n;
                    }
                }
            }
            const uint64_t exp_end = static_cast<uint64_t>(exp_dir.rva) + exp_dir.size;
            for (uint32_t i = 0; i < ed.NumberOfFunctions; ++i) {
                const uint64_t fn_off_delta = static_cast<uint64_t>(i) * 4u;
                if (fn_off_delta > 0xFFFFFFFFull - ed.AddressOfFunctions) break;
                auto frva_off = img.rva_to_file_offset(static_cast<uint32_t>(ed.AddressOfFunctions + fn_off_delta));
                uint32_t frva = 0;
                if (!frva_off || !read_pod(img.raw, *frva_off, frva) || frva == 0) continue;
                PeExportSymbol es;
                es.ordinal = ed.Base + i;
                es.name = names_by_idx[i];
                es.rva = frva;
                if (frva >= exp_dir.rva && static_cast<uint64_t>(frva) < exp_end) {
                    if (auto fwd = img.read_cstring_at_rva(frva)) es.forwarder = *fwd;
                }
                img.exports.push_back(std::move(es));
            }
        }
    }

    return img;
}

Result<PeImage> parse_pe64_file(const std::filesystem::path& path) {
    auto bytes = read_file_bytes(path);
    if (!bytes) return std::unexpected(bytes.error());
    return parse_pe64(std::move(*bytes), path.filename().string());
}

}  // namespace kestrel::loader
