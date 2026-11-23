#pragma once

#include "kestrel/hle/registry.hpp"
#include "kestrel/vfs/overlay_vfs.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d12.h>
#include <unknwn.h>

#include <cstdint>

namespace kestrel::hle {

struct DStorageStats {
    uint32_t files_opened = 0;
    uint32_t queues_created = 0;
    uint32_t requests_submitted = 0;
    uint64_t bytes_read = 0;
    uint32_t fences_signaled = 0;
};

struct KxDStorageQueueDesc {
    uint32_t source_type = 0;   // 0 = File
    uint16_t capacity = 64;
    int8_t priority = 0;
    const char* name = nullptr;
    ID3D12Device* device = nullptr;
};

struct IKxDStorageFile : public IUnknown {
    virtual void STDMETHODCALLTYPE Close() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFileInformation(BY_HANDLE_FILE_INFORMATION* info) = 0;
};

struct KxDStorageRequest {
    IKxDStorageFile* source_file = nullptr;
    uint64_t source_offset = 0;
    uint32_t source_size = 0;
    void* destination_buffer = nullptr;
    uint32_t destination_size = 0;
    uint64_t tag = 0;
};

struct IKxDStorageQueue : public IUnknown {
    virtual void STDMETHODCALLTYPE EnqueueRequest(const KxDStorageRequest* request) = 0;
    virtual void STDMETHODCALLTYPE EnqueueSignal(ID3D12Fence* fence, uint64_t value) = 0;
    virtual void STDMETHODCALLTYPE Submit() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetErrorCount() = 0;
};

struct IKxDStorageFactory : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateQueue(const KxDStorageQueueDesc* desc, REFIID riid, void** ppv) = 0;
    virtual HRESULT STDMETHODCALLTYPE OpenFile(const wchar_t* path, REFIID riid, void** ppv) = 0;
};

void set_dstorage_vfs(vfs::OverlayVfs* vfs);
void register_dstorage(Registry& reg);
DStorageStats dstorage_stats();
void reset_dstorage_stats();

HRESULT __stdcall kx_DStorageGetFactory(REFIID riid, void** ppv);
HRESULT __stdcall kx_DStorageSetConfiguration(const void* config);

}  // namespace kestrel::hle
