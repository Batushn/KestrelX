#include "kestrel/loader/pe_image.hpp"

#include <Windows.h>
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
            const uint32_t desc_rva = imp_dir.rva + idx * sizeof(IMAGE_IMPORT_DESCRIPTOR);
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
                auto t_off = img.rva_to_file_offset(ilt_rva + k * 8);
                if (!t_off) break;
                uint64_t thunk = 0;
                if (!read_pod(img.raw, *t_off, thunk) || thunk == 0) break;

                PeImportSymbol sym;
                sym.iat_rva = iat_rva + k * 8;
                if (thunk & IMAGE_ORDINAL_FLAG64) {
                    sym.by_ordinal = true;
                    sym.ordinal = static_cast<uint16_t>(thunk & 0xFFFF);
                } else {
                    sym.by_ordinal = false;
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

    // Parse exports
    const auto exp_dir = img.data_directories[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exp_dir.rva != 0 && exp_dir.size != 0) {
        auto exp_off = img.rva_to_file_offset(exp_dir.rva);
        IMAGE_EXPORT_DIRECTORY ed{};
        if (exp_off && read_pod(img.raw, *exp_off, ed)) {
            std::vector<std::string> names_by_idx(ed.NumberOfFunctions);
            for (uint32_t i = 0; i < ed.NumberOfNames; ++i) {
                auto nrva_off = img.rva_to_file_offset(ed.AddressOfNames + i * 4);
                auto ord_off = img.rva_to_file_offset(ed.AddressOfNameOrdinals + i * 2);
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
            for (uint32_t i = 0; i < ed.NumberOfFunctions; ++i) {
                auto frva_off = img.rva_to_file_offset(ed.AddressOfFunctions + i * 4);
                uint32_t frva = 0;
                if (!frva_off || !read_pod(img.raw, *frva_off, frva) || frva == 0) continue;
                PeExportSymbol es;
                es.ordinal = ed.Base + i;
                es.name = names_by_idx[i];
                es.rva = frva;
                if (frva >= exp_dir.rva && frva < exp_dir.rva + exp_dir.size) {
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
