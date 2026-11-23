#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <cstdint>

extern "C" {

// Clean-room companion guest DLL exports
#ifdef KESTREL_GUEST_DLL_EXPORTS
#define KESTREL_GUEST_API __declspec(dllexport)
#else
#define KESTREL_GUEST_API __declspec(dllimport)
#endif

KESTREL_GUEST_API uint32_t __cdecl kestrel_guest_dll_compute(uint32_t x);
KESTREL_GUEST_API uint32_t __cdecl kestrel_guest_dll_attached();

// Synthetic Xbox console imports resolved by KestrelX HLE
struct KxTitleMemoryStatus {
    uint32_t length;
    uint32_t reserved;
    uint64_t total_mem;
    uint64_t avail_mem;
    uint64_t legacy_used;
    uint64_t legacy_peak;
    uint64_t legacy_avail;
    uint64_t title_used;
    uint64_t title_avail;
};

__declspec(dllimport) void* __stdcall XMemAlloc(size_t size, uint64_t attributes);
__declspec(dllimport) void __stdcall XMemFree(void* ptr, uint64_t attributes);
__declspec(dllimport) size_t __stdcall XMemSize(void* ptr, uint64_t attributes);
__declspec(dllimport) BOOL __stdcall TitleMemoryStatus(KxTitleMemoryStatus* out);
__declspec(dllimport) uint32_t __stdcall GetConsoleType();

// Resolved from kernelx.dll via explicit import alias (CreateFileA is in kernelx.def)
// Note: We import kernelx!CreateFileA via a dedicated declaration to avoid colliding with kernel32!CreateFileA.
__declspec(dllimport) HRESULT __stdcall XGameGetXboxTitleId(uint32_t* out_id);
__declspec(dllimport) uint32_t __stdcall XSystemGetDeviceType();
using KxTaskCallback = void(__stdcall*)(void* context, bool canceled);
__declspec(dllimport) HRESULT __stdcall XTaskQueueCreate(uint32_t work_mode, uint32_t comp_mode, void** out_queue);
__declspec(dllimport) HRESULT __stdcall XTaskQueueSubmitCallback(void* queue, uint32_t port, void* ctx, KxTaskCallback cb);
__declspec(dllimport) bool __stdcall XTaskQueueDispatch(void* queue, uint32_t port, uint32_t timeout_ms);
__declspec(dllimport) void __stdcall XTaskQueueCloseHandle(void* queue);

__declspec(dllimport) HRESULT __stdcall D3D12XboxCreateDevice(const void* params, REFIID riid, void** out_dev);
__declspec(dllimport) void __stdcall D3D12ZeroMemoryX(void* dst, size_t size);
__declspec(dllimport) HRESULT __stdcall D3D12PresentX(ID3D12CommandQueue* queue, uint32_t flags, const void* present_params, const void* flip_params);
__declspec(dllimport) HRESULT __stdcall D3D12WaitFrameEventX(uint64_t token, uint32_t timeout_ms, void* out_event);

// Clean-room GameInput guest ABI
struct GuestGamepadState {
    uint32_t buttons;
    float left_trigger;
    float right_trigger;
    float left_thumbstick_x;
    float left_thumbstick_y;
    float right_thumbstick_x;
    float right_thumbstick_y;
};

struct IGuestGameInputReading : public IUnknown {
    virtual uint32_t STDMETHODCALLTYPE GetInputKind() = 0;
    virtual uint64_t STDMETHODCALLTYPE GetTimestamp() = 0;
    virtual bool STDMETHODCALLTYPE GetGamepadState(GuestGamepadState* state) = 0;
};

struct IGuestGameInput : public IUnknown {
    virtual uint64_t STDMETHODCALLTYPE GetCurrentTimestamp() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentReading(uint32_t kind, void* dev, IGuestGameInputReading** out) = 0;
};

__declspec(dllimport) HRESULT __stdcall GameInputCreate(IGuestGameInput** out_gi);

// Clean-room XAudio2 guest ABI
struct GuestWaveFormat {
    uint16_t format_tag;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
};

struct GuestAudioBuffer {
    uint32_t flags;
    uint32_t audio_bytes;
    const uint8_t* audio_data;
};

struct IGuestMasteringVoice {
    virtual ~IGuestMasteringVoice() = default;
    virtual HRESULT STDMETHODCALLTYPE SetVolume(float volume) = 0;
    virtual void STDMETHODCALLTYPE GetVolume(float* volume) = 0;
    virtual void STDMETHODCALLTYPE DestroyVoice() = 0;
};

struct IGuestSourceVoice {
    virtual ~IGuestSourceVoice() = default;
    virtual HRESULT STDMETHODCALLTYPE Start(uint32_t flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop(uint32_t flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE SubmitSourceBuffer(const GuestAudioBuffer* buf) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetVolume(float volume) = 0;
    virtual uint32_t STDMETHODCALLTYPE GetBuffersQueued() = 0;
    virtual void STDMETHODCALLTYPE DestroyVoice() = 0;
};

struct IGuestXAudio2 : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateMasteringVoice(IGuestMasteringVoice** out, uint32_t ch, uint32_t sr) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSourceVoice(IGuestSourceVoice** out, const GuestWaveFormat* fmt) = 0;
    virtual HRESULT STDMETHODCALLTYPE StartEngine() = 0;
    virtual void STDMETHODCALLTYPE StopEngine() = 0;
};

__declspec(dllimport) HRESULT __stdcall XAudio2Create(IGuestXAudio2** out_xa, uint32_t flags, uint32_t proc);

}  // extern "C"
