#include "kestrel/hle/dstorage.hpp"

#include <windows.h>
#include <compressapi.h>
#pragma comment(lib, "cabinet.lib")

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace kestrel::hle {

namespace {

vfs::OverlayVfs* g_vfs = nullptr;

std::atomic<uint32_t> g_files_opened{0};
std::atomic<uint32_t> g_queues_created{0};
std::atomic<uint32_t> g_requests_submitted{0};
std::atomic<uint64_t> g_bytes_read{0};
std::atomic<uint32_t> g_fences_signaled{0};
std::atomic<uint32_t> g_gdeflate_requests{0};
std::atomic<uint64_t> g_gdeflate_decompressed_bytes{0};
std::atomic<uint32_t> g_gpu_buffer_destinations{0};

std::string wide_to_utf8(const wchar_t* wstr) {
    if (!wstr) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), len - 1, nullptr, nullptr);
    return out;
}

class KxDStorageFileImpl final : public IKxDStorageFile {
public:
    KxDStorageFileImpl(std::filesystem::path host_path, HANDLE h)
        : host_path_(std::move(host_path)), handle_(h) {}

    ~KxDStorageFileImpl() { Close(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown) {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = this;
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ref_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG c = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0) delete this;
        return c;
    }

    void STDMETHODCALLTYPE Close() override {
        std::lock_guard lock(mtx_);
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    HRESULT STDMETHODCALLTYPE GetFileInformation(BY_HANDLE_FILE_INFORMATION* info) override {
        if (!info) return E_INVALIDARG;
        std::lock_guard lock(mtx_);
        if (handle_ == INVALID_HANDLE_VALUE) return E_HANDLE;
        return GetFileInformationByHandle(handle_, info) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }

    bool read_at(uint64_t offset, uint32_t size, void* dst, uint32_t& out_read) {
        out_read = 0;
        std::lock_guard lock(mtx_);
        if (handle_ == INVALID_HANDLE_VALUE || !dst) return false;
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle_, pos, nullptr, FILE_BEGIN)) return false;
        DWORD got = 0;
        if (!ReadFile(handle_, dst, size, &got, nullptr)) return false;
        out_read = static_cast<uint32_t>(got);
        return true;
    }

private:
    std::atomic<ULONG> ref_{1};
    std::mutex mtx_;
    std::filesystem::path host_path_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

