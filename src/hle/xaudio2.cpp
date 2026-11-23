#include "kestrel/hle/xaudio2.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
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

void set_xaudio2_wasapi_enabled(bool enabled) { g_wasapi_enabled.store(enabled, std::memory_order_relaxed); }

void register_xaudio2(Registry& reg) {
    for (const char* mod : {"xaudio2_9.dll", "xaudio2_9redist.dll", "xaudio2_8.dll"}) {
        reg.register_module(mod);
        reg.register_export(mod, "XAudio2Create", reinterpret_cast<void*>(&kx_XAudio2Create));
    }
}

XAudio2Stats xaudio2_stats() {
    std::lock_guard lock(g_audio_mtx);
    return {g_engines.load(), g_voices.load(), g_buffers.load(),
            g_frames_sub.load(), g_frames_con.load(), g_underruns.load(), g_last_rms};
}

void reset_xaudio2_stats() {
    g_engines = g_voices = g_buffers = g_frames_sub = g_frames_con = g_underruns = 0;
    std::lock_guard lock(g_audio_mtx);
    g_ring_read = g_ring_write = g_ring_count = 0;
    g_last_rms = 0.0;
}

}  // namespace kestrel::hle
