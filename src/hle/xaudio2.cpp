#include "kestrel/hle/xaudio2.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace kestrel::hle {

namespace {

constexpr size_t kRingCapacityFrames = 96000;  // 2 seconds @ 48 kHz stereo

std::mutex g_audio_mtx;
std::vector<float> g_ring(kRingCapacityFrames * 2, 0.0f);
size_t g_ring_read = 0;
size_t g_ring_write = 0;
size_t g_ring_count = 0;

std::atomic<bool> g_wasapi_enabled{false};
std::atomic<bool> g_wasapi_stop{false};
std::atomic<bool> g_wasapi_active{false};
std::atomic<uint64_t> g_wasapi_packets{0};
std::mutex g_wasapi_mtx;
std::thread g_wasapi_thread;

std::atomic<uint64_t> g_engines{0};
std::atomic<uint64_t> g_voices{0};
std::atomic<uint64_t> g_buffers{0};
std::atomic<uint64_t> g_frames_sub{0};
std::atomic<uint64_t> g_frames_con{0};
std::atomic<uint64_t> g_underruns{0};
double g_last_rms = 0.0;

void ring_push_stereo(const float* stereo_frames, size_t frames, float gain) {
    std::lock_guard lock(g_audio_mtx);
    double sum_sq = 0.0;
    for (size_t i = 0; i < frames; ++i) {
        const float l = std::clamp(stereo_frames[i * 2 + 0] * gain, -1.0f, 1.0f);
        const float r = std::clamp(stereo_frames[i * 2 + 1] * gain, -1.0f, 1.0f);
        sum_sq += static_cast<double>(l) * l + static_cast<double>(r) * r;
        if (g_ring_count < kRingCapacityFrames) {
            g_ring[g_ring_write * 2 + 0] = l;
            g_ring[g_ring_write * 2 + 1] = r;
            g_ring_write = (g_ring_write + 1) % kRingCapacityFrames;
            ++g_ring_count;
        }
    }
    if (frames > 0) {
        g_last_rms = std::sqrt(sum_sq / static_cast<double>(frames * 2));
    }
}

void wasapi_render_loop() {
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_owned = SUCCEEDED(com_hr);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* mix_fmt = nullptr;

    auto cleanup = [&]() {
        g_wasapi_active.store(false, std::memory_order_relaxed);
        if (client) client->Stop();
        if (mix_fmt) CoTaskMemFree(mix_fmt);
        if (render) render->Release();
        if (client) client->Release();
        if (device) device->Release();
        if (enumerator) enumerator->Release();
        if (com_owned) CoUninitialize();
    };

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator))) || !enumerator) {
        cleanup();
        return;
    }
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || !device) {
        cleanup();
        return;
    }
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&client))) || !client) {
        cleanup();
        return;
    }
    if (FAILED(client->GetMixFormat(&mix_fmt)) || !mix_fmt) {
        cleanup();
        return;
    }

    constexpr REFERENCE_TIME kBufferDuration100ns = 200000;  // 20 ms
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBufferDuration100ns, 0, mix_fmt, nullptr))) {
        cleanup();
        return;
    }

    UINT32 buffer_frames = 0;
    if (FAILED(client->GetBufferSize(&buffer_frames)) || buffer_frames == 0) {
        cleanup();
        return;
    }
    if (FAILED(client->GetService(IID_PPV_ARGS(&render))) || !render) {
        cleanup();
        return;
    }
    if (FAILED(client->Start())) {
        cleanup();
        return;
    }

    g_wasapi_active.store(true, std::memory_order_relaxed);

    bool is_float = (mix_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (mix_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix_fmt->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix_fmt);
        is_float = (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0);
    }
    const UINT32 channels = std::max<UINT32>(mix_fmt->nChannels, 1u);
    std::vector<float> stereo_scratch(buffer_frames * 2, 0.0f);

    while (!g_wasapi_stop.load(std::memory_order_acquire)) {
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) break;
        const UINT32 avail = (buffer_frames > padding) ? (buffer_frames - padding) : 0;
        if (avail > 0) {
            BYTE* dst = nullptr;
            if (SUCCEEDED(render->GetBuffer(avail, &dst)) && dst) {
                const size_t got = xaudio2_drain_stereo_f32(stereo_scratch.data(), avail);
                if (got == 0) {
                    (void)render->ReleaseBuffer(avail, AUDCLNT_BUFFERFLAGS_SILENT);
                } else {
                    if (is_float && mix_fmt->wBitsPerSample == 32) {
                        auto* out = reinterpret_cast<float*>(dst);
                        for (UINT32 i = 0; i < avail; ++i) {
                            const float l = stereo_scratch[i * 2 + 0];
                            const float r = stereo_scratch[i * 2 + 1];
                            for (UINT32 c = 0; c < channels; ++c) {
                                out[i * channels + c] = (c == 0) ? l : (c == 1 ? r : 0.0f);
                            }
                        }
                    } else if (mix_fmt->wBitsPerSample == 16) {
                        auto* out = reinterpret_cast<int16_t*>(dst);
                        for (UINT32 i = 0; i < avail; ++i) {
                            const auto l = static_cast<int16_t>(std::clamp(stereo_scratch[i * 2 + 0], -1.0f, 1.0f) * 32767.0f);
                            const auto r = static_cast<int16_t>(std::clamp(stereo_scratch[i * 2 + 1], -1.0f, 1.0f) * 32767.0f);
                            for (UINT32 c = 0; c < channels; ++c) {
                                out[i * channels + c] = (c == 0) ? l : (c == 1 ? r : 0);
                            }
                        }
                    } else {
                        std::memset(dst, 0, static_cast<size_t>(avail) * mix_fmt->nBlockAlign);
                    }
                    (void)render->ReleaseBuffer(avail, 0);
                }
                g_wasapi_packets.fetch_add(1, std::memory_order_relaxed);
            }
        }
        Sleep(5);
    }

    cleanup();
}