class KxDStorageQueueImpl final : public IKxDStorageQueue {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown) {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = this;
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ref_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG c = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0) delete this;
        return c;
    }

    void STDMETHODCALLTYPE EnqueueRequest(const KxDStorageRequest* request) override {
        if (!request) return;
        std::lock_guard lock(mtx_);
        if (request->source_file) {
            request->source_file->AddRef();
        }
        if (request->destination_resource) {
            request->destination_resource->AddRef();
        }
        pending_reads_.push_back(*request);
    }

    void STDMETHODCALLTYPE EnqueueSignal(ID3D12Fence* fence, uint64_t value) override {
        if (!fence) return;
        std::lock_guard lock(mtx_);
        fence->AddRef();
        pending_signals_.push_back({fence, value});
    }

    void STDMETHODCALLTYPE Submit() override {
        std::vector<KxDStorageRequest> reads;
        std::vector<std::pair<ID3D12Fence*, uint64_t>> signals;
        {
            std::lock_guard lock(mtx_);
            reads.swap(pending_reads_);
            signals.swap(pending_signals_);
        }

        for (const auto& req : reads) {
            std::vector<uint8_t> raw_input;
            const uint8_t* in_ptr = nullptr;
            size_t in_size = 0;

            if (req.source_file) {
                auto* file = static_cast<KxDStorageFileImpl*>(req.source_file);
                raw_input.resize(req.source_size);
                uint32_t got = 0;
                if (file->read_at(req.source_offset, req.source_size, raw_input.data(), got)) {
                    raw_input.resize(got);
                    in_ptr = raw_input.data();
                    in_size = got;
                } else {
                    error_count_.fetch_add(1, std::memory_order_relaxed);
                }
                file->Release();
            } else if (req.source_buffer) {
                in_ptr = static_cast<const uint8_t*>(req.source_buffer) + req.source_offset;
                in_size = req.source_size;
            }

            if (in_ptr && in_size > 0) {
                const void* final_data = in_ptr;
                size_t final_size = in_size;
                std::vector<uint8_t> decompressed;

                if (req.compression_format == DStorageCompressionFormat::GDeflate) {
                    const size_t target_unpacked = req.uncompressed_size > 0 ? req.uncompressed_size : req.destination_size;
                    decompressed.resize(target_unpacked);
                    size_t actual_out = 0;
                    if (SUCCEEDED(decompress_gdeflate(in_ptr, in_size, decompressed.data(), decompressed.size(), actual_out))) {
                        decompressed.resize(actual_out);
                        final_data = decompressed.data();
                        final_size = actual_out;
                        g_gdeflate_requests.fetch_add(1, std::memory_order_relaxed);
                        g_gdeflate_decompressed_bytes.fetch_add(actual_out, std::memory_order_relaxed);
                    } else {
                        error_count_.fetch_add(1, std::memory_order_relaxed);
                    }
                }

                const size_t to_write = (std::min)(final_size, static_cast<size_t>(req.destination_size));

                if (req.destination_type == DStorageRequestDestinationType::Buffer && req.destination_resource) {
                    D3D12_RANGE read_range{0, 0};
                    void* mapped = nullptr;
                    if (SUCCEEDED(req.destination_resource->Map(0, &read_range, &mapped)) && mapped) {
                        uint8_t* dst_ptr = static_cast<uint8_t*>(mapped) + req.destination_resource_offset;
                        std::memcpy(dst_ptr, final_data, to_write);
                        D3D12_RANGE write_range{static_cast<SIZE_T>(req.destination_resource_offset),
                                                static_cast<SIZE_T>(req.destination_resource_offset + to_write)};
                        req.destination_resource->Unmap(0, &write_range);
                        g_gpu_buffer_destinations.fetch_add(1, std::memory_order_relaxed);
                        g_requests_submitted.fetch_add(1, std::memory_order_relaxed);
                        g_bytes_read.fetch_add(to_write, std::memory_order_relaxed);
                    } else {
                        error_count_.fetch_add(1, std::memory_order_relaxed);
                    }
                } else if (req.destination_buffer) {
                    std::memcpy(req.destination_buffer, final_data, to_write);
                    g_requests_submitted.fetch_add(1, std::memory_order_relaxed);
                    g_bytes_read.fetch_add(to_write, std::memory_order_relaxed);
                }
            } else {
                error_count_.fetch_add(1, std::memory_order_relaxed);
            }

            if (req.destination_resource) {
                req.destination_resource->Release();
            }
        }

        for (const auto& [fence, val] : signals) {
            if (SUCCEEDED(fence->Signal(val))) {
                g_fences_signaled.fetch_add(1, std::memory_order_relaxed);
            } else {
                error_count_.fetch_add(1, std::memory_order_relaxed);
            }
            fence->Release();
        }
    }

    HRESULT STDMETHODCALLTYPE GetErrorCount() override {
        return static_cast<HRESULT>(error_count_.load(std::memory_order_relaxed));
    }

private:
    std::atomic<ULONG> ref_{1};
    std::mutex mtx_;
    std::vector<KxDStorageRequest> pending_reads_;
    std::vector<std::pair<ID3D12Fence*, uint64_t>> pending_signals_;
    std::atomic<uint32_t> error_count_{0};
};

