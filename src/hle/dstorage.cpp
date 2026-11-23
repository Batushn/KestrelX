#include "kestrel/hle/dstorage.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace kestrel::hle {

namespace {

vfs::OverlayVfs* g_vfs = nullptr;
std::atomic<uint32_t> g_files_opened{0};
std::atomic<uint32_t> g_queues_created{0};
std::atomic<uint32_t> g_requests_submitted{0};
std::atomic<uint64_t> g_bytes_read{0};
std::atomic<uint32_t> g_fences_signaled{0};

std::string wide_to_utf8(const wchar_t* wstr) {
    if (!wstr || !*wstr) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), n, nullptr, nullptr);
    return out;
}

class KxDStorageFileImpl final : public IKxDStorageFile {
public:
    explicit KxDStorageFileImpl(std::filesystem::path host_path, HANDLE handle)
        : host_path_(std::move(host_path)), handle_(handle) {}

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
        if (!request || !request->source_file || !request->destination_buffer) return;
        std::lock_guard lock(mtx_);
        request->source_file->AddRef();
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
            auto* file = static_cast<KxDStorageFileImpl*>(req.source_file);
            const uint32_t to_read = (std::min)(req.source_size, req.destination_size);
            uint32_t got = 0;
            if (file->read_at(req.source_offset, to_read, req.destination_buffer, got)) {
                g_requests_submitted.fetch_add(1, std::memory_order_relaxed);
                g_bytes_read.fetch_add(got, std::memory_order_relaxed);
            } else {
                error_count_.fetch_add(1, std::memory_order_relaxed);
            }
            file->Release();
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
    };
}

void reset_dstorage_stats() {
    g_files_opened = 0;
    g_queues_created = 0;
    g_requests_submitted = 0;
    g_bytes_read = 0;
    g_fences_signaled = 0;
}

void register_dstorage(Registry& reg) {
    for (const char* mod : {"dstorage.dll", "dstorage_xs.dll"}) {
        reg.register_module(mod);
        reg.register_export(mod, "DStorageGetFactory", reinterpret_cast<void*>(&kx_DStorageGetFactory));
        reg.register_export(mod, "DStorageSetConfiguration", reinterpret_cast<void*>(&kx_DStorageSetConfiguration));
    }
}

}  // namespace kestrel::hle