void start_wasapi_thread_locked() {
    if (g_wasapi_thread.joinable()) return;
    g_wasapi_stop.store(false, std::memory_order_release);
    g_wasapi_thread = std::thread(wasapi_render_loop);
}

void stop_wasapi_thread_locked() {
    g_wasapi_stop.store(true, std::memory_order_release);
    if (g_wasapi_thread.joinable()) {
        g_wasapi_thread.join();
    }
}

class KxMasteringVoice final : public IKxMasteringVoice {
public:
    HRESULT STDMETHODCALLTYPE SetVolume(float v) override {
        volume_ = std::clamp(v, 0.0f, 4.0f);
        return S_OK;
    }
    void STDMETHODCALLTYPE GetVolume(float* v) override {
        if (v) *v = volume_;
    }
    void STDMETHODCALLTYPE DestroyVoice() override { delete this; }

private:
    float volume_ = 1.0f;
};

class KxSourceVoice final : public IKxSourceVoice {
public:
    explicit KxSourceVoice(const KxWaveFormat& fmt) : fmt_(fmt) {}

    HRESULT STDMETHODCALLTYPE Start(uint32_t) override {
        playing_ = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop(uint32_t) override {
        playing_ = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVolume(float v) override {
        volume_ = std::clamp(v, 0.0f, 4.0f);
        return S_OK;
    }
    uint32_t STDMETHODCALLTYPE GetBuffersQueued() override { return queued_.load(std::memory_order_relaxed); }
    void STDMETHODCALLTYPE DestroyVoice() override { delete this; }

    HRESULT STDMETHODCALLTYPE SubmitSourceBuffer(const KxAudioBuffer* buf) override {
        if (!buf || !buf->audio_data || buf->audio_bytes == 0) return E_INVALIDARG;
        const uint32_t ch = std::max<uint32_t>(fmt_.channels, 1u);
        const uint32_t bps = (fmt_.bits_per_sample == 16) ? 2u : 4u;
        const size_t frames = buf->audio_bytes / (ch * bps);
        if (frames == 0) return E_INVALIDARG;

        std::vector<float> stereo(frames * 2, 0.0f);
        if (fmt_.format_tag == 1 && fmt_.bits_per_sample == 16) {
            const auto* s16 = reinterpret_cast<const int16_t*>(buf->audio_data);
            for (size_t i = 0; i < frames; ++i) {
                const float l = static_cast<float>(s16[i * ch + 0]) / 32768.0f;
                const float r = (ch >= 2) ? (static_cast<float>(s16[i * ch + 1]) / 32768.0f) : l;
                stereo[i * 2 + 0] = l;
                stereo[i * 2 + 1] = r;
            }
        } else {
            const auto* f32 = reinterpret_cast<const float*>(buf->audio_data);
            for (size_t i = 0; i < frames; ++i) {
                const float l = f32[i * ch + 0];
                const float r = (ch >= 2) ? f32[i * ch + 1] : l;
                stereo[i * 2 + 0] = l;
                stereo[i * 2 + 1] = r;
            }
        }

        ring_push_stereo(stereo.data(), frames, volume_);
        g_buffers.fetch_add(1, std::memory_order_relaxed);
        g_frames_sub.fetch_add(frames, std::memory_order_relaxed);
        queued_.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }

private:
    KxWaveFormat fmt_{};
    bool playing_ = false;
    float volume_ = 1.0f;
    std::atomic<uint32_t> queued_{0};
};

class KxXAudio2 final : public IKxXAudio2 {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ref_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE CreateMasteringVoice(
        IKxMasteringVoice** out_voice, uint32_t, uint32_t) override {
        if (!out_voice) return E_POINTER;
        g_voices.fetch_add(1, std::memory_order_relaxed);
        if (g_wasapi_enabled.load(std::memory_order_relaxed)) {
            std::lock_guard lock(g_wasapi_mtx);
            start_wasapi_thread_locked();
        }
        *out_voice = new KxMasteringVoice();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateSourceVoice(
        IKxSourceVoice** out_voice, const KxWaveFormat* fmt) override {
        if (!out_voice || !fmt) return E_POINTER;
        g_voices.fetch_add(1, std::memory_order_relaxed);
        *out_voice = new KxSourceVoice(*fmt);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE StartEngine() override {
        running_ = true;
        return S_OK;
    }
    void STDMETHODCALLTYPE StopEngine() override { running_ = false; }

private:
    std::atomic<ULONG> ref_{1};
    bool running_ = true;
};

HRESULT __stdcall kx_XAudio2Create(IKxXAudio2** out_xa, uint32_t, uint32_t) {
    if (!out_xa) return E_POINTER;
    g_engines.fetch_add(1, std::memory_order_relaxed);
    *out_xa = new KxXAudio2();
    return S_OK;
}

}  // namespace

size_t xaudio2_drain_stereo_f32(float* out_interleaved_stereo, size_t max_frames) {
    if (!out_interleaved_stereo || max_frames == 0) return 0;
    std::lock_guard lock(g_audio_mtx);
    const size_t avail = std::min(g_ring_count, max_frames);
    for (size_t i = 0; i < avail; ++i) {
        out_interleaved_stereo[i * 2 + 0] = g_ring[g_ring_read * 2 + 0];
        out_interleaved_stereo[i * 2 + 1] = g_ring[g_ring_read * 2 + 1];
        g_ring_read = (g_ring_read + 1) % kRingCapacityFrames;
    }
    g_ring_count -= avail;
    if (avail < max_frames) {
        std::memset(out_interleaved_stereo + avail * 2, 0, (max_frames - avail) * 2 * sizeof(float));
        g_underruns.fetch_add(1, std::memory_order_relaxed);
    }
    g_frames_con.fetch_add(avail, std::memory_order_relaxed);
    return avail;
}

void set_xaudio2_wasapi_enabled(bool enabled) {
    g_wasapi_enabled.store(enabled, std::memory_order_relaxed);
    std::lock_guard lock(g_wasapi_mtx);
    if (enabled) start_wasapi_thread_locked();
    else stop_wasapi_thread_locked();
}

void shutdown_xaudio2_wasapi() {
    g_wasapi_enabled.store(false, std::memory_order_relaxed);
    std::lock_guard lock(g_wasapi_mtx);
    stop_wasapi_thread_locked();
}

void register_xaudio2(Registry& reg) {
    for (const char* mod : {"xaudio2_9.dll", "xaudio2_9redist.dll", "xaudio2_8.dll"}) {
        reg.register_module(mod);
        reg.register_export(mod, "XAudio2Create", reinterpret_cast<void*>(&kx_XAudio2Create));
    }
}

XAudio2Stats xaudio2_stats() {
    std::lock_guard lock(g_audio_mtx);
    return {g_engines.load(), g_voices.load(), g_buffers.load(),
            g_frames_sub.load(), g_frames_con.load(), g_underruns.load(),
            g_wasapi_packets.load(), g_wasapi_active.load(), g_last_rms};
}

void reset_xaudio2_stats() {
    shutdown_xaudio2_wasapi();
    g_engines = g_voices = g_buffers = g_frames_sub = g_frames_con = g_underruns = g_wasapi_packets = 0;
    std::lock_guard lock(g_audio_mtx);
    g_ring_read = g_ring_write = g_ring_count = 0;
    g_last_rms = 0.0;
}

}  // namespace kestrel::hle
