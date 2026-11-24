#pragma once

#include "kestrel/hle/registry.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <unknwn.h>
#include <cstddef>
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

struct KxSendDescriptor {
    uint32_t flags = 0;
    void* output_voice = nullptr;  // IKxSubmixVoice* or IKxMasteringVoice*
};

struct KxVoiceSends {
    uint32_t send_count = 0;
    const KxSendDescriptor* sends = nullptr;
};

struct KxVoiceState {
    void* current_buffer_context = nullptr;
    uint32_t buffers_queued = 0;
    uint64_t samples_played = 0;
};

struct IKxMasteringVoice {
    virtual ~IKxMasteringVoice() = default;
    virtual HRESULT STDMETHODCALLTYPE SetVolume(float volume) = 0;
    virtual void STDMETHODCALLTYPE GetVolume(float* volume) = 0;
    virtual void STDMETHODCALLTYPE DestroyVoice() = 0;
    // Appended vtable slots (preserving guest ABI slots 0..3)
    virtual HRESULT STDMETHODCALLTYPE SetChannelVolumes(uint32_t channels, const float* volumes) = 0;
    virtual void STDMETHODCALLTYPE GetChannelVolumes(uint32_t channels, float* volumes) = 0;
};

struct IKxSubmixVoice {
    virtual ~IKxSubmixVoice() = default;
    virtual HRESULT STDMETHODCALLTYPE SetVolume(float volume) = 0;
    virtual void STDMETHODCALLTYPE GetVolume(float* volume) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetChannelVolumes(uint32_t channels, const float* volumes) = 0;
    virtual void STDMETHODCALLTYPE GetChannelVolumes(uint32_t channels, float* volumes) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOutputMatrix(
        void* dst_voice, uint32_t src_channels, uint32_t dst_channels, const float* level_matrix) = 0;
    virtual void STDMETHODCALLTYPE GetOutputMatrix(
        void* dst_voice, uint32_t src_channels, uint32_t dst_channels, float* level_matrix) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOutputVoices(const KxVoiceSends* send_list) = 0;
    virtual uint32_t STDMETHODCALLTYPE GetProcessingStage() const = 0;
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
    // Appended vtable slots (preserving guest ABI slots 0..6)
    virtual void STDMETHODCALLTYPE GetVolume(float* volume) = 0;
    virtual HRESULT STDMETHODCALLTYPE FlushSourceBuffers() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetFrequencyRatio(float ratio) = 0;
    virtual void STDMETHODCALLTYPE GetFrequencyRatio(float* ratio) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetChannelVolumes(uint32_t channels, const float* volumes) = 0;
    virtual void STDMETHODCALLTYPE GetChannelVolumes(uint32_t channels, float* volumes) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOutputMatrix(
        void* dst_voice, uint32_t src_channels, uint32_t dst_channels, const float* level_matrix) = 0;
    virtual void STDMETHODCALLTYPE GetOutputMatrix(
        void* dst_voice, uint32_t src_channels, uint32_t dst_channels, float* level_matrix) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOutputVoices(const KxVoiceSends* send_list) = 0;
    virtual void STDMETHODCALLTYPE GetState(KxVoiceState* state) = 0;
};

struct IKxXAudio2 : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateMasteringVoice(
        IKxMasteringVoice** out_voice, uint32_t channels = 2, uint32_t sample_rate = 48000) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSourceVoice(
        IKxSourceVoice** out_voice, const KxWaveFormat* fmt) = 0;
    virtual HRESULT STDMETHODCALLTYPE StartEngine() = 0;
    virtual void STDMETHODCALLTYPE StopEngine() = 0;
    // Appended vtable slots (preserving guest ABI slots 0..6)
    virtual HRESULT STDMETHODCALLTYPE CreateSubmixVoice(
        IKxSubmixVoice** out_voice,
        uint32_t input_channels = 2,
        uint32_t input_sample_rate = 48000,
        uint32_t flags = 0,
        uint32_t processing_stage = 0,
        const KxVoiceSends* send_list = nullptr) = 0;
};

// Clean-room X3DAudio foundation types & functions
struct KxVec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct KxX3DAudioHandle {
    uint8_t bytes[20]{};
};

constexpr uint32_t KxX3DAudioCalculateMatrix       = 0x00000001u;
constexpr uint32_t KxX3DAudioCalculateDelay        = 0x00000002u;
constexpr uint32_t KxX3DAudioCalculateLpfDirect    = 0x00000004u;
constexpr uint32_t KxX3DAudioCalculateLpfReverb    = 0x00000008u;
constexpr uint32_t KxX3DAudioCalculateReverb       = 0x00000010u;
constexpr uint32_t KxX3DAudioCalculateDoppler      = 0x00000020u;
constexpr uint32_t KxX3DAudioCalculateEmitterAngle = 0x00000040u;

