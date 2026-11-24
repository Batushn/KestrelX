#include "kestrel/hle/xaudio2.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kestrel::hle {

namespace {

constexpr uint32_t kDefaultSampleRate = 48000;
constexpr uint32_t kMixerChannels = 2;
constexpr float kMinFreqRatio = 1.0f / 1024.0f;
constexpr float kMaxFreqRatio = 1024.0f;
constexpr float kPi = 3.14159265358979323846f;

class KxMasteringVoice;
class KxSubmixVoice;
class KxSourceVoice;

struct EngineState {
    std::atomic<bool> running{true};
    KxMasteringVoice* mastering = nullptr;
};

struct OutputMatrix {
    uint32_t src_channels = 0;
    uint32_t dst_channels = 0;
    std::vector<float> coeffs;  // row-major: coeffs[dst * src_channels + src]
};

struct QueuedBuffer {
    std::vector<float> samples;  // interleaved [frame * channels + ch]
    uint32_t channels = 2;
    size_t frames = 0;
    uint32_t flags = 0;
};

std::mutex g_audio_mtx;
std::vector<KxMasteringVoice*> g_mastering_voices;
std::vector<KxSubmixVoice*> g_submix_voices;
std::vector<KxSourceVoice*> g_source_voices;

std::atomic<bool> g_wasapi_enabled{false};
std::atomic<bool> g_wasapi_stop{false};
std::atomic<bool> g_wasapi_active{false};
std::atomic<uint64_t> g_wasapi_packets{0};
std::mutex g_wasapi_mtx;
std::thread g_wasapi_thread;

std::atomic<uint64_t> g_engines{0};
std::atomic<uint64_t> g_voices{0};
std::atomic<uint64_t> g_submix_voices_created{0};
std::atomic<uint64_t> g_buffers{0};
std::atomic<uint64_t> g_frames_sub{0};
std::atomic<uint64_t> g_frames_con{0};
std::atomic<uint64_t> g_underruns{0};
std::atomic<uint64_t> g_x3daudio_calls{0};
double g_last_rms = 0.0;

const OutputMatrix* find_matrix_locked(
    const std::unordered_map<void*, OutputMatrix>& matrices,
    void* dst_key,
    uint32_t src_ch,
    uint32_t dst_ch) {
    auto match = [&](void* key) -> const OutputMatrix* {
        auto it = matrices.find(key);
        if (it == matrices.end()) return nullptr;
        const auto& m = it->second;
        if (m.src_channels == src_ch && m.dst_channels == dst_ch &&
            m.coeffs.size() == static_cast<size_t>(src_ch) * dst_ch) {
            return &m;
        }
        return nullptr;
    };
    if (const auto* exact = match(dst_key)) return exact;
    return match(nullptr);
}

void apply_matrix_accumulate(
    const float* src_interleaved,
    size_t frames,
    uint32_t src_ch,
    float voice_volume,
    const std::vector<float>& channel_volumes,
    const OutputMatrix* explicit_matrix,
    float* dst_interleaved,
    uint32_t dst_ch) {
    if (frames == 0 || src_ch == 0 || dst_ch == 0) return;

    auto ch_gain = [&](uint32_t s) -> float {
        const float cv = (s < channel_volumes.size()) ? channel_volumes[s] : 1.0f;
        return cv * voice_volume;
    };

    if (explicit_matrix) {
        const auto& coeffs = explicit_matrix->coeffs;
        for (size_t i = 0; i < frames; ++i) {
            const float* in_frame = src_interleaved + i * src_ch;
            float* out_frame = dst_interleaved + i * dst_ch;
            for (uint32_t d = 0; d < dst_ch; ++d) {
                float acc = 0.0f;
                const size_t row = static_cast<size_t>(d) * src_ch;
                for (uint32_t s = 0; s < src_ch; ++s) {
                    acc += in_frame[s] * ch_gain(s) * coeffs[row + s];
                }
                out_frame[d] += acc;
            }
        }
        return;
    }

    if (src_ch == 1) {
        const float g = ch_gain(0);
        for (size_t i = 0; i < frames; ++i) {
            const float v = src_interleaved[i] * g;
            float* out_frame = dst_interleaved + i * dst_ch;
            for (uint32_t d = 0; d < dst_ch; ++d) {
                out_frame[d] += v;
            }
        }
    } else if (src_ch == 6 && dst_ch == 2) {
        // ITU-R BS.775 5.1 -> Stereo downmix
        // Channel layout: 0:FL, 1:FR, 2:FC, 3:LFE, 4:BL, 5:BR
        constexpr float kWeight = 0.70710678f;
        for (size_t i = 0; i < frames; ++i) {
            const float* in_frame = src_interleaved + i * 6;
            float* out_frame = dst_interleaved + i * 2;
            const float fl = in_frame[0] * ch_gain(0);
            const float fr = in_frame[1] * ch_gain(1);
            const float fc = in_frame[2] * ch_gain(2);
            // LFE (in_frame[3]) is omitted per ITU-R BS.775 to avoid subwoofer overload
            const float bl = in_frame[4] * ch_gain(4);
            const float br = in_frame[5] * ch_gain(5);
            out_frame[0] += fl + kWeight * fc + kWeight * bl;
            out_frame[1] += fr + kWeight * fc + kWeight * br;
        }
    } else if (src_ch == 8 && dst_ch == 2) {
        // ITU-R BS.775 7.1 -> Stereo downmix
        // Channel layout: 0:FL, 1:FR, 2:FC, 3:LFE, 4:BL, 5:BR, 6:SL, 7:SR
        constexpr float kWeight = 0.70710678f;
        for (size_t i = 0; i < frames; ++i) {
            const float* in_frame = src_interleaved + i * 8;
            float* out_frame = dst_interleaved + i * 2;
            const float fl = in_frame[0] * ch_gain(0);
            const float fr = in_frame[1] * ch_gain(1);
            const float fc = in_frame[2] * ch_gain(2);
            const float bl = in_frame[4] * ch_gain(4);
            const float br = in_frame[5] * ch_gain(5);
            const float sl = in_frame[6] * ch_gain(6);
            const float sr = in_frame[7] * ch_gain(7);
            out_frame[0] += fl + kWeight * fc + kWeight * (bl + sl);
            out_frame[1] += fr + kWeight * fc + kWeight * (br + sr);
        }
    } else {
        const uint32_t common = std::min(src_ch, dst_ch);
        for (size_t i = 0; i < frames; ++i) {
            const float* in_frame = src_interleaved + i * src_ch;
            float* out_frame = dst_interleaved + i * dst_ch;
            for (uint32_t c = 0; c < common; ++c) {
                out_frame[c] += in_frame[c] * ch_gain(c);
            }
        }
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

void detach_destination_from_all_voices_locked(void* dst_ptr);

class KxMasteringVoice final : public IKxMasteringVoice {
public:
    KxMasteringVoice(std::shared_ptr<EngineState> engine_state, uint32_t channels, uint32_t sample_rate)
        : engine_state_(std::move(engine_state)),
          channels_(channels > 0 ? channels : kMixerChannels),
          sample_rate_(sample_rate > 0 ? sample_rate : kDefaultSampleRate),
          channel_volumes_(kMixerChannels, 1.0f) {
        std::lock_guard lock(g_audio_mtx);
        g_mastering_voices.push_back(this);
        if (engine_state_) engine_state_->mastering = this;
    }

    HRESULT STDMETHODCALLTYPE SetVolume(float v) override {
        std::lock_guard lock(g_audio_mtx);
        volume_ = std::clamp(v, 0.0f, 4.0f);
        return S_OK;
    }

    void STDMETHODCALLTYPE GetVolume(float* v) override {
        if (!v) return;
        std::lock_guard lock(g_audio_mtx);
        *v = volume_;
    }

    void STDMETHODCALLTYPE DestroyVoice() override {
        {
            std::lock_guard lock(g_audio_mtx);
            void* self_key = static_cast<void*>(static_cast<IKxMasteringVoice*>(this));
            detach_destination_from_all_voices_locked(self_key);
            if (engine_state_ && engine_state_->mastering == this) {
                engine_state_->mastering = nullptr;
            }
            std::erase(g_mastering_voices, this);
        }
        delete this;
    }

    HRESULT STDMETHODCALLTYPE SetChannelVolumes(uint32_t channels, const float* volumes) override {
        if (!volumes || channels == 0) return E_INVALIDARG;
        std::lock_guard lock(g_audio_mtx);
        channel_volumes_.resize(std::max<uint32_t>(channels, kMixerChannels), 1.0f);
        for (uint32_t c = 0; c < channels; ++c) {
            channel_volumes_[c] = std::clamp(volumes[c], 0.0f, 4.0f);
        }
        return S_OK;
    }

    void STDMETHODCALLTYPE GetChannelVolumes(uint32_t channels, float* volumes) override {
        if (!volumes || channels == 0) return;
        std::lock_guard lock(g_audio_mtx);
        for (uint32_t c = 0; c < channels; ++c) {
            volumes[c] = (c < channel_volumes_.size()) ? channel_volumes_[c] : 1.0f;
        }
    }

    uint32_t channels() const { return channels_; }
    uint32_t sample_rate() const { return sample_rate_; }
    float volume_locked() const { return volume_; }
    float channel_volume_locked(uint32_t c) const {
        return (c < channel_volumes_.size()) ? channel_volumes_[c] : 1.0f;
    }
    bool engine_running() const {
        return !engine_state_ || engine_state_->running.load(std::memory_order_acquire);
    }

    std::vector<float> mix_buf_;

private:
    std::shared_ptr<EngineState> engine_state_;
    uint32_t channels_ = kMixerChannels;
    uint32_t sample_rate_ = kDefaultSampleRate;
    float volume_ = 1.0f;
    std::vector<float> channel_volumes_;
};

class KxSubmixVoice final : public IKxSubmixVoice {
public:
    KxSubmixVoice(
        std::shared_ptr<EngineState> engine_state,
        uint32_t input_channels,
        uint32_t input_sample_rate,
        uint32_t processing_stage,
        const KxVoiceSends* send_list)
        : engine_state_(std::move(engine_state)),
          channels_(input_channels > 0 ? input_channels : kMixerChannels),
          sample_rate_(input_sample_rate > 0 ? input_sample_rate : kDefaultSampleRate),
          processing_stage_(processing_stage),
          channel_volumes_(channels_, 1.0f) {
        if (send_list) {
            has_custom_sends_ = true;
            if (send_list->sends && send_list->send_count > 0) {
                sends_.reserve(send_list->send_count);
                for (uint32_t i = 0; i < send_list->send_count; ++i) {
                    if (send_list->sends[i].output_voice) {
                        sends_.push_back(send_list->sends[i].output_voice);
                    }
                }
            }
        }
        std::lock_guard lock(g_audio_mtx);
        g_submix_voices.push_back(this);
    }

    HRESULT STDMETHODCALLTYPE SetVolume(float v) override {
        std::lock_guard lock(g_audio_mtx);
        volume_ = std::clamp(v, 0.0f, 4.0f);
        return S_OK;
    }

    void STDMETHODCALLTYPE GetVolume(float* v) override {
        if (!v) return;
        std::lock_guard lock(g_audio_mtx);
        *v = volume_;
    }

    HRESULT STDMETHODCALLTYPE SetChannelVolumes(uint32_t channels, const float* volumes) override {
        if (!volumes || channels != channels_) return E_INVALIDARG;
        std::lock_guard lock(g_audio_mtx);
        for (uint32_t c = 0; c < channels_; ++c) {
            channel_volumes_[c] = std::clamp(volumes[c], 0.0f, 4.0f);
        }
        return S_OK;
    }

    void STDMETHODCALLTYPE GetChannelVolumes(uint32_t channels, float* volumes) override {
        if (!volumes || channels == 0) return;
        std::lock_guard lock(g_audio_mtx);
        const uint32_t n = std::min(channels, channels_);
        for (uint32_t c = 0; c < n; ++c) {
            volumes[c] = channel_volumes_[c];
        }
    }

    HRESULT STDMETHODCALLTYPE SetOutputMatrix(
        void* dst_voice, uint32_t src_channels, uint32_t dst_channels, const float* level_matrix) override {
        if (!level_matrix || src_channels != channels_ || dst_channels == 0) return E_INVALIDARG;
        std::lock_guard lock(g_audio_mtx);
        OutputMatrix m;
        m.src_channels = src_channels;
        m.dst_channels = dst_channels;
        const size_t total = static_cast<size_t>(src_channels) * dst_channels;
        m.coeffs.resize(total);
        for (size_t i = 0; i < total; ++i) {
            m.coeffs[i] = std::clamp(level_matrix[i], -4.0f, 4.0f);
        }
        output_matrices_[dst_voice] = std::move(m);
        return S_OK;
    }

    void STDMETHODCALLTYPE GetOutputMatrix(
        void* dst_voice, uint32_t src_channels, uint32_t dst_channels, float* level_matrix) override {
        if (!level_matrix || src_channels == 0 || dst_channels == 0) return;
        std::lock_guard lock(g_audio_mtx);
        const size_t total = static_cast<size_t>(src_channels) * dst_channels;
        const OutputMatrix* m = find_matrix_locked(output_matrices_, dst_voice, src_channels, dst_channels);
        if (m) {
            std::memcpy(level_matrix, m->coeffs.data(), total * sizeof(float));
        } else {
            std::memset(level_matrix, 0, total * sizeof(float));
            if (src_channels == 1) {
                for (uint32_t d = 0; d < dst_channels; ++d) level_matrix[d] = 1.0f;
            } else {
                const uint32_t common = std::min(src_channels, dst_channels);
                for (uint32_t c = 0; c < common; ++c) level_matrix[static_cast<size_t>(c) * src_channels + c] = 1.0f;
            }
        }
    }

    HRESULT STDMETHODCALLTYPE SetOutputVoices(const KxVoiceSends* send_list) override {
        std::lock_guard lock(g_audio_mtx);
        sends_.clear();
        if (!send_list) {
            has_custom_sends_ = false;
            return S_OK;
        }
        has_custom_sends_ = true;
        if (send_list->sends && send_list->send_count > 0) {
            sends_.reserve(send_list->send_count);
            for (uint32_t i = 0; i < send_list->send_count; ++i) {
                if (send_list->sends[i].output_voice) {
                    sends_.push_back(send_list->sends[i].output_voice);
                }
            }
        }
        return S_OK;
    }

    uint32_t STDMETHODCALLTYPE GetProcessingStage() const override { return processing_stage_; }

    void STDMETHODCALLTYPE DestroyVoice() override {
        {
            std::lock_guard lock(g_audio_mtx);
            void* self_key = static_cast<void*>(static_cast<IKxSubmixVoice*>(this));
            detach_destination_from_all_voices_locked(self_key);
            std::erase(g_submix_voices, this);
        }
        delete this;
    }

    void detach_destination_locked(void* dst_ptr) {
        std::erase(sends_, dst_ptr);
        output_matrices_.erase(dst_ptr);
    }

    uint32_t channels() const { return channels_; }
    uint32_t sample_rate() const { return sample_rate_; }
    uint32_t processing_stage() const { return processing_stage_; }
    float volume_locked() const { return volume_; }
    const std::vector<float>& channel_volumes_locked() const { return channel_volumes_; }
    bool has_custom_sends_locked() const { return has_custom_sends_; }
    const std::vector<void*>& sends_locked() const { return sends_; }
    const std::unordered_map<void*, OutputMatrix>& matrices_locked() const { return output_matrices_; }
    const std::shared_ptr<EngineState>& engine_state() const { return engine_state_; }

    std::vector<float> mix_buf_;
    size_t frames_active_ = 0;

private:
    std::shared_ptr<EngineState> engine_state_;
    uint32_t channels_ = kMixerChannels;
    uint32_t sample_rate_ = kDefaultSampleRate;
    uint32_t processing_stage_ = 0;
    float volume_ = 1.0f;
    std::vector<float> channel_volumes_;
    bool has_custom_sends_ = false;
    std::vector<void*> sends_;
    std::unordered_map<void*, OutputMatrix> output_matrices_;
};

class KxSourceVoice final : public IKxSourceVoice {
public:
    KxSourceVoice(std::shared_ptr<EngineState> engine_state, const KxWaveFormat& fmt)
        : engine_state_(std::move(engine_state)),
          fmt_(fmt),
          channels_(std::max<uint32_t>(fmt.channels, 1u)),
          sample_rate_(fmt.sample_rate > 0 ? fmt.sample_rate : kDefaultSampleRate),
          channel_volumes_(channels_, 1.0f) {
        std::lock_guard lock(g_audio_mtx);
        g_source_voices.push_back(this);
    }

    HRESULT STDMETHODCALLTYPE Start(uint32_t) override {
        std::lock_guard lock(g_audio_mtx);
        playing_ = true;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Stop(uint32_t) override {
        std::lock_guard lock(g_audio_mtx);
        playing_ = false;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SubmitSourceBuffer(const KxAudioBuffer* buf) override {
        if (!buf || !buf->audio_data || buf->audio_bytes == 0) return E_INVALIDARG;
        const uint32_t ch = channels_;
        uint32_t bps = 4u;
        if (fmt_.format_tag == 1) {
            if (fmt_.bits_per_sample == 8) bps = 1u;
            else if (fmt_.bits_per_sample == 16) bps = 2u;
            else bps = 4u;
        }
        const size_t frames = buf->audio_bytes / (static_cast<size_t>(ch) * bps);
        if (frames == 0) return E_INVALIDARG;

        QueuedBuffer qb;
        qb.channels = ch;
        qb.frames = frames;
        qb.flags = buf->flags;
        qb.samples.resize(frames * ch, 0.0f);

        if (fmt_.format_tag == 1 && fmt_.bits_per_sample == 16) {
            const auto* s16 = reinterpret_cast<const int16_t*>(buf->audio_data);
            for (size_t i = 0; i < frames * ch; ++i) {
                qb.samples[i] = static_cast<float>(s16[i]) / 32768.0f;
            }
        } else if (fmt_.format_tag == 1 && fmt_.bits_per_sample == 8) {
            for (size_t i = 0; i < frames * ch; ++i) {
                qb.samples[i] = (static_cast<float>(buf->audio_data[i]) - 128.0f) / 128.0f;
            }
        } else if (fmt_.format_tag == 1 && fmt_.bits_per_sample == 32) {
            const auto* s32 = reinterpret_cast<const int32_t*>(buf->audio_data);
            for (size_t i = 0; i < frames * ch; ++i) {
                qb.samples[i] = static_cast<float>(static_cast<double>(s32[i]) / 2147483648.0);
            }
        } else {
            const auto* f32 = reinterpret_cast<const float*>(buf->audio_data);
            std::memcpy(qb.samples.data(), f32, frames * ch * sizeof(float));
        }

        {
            std::lock_guard lock(g_audio_mtx);
            queue_.push_back(std::move(qb));
            queued_.store(static_cast<uint32_t>(queue_.size()), std::memory_order_relaxed);
        }

        g_buffers.fetch_add(1, std::memory_order_relaxed);
        g_frames_sub.fetch_add(frames, std::memory_order_relaxed);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetVolume(float v) override {
        std::lock_guard lock(g_audio_mtx);
        volume_ = std::clamp(v, 0.0f, 4.0f);
        return S_OK;
    }

    uint32_t STDMETHODCALLTYPE GetBuffersQueued() override {
        return queued_.load(std::memory_order_relaxed);
    }

    void STDMETHODCALLTYPE DestroyVoice() override {
        {
            std::lock_guard lock(g_audio_mtx);
            std::erase(g_source_voices, this);
        }
        delete this;
    }

    void STDMETHODCALLTYPE GetVolume(float* v) override {
        if (!v) return;
        std::lock_guard lock(g_audio_mtx);
        *v = volume_;
    }

    HRESULT STDMETHODCALLTYPE FlushSourceBuffers() override {
        std::lock_guard lock(g_audio_mtx);
        queue_.clear();
        cursor_ = 0.0;
        queued_.store(0, std::memory_order_relaxed);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetFrequencyRatio(float ratio) override {
        if (!(ratio > 0.0f) || std::isnan(ratio) || std::isinf(ratio)) return E_INVALIDARG;
        std::lock_guard lock(g_audio_mtx);
        frequency_ratio_ = std::clamp(ratio, kMinFreqRatio, kMaxFreqRatio);
        return S_OK;
    }

    void STDMETHODCALLTYPE GetFrequencyRatio(float* ratio) override {
        if (!ratio) return;
        std::lock_guard lock(g_audio_mtx);
        *ratio = frequency_ratio_;
    }

    HRESULT STDMETHODCALLTYPE SetChannelVolumes(uint32_t channels, const float* volumes) override {
        if (!volumes || channels != channels_) return E_INVALIDARG;
        std::lock_guard lock(g_audio_mtx);
        for (uint32_t c = 0; c < channels_; ++c) {
            channel_volumes_[c] = std::clamp(volumes[c], 0.0f, 4.0f);
        }
        return S_OK;
    }

    void STDMETHODCALLTYPE GetChannelVolumes(uint32_t channels, float* volumes) override {
        if (!volumes || channels == 0) return;
        std::lock_guard lock(g_audio_mtx);
        const uint32_t n = std::min(channels, channels_);
        for (uint32_t c = 0; c < n; ++c) {
            volumes[c] = channel_volumes_[c];
        }
    }

    HRESULT STDMETHODCALLTYPE SetOutputMatrix(
        void* dst_voice, uint32_t src_channels, uint32_t dst_channels, const float* level_matrix) override {
        if (!level_matrix || src_channels != channels_ || dst_channels == 0) return E_INVALIDARG;
        std::lock_guard lock(g_audio_mtx);
        OutputMatrix m;
        m.src_channels = src_channels;
        m.dst_channels = dst_channels;
        const size_t total = static_cast<size_t>(src_channels) * dst_channels;
        m.coeffs.resize(total);
        for (size_t i = 0; i < total; ++i) {
            m.coeffs[i] = std::clamp(level_matrix[i], -4.0f, 4.0f);
        }
        output_matrices_[dst_voice] = std::move(m);
        return S_OK;
    }

    void STDMETHODCALLTYPE GetOutputMatrix(
        void* dst_voice, uint32_t src_channels, uint32_t dst_channels, float* level_matrix) override {
        if (!level_matrix || src_channels == 0 || dst_channels == 0) return;
        std::lock_guard lock(g_audio_mtx);
        const size_t total = static_cast<size_t>(src_channels) * dst_channels;
        const OutputMatrix* m = find_matrix_locked(output_matrices_, dst_voice, src_channels, dst_channels);
        if (m) {
            std::memcpy(level_matrix, m->coeffs.data(), total * sizeof(float));
        } else {
            std::memset(level_matrix, 0, total * sizeof(float));
            if (src_channels == 1) {
                for (uint32_t d = 0; d < dst_channels; ++d) level_matrix[d] = 1.0f;
            } else {
                const uint32_t common = std::min(src_channels, dst_channels);
                for (uint32_t c = 0; c < common; ++c) level_matrix[static_cast<size_t>(c) * src_channels + c] = 1.0f;
            }
        }
    }

    HRESULT STDMETHODCALLTYPE SetOutputVoices(const KxVoiceSends* send_list) override {
        std::lock_guard lock(g_audio_mtx);
        sends_.clear();
        if (!send_list) {
            has_custom_sends_ = false;
            return S_OK;
        }
        has_custom_sends_ = true;
        if (send_list->sends && send_list->send_count > 0) {
            sends_.reserve(send_list->send_count);
            for (uint32_t i = 0; i < send_list->send_count; ++i) {
                if (send_list->sends[i].output_voice) {
                    sends_.push_back(send_list->sends[i].output_voice);
                }
            }
        }
        return S_OK;
    }

    void STDMETHODCALLTYPE GetState(KxVoiceState* state) override {
        if (!state) return;
        std::lock_guard lock(g_audio_mtx);
        state->current_buffer_context = nullptr;
        state->buffers_queued = static_cast<uint32_t>(queue_.size());
        state->samples_played = samples_played_;
    }

    void detach_destination_locked(void* dst_ptr) {
        std::erase(sends_, dst_ptr);
        output_matrices_.erase(dst_ptr);
    }

    // Pull & linearly resample up to max_frames into dst_interleaved (sized max_frames * channels_).
    // Returns number of output frames rendered before the queue drained.
    size_t pull_resampled_locked(float* dst_interleaved, size_t max_frames, uint32_t output_rate) {
        if (!playing_ || queue_.empty() || max_frames == 0) return 0;
        if (engine_state_ && !engine_state_->running.load(std::memory_order_acquire)) return 0;

        const double out_sr = static_cast<double>(output_rate > 0 ? output_rate : kDefaultSampleRate);
        const double step = (static_cast<double>(sample_rate_) / out_sr) * static_cast<double>(frequency_ratio_);
        const uint32_t ch = channels_;

        size_t rendered = 0;
        while (rendered < max_frames && !queue_.empty()) {
            const QueuedBuffer& cur = queue_.front();
            const size_t idx0 = std::min<size_t>(static_cast<size_t>(cursor_), cur.frames - 1);
            const float frac = static_cast<float>(cursor_ - static_cast<double>(idx0));

            const float* s0 = cur.samples.data() + idx0 * ch;
            const float* s1 = s0;
            if (idx0 + 1 < cur.frames) {
                s1 = cur.samples.data() + (idx0 + 1) * ch;
            } else if (queue_.size() > 1) {
                s1 = queue_[1].samples.data();
            }

            float* out_frame = dst_interleaved + rendered * ch;
            if (frac <= 1e-7f) {
                for (uint32_t c = 0; c < ch; ++c) {
                    out_frame[c] = s0[c];
                }
            } else {
                const float inv_frac = 1.0f - frac;
                for (uint32_t c = 0; c < ch; ++c) {
                    out_frame[c] = s0[c] * inv_frac + s1[c] * frac;
                }
            }

            ++rendered;
            ++samples_played_;
            cursor_ += step;

            while (!queue_.empty() && cursor_ >= static_cast<double>(queue_.front().frames)) {
                cursor_ -= static_cast<double>(queue_.front().frames);
                queue_.pop_front();
            }
            if (queue_.empty()) {
                cursor_ = 0.0;
            }
        }

        queued_.store(static_cast<uint32_t>(queue_.size()), std::memory_order_relaxed);
        return rendered;
    }

    uint32_t channels() const { return channels_; }
    float volume_locked() const { return volume_; }
    const std::vector<float>& channel_volumes_locked() const { return channel_volumes_; }
    bool has_custom_sends_locked() const { return has_custom_sends_; }
    const std::vector<void*>& sends_locked() const { return sends_; }
    const std::unordered_map<void*, OutputMatrix>& matrices_locked() const { return output_matrices_; }
    const std::shared_ptr<EngineState>& engine_state() const { return engine_state_; }

private:
    std::shared_ptr<EngineState> engine_state_;
    KxWaveFormat fmt_{};
    uint32_t channels_ = kMixerChannels;
    uint32_t sample_rate_ = kDefaultSampleRate;
    bool playing_ = false;
    float volume_ = 1.0f;
    float frequency_ratio_ = 1.0f;
    double cursor_ = 0.0;
    uint64_t samples_played_ = 0;
    std::vector<float> channel_volumes_;
    std::deque<QueuedBuffer> queue_;
    std::atomic<uint32_t> queued_{0};
    bool has_custom_sends_ = false;
    std::vector<void*> sends_;
    std::unordered_map<void*, OutputMatrix> output_matrices_;
};

void detach_destination_from_all_voices_locked(void* dst_ptr) {
    for (auto* sv : g_source_voices) {
        sv->detach_destination_locked(dst_ptr);
    }
    for (auto* sub : g_submix_voices) {
        sub->detach_destination_locked(dst_ptr);
    }
}

KxSubmixVoice* find_submix_locked(void* ptr) {
    for (auto* s : g_submix_voices) {
        if (static_cast<void*>(static_cast<IKxSubmixVoice*>(s)) == ptr) return s;
    }
    return nullptr;
}

KxMasteringVoice* find_mastering_locked(void* ptr) {
    for (auto* m : g_mastering_voices) {
        if (static_cast<void*>(static_cast<IKxMasteringVoice*>(m)) == ptr) return m;
    }
    return nullptr;
}

class KxXAudio2 final : public IKxXAudio2 {
public:
    KxXAudio2() : state_(std::make_shared<EngineState>()) {}

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
        IKxMasteringVoice** out_voice, uint32_t channels, uint32_t sample_rate) override {
        if (!out_voice) return E_POINTER;
        g_voices.fetch_add(1, std::memory_order_relaxed);
        if (g_wasapi_enabled.load(std::memory_order_relaxed)) {
            std::lock_guard lock(g_wasapi_mtx);
            start_wasapi_thread_locked();
        }
        *out_voice = new KxMasteringVoice(state_, channels, sample_rate);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateSourceVoice(
        IKxSourceVoice** out_voice, const KxWaveFormat* fmt) override {
        if (!out_voice || !fmt) return E_POINTER;
        g_voices.fetch_add(1, std::memory_order_relaxed);
        *out_voice = new KxSourceVoice(state_, *fmt);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE StartEngine() override {
        state_->running.store(true, std::memory_order_release);
        return S_OK;
    }

    void STDMETHODCALLTYPE StopEngine() override {
        state_->running.store(false, std::memory_order_release);
    }

    HRESULT STDMETHODCALLTYPE CreateSubmixVoice(
        IKxSubmixVoice** out_voice,
        uint32_t input_channels,
        uint32_t input_sample_rate,
        uint32_t,
        uint32_t processing_stage,
        const KxVoiceSends* send_list) override {
        if (!out_voice) return E_POINTER;
        g_voices.fetch_add(1, std::memory_order_relaxed);
        g_submix_voices_created.fetch_add(1, std::memory_order_relaxed);
        *out_voice = new KxSubmixVoice(state_, input_channels, input_sample_rate, processing_stage, send_list);
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_{1};
    std::shared_ptr<EngineState> state_;
};

HRESULT __stdcall kx_XAudio2Create(IKxXAudio2** out_xa, uint32_t, uint32_t) {
    if (!out_xa) return E_POINTER;
    g_engines.fetch_add(1, std::memory_order_relaxed);
    *out_xa = new KxXAudio2();
    return S_OK;
}

// Clean-room 3D vector helpers for X3DAudio
struct X3DInstanceHeader {
    uint32_t magic;
    uint32_t speaker_mask;
    float speed_of_sound;
    uint32_t speaker_channels;
    uint32_t reserved;
};
static_assert(sizeof(X3DInstanceHeader) == sizeof(KxX3DAudioHandle));

constexpr uint32_t kX3DMagic = 0x3344584Bu;  // "KXD3"

KxVec3 vec_sub(const KxVec3& a, const KxVec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

float vec_dot(const KxVec3& a, const KxVec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

KxVec3 vec_cross(const KxVec3& a, const KxVec3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

float vec_len(const KxVec3& v) {
    return std::sqrt(vec_dot(v, v));
}

KxVec3 vec_normalize(const KxVec3& v, const KxVec3& fallback) {
    const float len = vec_len(v);
    if (len <= 1e-6f) return fallback;
    const float inv = 1.0f / len;
    return {v.x * inv, v.y * inv, v.z * inv};
}

}  // namespace

HRESULT __stdcall kx_X3DAudioInitialize(
    uint32_t speaker_channel_mask, float speed_of_sound, KxX3DAudioHandle* out_instance) {
    if (!out_instance || !(speed_of_sound > 0.0f) || std::isnan(speed_of_sound) || std::isinf(speed_of_sound)) {
        return E_INVALIDARG;
    }
    const uint32_t mask = (speaker_channel_mask != 0) ? speaker_channel_mask : KxSpeakerStereo;
    const uint32_t ch = std::max<uint32_t>(static_cast<uint32_t>(std::popcount(mask)), 1u);
    X3DInstanceHeader hdr{kX3DMagic, mask, speed_of_sound, ch, 0u};
    std::memcpy(out_instance->bytes, &hdr, sizeof(hdr));
    return S_OK;
}

void __stdcall kx_X3DAudioCalculate(
    const KxX3DAudioHandle* instance,
    const KxX3DAudioListener* listener,
    const KxX3DAudioEmitter* emitter,
    uint32_t flags,
    KxX3DAudioDspSettings* dsp_settings) {
    if (!instance || !listener || !emitter || !dsp_settings) return;

    X3DInstanceHeader hdr{};
    std::memcpy(&hdr, instance->bytes, sizeof(hdr));
    const float speed_of_sound = (hdr.magic == kX3DMagic && hdr.speed_of_sound > 0.0f) ? hdr.speed_of_sound : 343.5f;

    // Build left-handed listener basis (+X = Right, +Y = Up, +Z = Forward)
    const KxVec3 front = vec_normalize(listener->orient_front, {0.0f, 0.0f, 1.0f});
    const KxVec3 top = vec_normalize(listener->orient_top, {0.0f, 1.0f, 0.0f});
    const KxVec3 right = vec_normalize(vec_cross(top, front), {1.0f, 0.0f, 0.0f});

    const KxVec3 delta = vec_sub(emitter->position, listener->position);
    const float dist = vec_len(delta);
    const KxVec3 dir = (dist > 1e-6f)
        ? KxVec3{delta.x / dist, delta.y / dist, delta.z / dist}
        : KxVec3{0.0f, 0.0f, 0.0f};

    dsp_settings->emitter_to_listener_distance = dist;

    // Horizontal pan in [-1 (left), +1 (right)]
    float pan = (dist > 1e-6f) ? std::clamp(vec_dot(dir, right), -1.0f, 1.0f) : 0.0f;
    if (emitter->inner_radius > 0.0f && dist < emitter->inner_radius) {
        pan *= (dist / emitter->inner_radius);
    }

    if ((flags & KxX3DAudioCalculateEmitterAngle) != 0u) {
        if (dist > 1e-6f) {
            const KxVec3 em_front = vec_normalize(emitter->orient_front, {0.0f, 0.0f, 1.0f});
            const KxVec3 to_listener{-dir.x, -dir.y, -dir.z};
            const float cos_ang = std::clamp(vec_dot(em_front, to_listener), -1.0f, 1.0f);
            dsp_settings->emitter_to_listener_angle = std::acos(cos_ang);
        } else {
            dsp_settings->emitter_to_listener_angle = 0.0f;
        }
    }

    if ((flags & KxX3DAudioCalculateMatrix) != 0u && dsp_settings->matrix_coefficients != nullptr) {
        const uint32_t src_ch = std::max<uint32_t>(dsp_settings->src_channel_count, 1u);
        const uint32_t dst_ch = std::max<uint32_t>(dsp_settings->dst_channel_count, 1u);
        const size_t total = static_cast<size_t>(src_ch) * dst_ch;
        std::memset(dsp_settings->matrix_coefficients, 0, total * sizeof(float));

        const float dist_scale = (emitter->curve_distance_scaler > 1e-6f) ? emitter->curve_distance_scaler : 1.0f;
        const float norm_dist = dist / dist_scale;
        const float atten = (norm_dist <= 1.0f) ? 1.0f : (1.0f / norm_dist);

        if (dst_ch == 1) {
            const float gain = atten / static_cast<float>(src_ch);
            for (uint32_t s = 0; s < src_ch; ++s) {
                dsp_settings->matrix_coefficients[s] = gain;
            }
        } else {
            // Constant-power stereo pan law: pan=-1 -> L=1,R=0; pan=0 -> L=R=0.7071; pan=+1 -> L=0,R=1
            const float angle = (pan + 1.0f) * (kPi * 0.25f);
            const float left_gain = std::cos(angle) * atten;
            const float right_gain = std::sin(angle) * atten;

            if (src_ch == 1) {
                dsp_settings->matrix_coefficients[0] = left_gain;
                dsp_settings->matrix_coefficients[1] = right_gain;
            } else if (src_ch == 2) {
                dsp_settings->matrix_coefficients[0 * src_ch + 0] = left_gain;
                dsp_settings->matrix_coefficients[1 * src_ch + 1] = right_gain;
            } else if (src_ch == 6) {
                // ITU-R BS.775 5.1 -> Stereo pan matrix
                constexpr float kWeight = 0.70710678f;
                // row 0: Left [FL, FR, FC, LFE, BL, BR]
                dsp_settings->matrix_coefficients[0 * 6 + 0] = left_gain * 1.0f;
                dsp_settings->matrix_coefficients[0 * 6 + 1] = 0.0f;
                dsp_settings->matrix_coefficients[0 * 6 + 2] = left_gain * kWeight;
                dsp_settings->matrix_coefficients[0 * 6 + 3] = 0.0f;
                dsp_settings->matrix_coefficients[0 * 6 + 4] = left_gain * kWeight;
                dsp_settings->matrix_coefficients[0 * 6 + 5] = 0.0f;
                // row 1: Right [FL, FR, FC, LFE, BL, BR]
                dsp_settings->matrix_coefficients[1 * 6 + 0] = 0.0f;
                dsp_settings->matrix_coefficients[1 * 6 + 1] = right_gain * 1.0f;
                dsp_settings->matrix_coefficients[1 * 6 + 2] = right_gain * kWeight;
                dsp_settings->matrix_coefficients[1 * 6 + 3] = 0.0f;
                dsp_settings->matrix_coefficients[1 * 6 + 4] = 0.0f;
                dsp_settings->matrix_coefficients[1 * 6 + 5] = right_gain * kWeight;
            } else if (src_ch == 8) {
                // ITU-R BS.775 7.1 -> Stereo pan matrix
                constexpr float kWeight = 0.70710678f;
                // row 0: Left [FL, FR, FC, LFE, BL, BR, SL, SR]
                dsp_settings->matrix_coefficients[0 * 8 + 0] = left_gain * 1.0f;
                dsp_settings->matrix_coefficients[0 * 8 + 1] = 0.0f;
                dsp_settings->matrix_coefficients[0 * 8 + 2] = left_gain * kWeight;
                dsp_settings->matrix_coefficients[0 * 8 + 3] = 0.0f;
                dsp_settings->matrix_coefficients[0 * 8 + 4] = left_gain * kWeight;
                dsp_settings->matrix_coefficients[0 * 8 + 5] = 0.0f;
                dsp_settings->matrix_coefficients[0 * 8 + 6] = left_gain * kWeight;
                dsp_settings->matrix_coefficients[0 * 8 + 7] = 0.0f;
                // row 1: Right [FL, FR, FC, LFE, BL, BR, SL, SR]
                dsp_settings->matrix_coefficients[1 * 8 + 0] = 0.0f;
                dsp_settings->matrix_coefficients[1 * 8 + 1] = right_gain * 1.0f;
                dsp_settings->matrix_coefficients[1 * 8 + 2] = right_gain * kWeight;
                dsp_settings->matrix_coefficients[1 * 8 + 3] = 0.0f;
                dsp_settings->matrix_coefficients[1 * 8 + 4] = 0.0f;
                dsp_settings->matrix_coefficients[1 * 8 + 5] = right_gain * kWeight;
                dsp_settings->matrix_coefficients[1 * 8 + 6] = 0.0f;
                dsp_settings->matrix_coefficients[1 * 8 + 7] = right_gain * kWeight;
            } else {
                const float inv_src = 1.0f / static_cast<float>(src_ch);
                for (uint32_t s = 0; s < src_ch; ++s) {
                    dsp_settings->matrix_coefficients[0 * src_ch + s] = left_gain * inv_src;
                    dsp_settings->matrix_coefficients[1 * src_ch + s] = right_gain * inv_src;
                }
            }
        }
    }

    if ((flags & KxX3DAudioCalculateDoppler) != 0u) {
        const float v_l = (dist > 1e-6f) ? vec_dot(listener->velocity, dir) : 0.0f;
        const float v_e = (dist > 1e-6f) ? vec_dot(emitter->velocity, dir) : 0.0f;
        dsp_settings->listener_velocity_component = v_l;
        dsp_settings->emitter_velocity_component = v_e;
        const float doppler_scale = (emitter->doppler_scaler >= 0.0f) ? emitter->doppler_scaler : 1.0f;
        const float num = std::max(speed_of_sound + doppler_scale * v_l, 1e-3f);
        const float den = std::max(speed_of_sound + doppler_scale * v_e, 1e-3f);
        dsp_settings->doppler_factor = std::clamp(num / den, kMinFreqRatio, kMaxFreqRatio);
    }

    if ((flags & KxX3DAudioCalculateDelay) != 0u && dsp_settings->delay_times != nullptr &&
        dsp_settings->dst_channel_count >= 2) {
        constexpr float kHeadWidthMeters = 0.22f;
        const float max_itd_ms = (kHeadWidthMeters / speed_of_sound) * 1000.0f;
        std::memset(dsp_settings->delay_times, 0, dsp_settings->dst_channel_count * sizeof(float));
        if (pan > 0.0f) {
            dsp_settings->delay_times[0] = pan * max_itd_ms;
            dsp_settings->delay_times[1] = 0.0f;
        } else {
            dsp_settings->delay_times[0] = 0.0f;
            dsp_settings->delay_times[1] = (-pan) * max_itd_ms;
        }
    }

    if ((flags & KxX3DAudioCalculateLpfDirect) != 0u) {
        const float dist_scale = (emitter->curve_distance_scaler > 1e-6f) ? emitter->curve_distance_scaler : 1.0f;
        const float norm_dist = dist / dist_scale;
        dsp_settings->lpf_direct_coefficient = std::clamp(1.0f / (1.0f + 0.05f * norm_dist), 0.1f, 1.0f);
    }

    if ((flags & KxX3DAudioCalculateLpfReverb) != 0u) {
        dsp_settings->lpf_reverb_coefficient = 0.75f;
    }

    if ((flags & KxX3DAudioCalculateReverb) != 0u) {
        const float dist_scale = (emitter->curve_distance_scaler > 1e-6f) ? emitter->curve_distance_scaler : 1.0f;
        const float norm_dist = dist / dist_scale;
        dsp_settings->reverb_level = std::clamp(norm_dist / (1.0f + norm_dist), 0.0f, 1.0f);
    }

    g_x3daudio_calls.fetch_add(1, std::memory_order_relaxed);
}

size_t xaudio2_drain_stereo_f32(float* out_interleaved_stereo, size_t max_frames) {
    if (!out_interleaved_stereo || max_frames == 0) return 0;

    std::lock_guard lock(g_audio_mtx);

    std::vector<float> fallback_master_buf(max_frames * kMixerChannels, 0.0f);
    for (auto* mv : g_mastering_voices) {
        mv->mix_buf_.assign(max_frames * kMixerChannels, 0.0f);
    }
    for (auto* sub : g_submix_voices) {
        sub->mix_buf_.assign(max_frames * sub->channels(), 0.0f);
        sub->frames_active_ = 0;
    }

    size_t max_frames_produced = 0;
    std::vector<float> voice_scratch;

    // Stage 1: Pull active source voices (resample + route to submixes or mastering voice)
    for (auto* sv : g_source_voices) {
        const auto& es = sv->engine_state();
        if (es && !es->running.load(std::memory_order_acquire)) continue;

        const uint32_t out_rate = (es && es->mastering) ? es->mastering->sample_rate() : kDefaultSampleRate;
        const uint32_t src_ch = sv->channels();
        voice_scratch.resize(max_frames * src_ch);

        const size_t rendered = sv->pull_resampled_locked(voice_scratch.data(), max_frames, out_rate);
        if (rendered == 0) continue;

        max_frames_produced = std::max(max_frames_produced, rendered);

        auto route_to_master_default = [&]() {
            if (es && es->mastering) {
                void* key = static_cast<void*>(static_cast<IKxMasteringVoice*>(es->mastering));
                const OutputMatrix* mat = find_matrix_locked(sv->matrices_locked(), key, src_ch, kMixerChannels);
                apply_matrix_accumulate(
                    voice_scratch.data(), rendered, src_ch, sv->volume_locked(),
                    sv->channel_volumes_locked(), mat, es->mastering->mix_buf_.data(), kMixerChannels);
            } else {
                const OutputMatrix* mat = find_matrix_locked(sv->matrices_locked(), nullptr, src_ch, kMixerChannels);
                apply_matrix_accumulate(
                    voice_scratch.data(), rendered, src_ch, sv->volume_locked(),
                    sv->channel_volumes_locked(), mat, fallback_master_buf.data(), kMixerChannels);
            }
        };

        if (!sv->has_custom_sends_locked()) {
            route_to_master_default();
        } else {
            for (void* dst_ptr : sv->sends_locked()) {
                if (auto* sub = find_submix_locked(dst_ptr)) {
                    const uint32_t dst_ch = sub->channels();
                    const OutputMatrix* mat = find_matrix_locked(sv->matrices_locked(), dst_ptr, src_ch, dst_ch);
                    apply_matrix_accumulate(
                        voice_scratch.data(), rendered, src_ch, sv->volume_locked(),
                        sv->channel_volumes_locked(), mat, sub->mix_buf_.data(), dst_ch);
                    sub->frames_active_ = std::max(sub->frames_active_, rendered);
                } else if (auto* mv = find_mastering_locked(dst_ptr)) {
                    const OutputMatrix* mat = find_matrix_locked(sv->matrices_locked(), dst_ptr, src_ch, kMixerChannels);
                    apply_matrix_accumulate(
                        voice_scratch.data(), rendered, src_ch, sv->volume_locked(),
                        sv->channel_volumes_locked(), mat, mv->mix_buf_.data(), kMixerChannels);
                }
            }
        }
    }

    // Stage 2: Process submix voices in ascending processing_stage order
    if (!g_submix_voices.empty() && max_frames_produced > 0) {
        std::vector<KxSubmixVoice*> ordered = g_submix_voices;
        std::stable_sort(ordered.begin(), ordered.end(), [](const KxSubmixVoice* a, const KxSubmixVoice* b) {
            return a->processing_stage() < b->processing_stage();
        });

        for (auto* sub : ordered) {
            const auto& es = sub->engine_state();
            if (es && !es->running.load(std::memory_order_acquire)) continue;
            if (sub->frames_active_ == 0) continue;

            const size_t n = sub->frames_active_;
            const uint32_t sub_ch = sub->channels();

            if (!sub->has_custom_sends_locked()) {
                if (es && es->mastering) {
                    void* key = static_cast<void*>(static_cast<IKxMasteringVoice*>(es->mastering));
                    const OutputMatrix* mat = find_matrix_locked(sub->matrices_locked(), key, sub_ch, kMixerChannels);
                    apply_matrix_accumulate(
                        sub->mix_buf_.data(), n, sub_ch, sub->volume_locked(),
                        sub->channel_volumes_locked(), mat, es->mastering->mix_buf_.data(), kMixerChannels);
                } else {
                    const OutputMatrix* mat = find_matrix_locked(sub->matrices_locked(), nullptr, sub_ch, kMixerChannels);
                    apply_matrix_accumulate(
                        sub->mix_buf_.data(), n, sub_ch, sub->volume_locked(),
                        sub->channel_volumes_locked(), mat, fallback_master_buf.data(), kMixerChannels);
                }
            } else {
                for (void* dst_ptr : sub->sends_locked()) {
                    if (auto* dst_sub = find_submix_locked(dst_ptr)) {
                        if (dst_sub != sub && dst_sub->processing_stage() > sub->processing_stage()) {
                            const uint32_t dst_ch = dst_sub->channels();
                            const OutputMatrix* mat = find_matrix_locked(sub->matrices_locked(), dst_ptr, sub_ch, dst_ch);
                            apply_matrix_accumulate(
                                sub->mix_buf_.data(), n, sub_ch, sub->volume_locked(),
                                sub->channel_volumes_locked(), mat, dst_sub->mix_buf_.data(), dst_ch);
                            dst_sub->frames_active_ = std::max(dst_sub->frames_active_, n);
                        }
                    } else if (auto* mv = find_mastering_locked(dst_ptr)) {
                        const OutputMatrix* mat = find_matrix_locked(sub->matrices_locked(), dst_ptr, sub_ch, kMixerChannels);
                        apply_matrix_accumulate(
                            sub->mix_buf_.data(), n, sub_ch, sub->volume_locked(),
                            sub->channel_volumes_locked(), mat, mv->mix_buf_.data(), kMixerChannels);
                    }
                }
            }
        }
    }

    // Stage 3: Sum mastering buses, apply mastering volume + hard output limiter [-1.0, 1.0]
    double sum_sq = 0.0;
    for (size_t i = 0; i < max_frames_produced; ++i) {
        float l = fallback_master_buf[i * 2 + 0];
        float r = fallback_master_buf[i * 2 + 1];
        for (const auto* mv : g_mastering_voices) {
            if (!mv->engine_running()) continue;
            const float mv_vol = mv->volume_locked();
            l += mv->mix_buf_[i * 2 + 0] * mv_vol * mv->channel_volume_locked(0);
            r += mv->mix_buf_[i * 2 + 1] * mv_vol * mv->channel_volume_locked(1);
        }
        l = std::clamp(l, -1.0f, 1.0f);
        r = std::clamp(r, -1.0f, 1.0f);
        out_interleaved_stereo[i * 2 + 0] = l;
        out_interleaved_stereo[i * 2 + 1] = r;
        sum_sq += static_cast<double>(l) * l + static_cast<double>(r) * r;
    }

    if (max_frames_produced < max_frames) {
        std::memset(out_interleaved_stereo + max_frames_produced * 2, 0,
                    (max_frames - max_frames_produced) * 2 * sizeof(float));
        g_underruns.fetch_add(1, std::memory_order_relaxed);
    }

    g_frames_con.fetch_add(max_frames_produced, std::memory_order_relaxed);
    g_last_rms = (max_frames_produced > 0)
        ? std::sqrt(sum_sq / static_cast<double>(max_frames_produced * 2))
        : 0.0;

    return max_frames_produced;
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
        reg.register_export(mod, "X3DAudioInitialize", reinterpret_cast<void*>(&kx_X3DAudioInitialize));
        reg.register_export(mod, "X3DAudioCalculate", reinterpret_cast<void*>(&kx_X3DAudioCalculate));
    }
    reg.register_module("x3daudio1_7.dll");
    reg.register_export("x3daudio1_7.dll", "X3DAudioInitialize", reinterpret_cast<void*>(&kx_X3DAudioInitialize));
    reg.register_export("x3daudio1_7.dll", "X3DAudioCalculate", reinterpret_cast<void*>(&kx_X3DAudioCalculate));
}

XAudio2Stats xaudio2_stats() {
    std::lock_guard lock(g_audio_mtx);
    return {g_engines.load(), g_voices.load(), g_buffers.load(),
            g_frames_sub.load(), g_frames_con.load(), g_underruns.load(),
            g_wasapi_packets.load(), g_wasapi_active.load(), g_last_rms,
            g_submix_voices_created.load(), g_x3daudio_calls.load()};
}

void reset_xaudio2_stats() {
    shutdown_xaudio2_wasapi();
    g_engines = 0;
    g_voices = 0;
    g_submix_voices_created = 0;
    g_buffers = 0;
    g_frames_sub = 0;
    g_frames_con = 0;
    g_underruns = 0;
    g_wasapi_packets = 0;
    g_x3daudio_calls = 0;
    std::lock_guard lock(g_audio_mtx);
    g_last_rms = 0.0;
}

}  // namespace kestrel::hle