class KxDStorageFactoryImpl final : public IKxDStorageFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown) {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = this;
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ref_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG c = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0) delete this;
        return c;
    }

    HRESULT STDMETHODCALLTYPE CreateQueue(const KxDStorageQueueDesc*, REFIID, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = new KxDStorageQueueImpl();
        g_queues_created.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OpenFile(const wchar_t* path, REFIID, void** ppv) override {
        if (!path || !ppv) return E_INVALIDARG;
        *ppv = nullptr;

        std::filesystem::path host_path;
        if (g_vfs) {
            auto resolved = g_vfs->resolve(wide_to_utf8(path), false);
            if (!resolved) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            host_path = *resolved;
        } else {
            host_path = std::filesystem::path(path);
        }

        HANDLE h = CreateFileW(
            host_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        *ppv = new KxDStorageFileImpl(std::move(host_path), h);
        g_files_opened.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_{1};
};

}  // namespace

HRESULT decompress_gdeflate(const void* src, size_t src_size, void* dst, size_t dst_size, size_t& out_unpacked) {
    out_unpacked = 0;
    if (!src || src_size == 0 || !dst || dst_size == 0) return E_INVALIDARG;

    const uint8_t* in_ptr = static_cast<const uint8_t*>(src);
    uint8_t* out_ptr = static_cast<uint8_t*>(dst);

    // 1. Check for RFC 1951 uncompressed block (BFINAL=1, BTYPE=00)
    // Format: 0x01, LEN (u16), NLEN (u16), payload[LEN]
    if (src_size >= 5 && (in_ptr[0] & 0x07) == 0x01) {
        const uint16_t len = static_cast<uint16_t>(in_ptr[1] | (in_ptr[2] << 8));
        const uint16_t nlen = static_cast<uint16_t>(in_ptr[3] | (in_ptr[4] << 8));
        if ((len ^ nlen) == 0xFFFF && src_size >= 5 + len) {
            const size_t to_copy = (std::min)(static_cast<size_t>(len), dst_size);
            std::memcpy(out_ptr, in_ptr + 5, to_copy);
            out_unpacked = to_copy;
            return S_OK;
        }
    }

    // 2. Check for MSZIP/DEFLATE stream decompression via cabinet.dll Compress API
    std::vector<uint8_t> mszip_buf;
    const uint8_t* comp_input = in_ptr;
    size_t comp_size = src_size;

    if (!(src_size >= 2 && in_ptr[0] == 0x43 && in_ptr[1] == 0x4B)) {
        mszip_buf.resize(src_size + 2);
        mszip_buf[0] = 0x43;
        mszip_buf[1] = 0x4B;
        std::memcpy(mszip_buf.data() + 2, in_ptr, src_size);
        comp_input = mszip_buf.data();
        comp_size = mszip_buf.size();
    }

    DECOMPRESSOR_HANDLE decomp = nullptr;
    if (CreateDecompressor(COMPRESS_ALGORITHM_MSZIP, nullptr, &decomp)) {
        SIZE_T actual = 0;
        if (Decompress(decomp, const_cast<uint8_t*>(comp_input), comp_size, out_ptr, dst_size, &actual)) {
            CloseDecompressor(decomp);
            out_unpacked = static_cast<size_t>(actual);
            return S_OK;
        }
        CloseDecompressor(decomp);
    }

    // 3. Fallback: transparent uncompressed pass-through
    const size_t direct_copy = (std::min)(src_size, dst_size);
    std::memcpy(out_ptr, in_ptr, direct_copy);
    out_unpacked = direct_copy;
    return S_OK;
}

HRESULT __stdcall kx_DStorageGetFactory(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    auto* factory = new KxDStorageFactoryImpl();
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

HRESULT __stdcall kx_DStorageSetConfiguration(const void*) {
    return S_OK;
}

void set_dstorage_vfs(vfs::OverlayVfs* vfs) { g_vfs = vfs; }

DStorageStats dstorage_stats() {
    return {
        g_files_opened.load(std::memory_order_relaxed),
        g_queues_created.load(std::memory_order_relaxed),
        g_requests_submitted.load(std::memory_order_relaxed),
        g_bytes_read.load(std::memory_order_relaxed),
        g_fences_signaled.load(std::memory_order_relaxed),
        g_gdeflate_requests.load(std::memory_order_relaxed),
        g_gdeflate_decompressed_bytes.load(std::memory_order_relaxed),
        g_gpu_buffer_destinations.load(std::memory_order_relaxed),
    };
}

void reset_dstorage_stats() {
    g_files_opened = 0;
    g_queues_created = 0;
    g_requests_submitted = 0;
    g_bytes_read = 0;
    g_fences_signaled = 0;
    g_gdeflate_requests = 0;
    g_gdeflate_decompressed_bytes = 0;
    g_gpu_buffer_destinations = 0;
}

void register_dstorage(Registry& reg) {
    for (const char* mod : {"dstorage.dll", "dstorage_xs.dll"}) {
        reg.register_module(mod);
        reg.register_export(mod, "DStorageGetFactory", reinterpret_cast<void*>(&kx_DStorageGetFactory));
        reg.register_export(mod, "DStorageSetConfiguration", reinterpret_cast<void*>(&kx_DStorageSetConfiguration));
    }
}

}  // namespace kestrel::hle