constexpr uint32_t KxSpeakerFrontLeft        = 0x00000001u;
constexpr uint32_t KxSpeakerFrontRight       = 0x00000002u;
constexpr uint32_t KxSpeakerFrontCenter      = 0x00000004u;
constexpr uint32_t KxSpeakerLowFrequency     = 0x00000008u;
constexpr uint32_t KxSpeakerBackLeft         = 0x00000010u;
constexpr uint32_t KxSpeakerBackRight        = 0x00000020u;
constexpr uint32_t KxSpeakerSideLeft         = 0x00000200u;
constexpr uint32_t KxSpeakerSideRight        = 0x00000400u;
constexpr uint32_t KxSpeakerStereo           = KxSpeakerFrontLeft | KxSpeakerFrontRight;
constexpr uint32_t KxSpeakerQuad             = KxSpeakerFrontLeft | KxSpeakerFrontRight | KxSpeakerBackLeft | KxSpeakerBackRight;
constexpr uint32_t KxSpeaker5Point1          = KxSpeakerFrontLeft | KxSpeakerFrontRight | KxSpeakerFrontCenter | KxSpeakerLowFrequency | KxSpeakerBackLeft | KxSpeakerBackRight;
constexpr uint32_t KxSpeaker7Point1          = KxSpeaker5Point1 | KxSpeakerSideLeft | KxSpeakerSideRight;

struct KxX3DAudioListener {
    KxVec3 orient_front{0.0f, 0.0f, 1.0f};
    KxVec3 orient_top{0.0f, 1.0f, 0.0f};
    KxVec3 position{0.0f, 0.0f, 0.0f};
    KxVec3 velocity{0.0f, 0.0f, 0.0f};
};

struct KxX3DAudioEmitter {
    KxVec3 orient_front{0.0f, 0.0f, 1.0f};
    KxVec3 orient_top{0.0f, 1.0f, 0.0f};
    KxVec3 position{0.0f, 0.0f, 0.0f};
    KxVec3 velocity{0.0f, 0.0f, 0.0f};
    uint32_t channel_count = 1;
    float curve_distance_scaler = 1.0f;
    float doppler_scaler = 1.0f;
    float inner_radius = 0.0f;
    float inner_radius_angle = 0.0f;
};

struct KxX3DAudioDspSettings {
    float* matrix_coefficients = nullptr;
    float* delay_times = nullptr;
    uint32_t src_channel_count = 1;
    uint32_t dst_channel_count = 2;
    float lpf_direct_coefficient = 1.0f;
    float lpf_reverb_coefficient = 1.0f;
    float reverb_level = 0.0f;
    float doppler_factor = 1.0f;
    float emitter_to_listener_angle = 0.0f;
    float emitter_to_listener_distance = 0.0f;
    float emitter_velocity_component = 0.0f;
    float listener_velocity_component = 0.0f;
};

HRESULT __stdcall kx_X3DAudioInitialize(
    uint32_t speaker_channel_mask, float speed_of_sound, KxX3DAudioHandle* out_instance);
void __stdcall kx_X3DAudioCalculate(
    const KxX3DAudioHandle* instance,
    const KxX3DAudioListener* listener,
    const KxX3DAudioEmitter* emitter,
    uint32_t flags,
    KxX3DAudioDspSettings* dsp_settings);

struct XAudio2Stats {
    uint64_t engines_created = 0;
    uint64_t voices_created = 0;
    uint64_t buffers_submitted = 0;
    uint64_t frames_submitted = 0;
    uint64_t frames_consumed = 0;
    uint64_t underruns = 0;
    uint64_t wasapi_packets = 0;
    bool wasapi_active = false;
    double last_rms = 0.0;
    uint64_t submix_voices_created = 0;
    uint64_t x3daudio_calls = 0;
};

// Pull stereo float32 PCM frames from the software mixer (for WASAPI sink or headless verification).
size_t xaudio2_drain_stereo_f32(float* out_interleaved_stereo, size_t max_frames);
void set_xaudio2_wasapi_enabled(bool enabled);
void shutdown_xaudio2_wasapi();
void register_xaudio2(Registry& reg);
XAudio2Stats xaudio2_stats();
void reset_xaudio2_stats();

}  // namespace kestrel::hle
