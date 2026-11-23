#pragma once

#include "kestrel/hle/registry.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <unknwn.h>
#include <cstdint>

namespace kestrel::hle {

struct KxWaveFormat {
    uint16_t format_tag = 3;  // 1 = PCM int16, 3 = IEEE float32
    uint16_t channels = 2;
    uint32_t sample_rate = 48000;
    uint16_t bits_per_sample = 32;
};

struct KxAudioBuffer {
    uint32_t flags = 0;
    uint32_t audio_bytes = 0;
    const uint8_t* audio_data = nullptr;
};

struct IKxMasteringVoice {
    virtual ~IKxMasteringVoice() = default;
    virtual HRESULT STDMETHODCALLTYPE SetVolume(float volume) = 0;
    virtual void STDMETHODCALLTYPE GetVolume(float* volume) = 0;
    virtual void STDMETHODCALLTYPE DestroyVoice() = 0;
};

struct IKxSourceVoice {
    virtual ~IKxSourceVoice() = default;
    virtual HRESULT STDMETHODCALLTYPE Start(uint32_t flags = 0) = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop(uint32_t flags = 0) = 0;
    virtual HRESULT STDMETHODCALLTYPE SubmitSourceBuffer(const KxAudioBuffer* buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetVolume(float volume) = 0;
    virtual uint32_t STDMETHODCALLTYPE GetBuffersQueued() = 0;
    virtual void STDMETHODCALLTYPE DestroyVoice() = 0;
};

struct IKxXAudio2 : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateMasteringVoice(
        IKxMasteringVoice** out_voice, uint32_t channels = 2, uint32_t sample_rate = 48000) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSourceVoice(
        IKxSourceVoice** out_voice, const KxWaveFormat* fmt) = 0;
    virtual HRESULT STDMETHODCALLTYPE StartEngine() = 0;
    virtual void STDMETHODCALLTYPE StopEngine() = 0;
};

struct XAudio2Stats {
    uint64_t engines_created = 0;
    uint64_t voices_created = 0;
    uint64_t buffers_submitted = 0;
    uint64_t frames_submitted = 0;
    uint64_t frames_consumed = 0;
    uint64_t underruns = 0;
    double last_rms = 0.0;
};

// Drain stereo float32 PCM frames from the internal ring buffer (for WASAPI sink or headless verification).
size_t xaudio2_drain_stereo_f32(float* out_interleaved_stereo, size_t max_frames);
void set_xaudio2_wasapi_enabled(bool enabled);
void register_xaudio2(Registry& reg);
XAudio2Stats xaudio2_stats();
void reset_xaudio2_stats();

}  // namespace kestrel::hle
