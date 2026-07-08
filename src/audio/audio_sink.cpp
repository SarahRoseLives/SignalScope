#include "audio/audio_sink.h"

#include "util/log.h"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr ma_uint32 kDeviceRate = 48000;
} // namespace

static void audioDataCb(ma_device* dev, void* output, const void* /*input*/, ma_uint32 frameCount)
{
    auto* self = static_cast<AudioSink*>(dev->pUserData);
    if (self)
        self->render(static_cast<float*>(output), (int)frameCount);
    else
        std::fill_n(static_cast<float*>(output), frameCount, 0.0f);
}

AudioSink::AudioSink()
{
    cap_ = 1u << 18; // 262144 mono samples (~5.5 s at 48 kHz)
    buf_.assign(cap_, 0.0f);
}

AudioSink::~AudioSink()
{
    stop();
}

bool AudioSink::start(std::string& err)
{
    if (running_.load())
        return true;

    device_ = new ma_device();
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 1;
    cfg.sampleRate        = kDeviceRate;
    cfg.dataCallback      = audioDataCb;
    cfg.pUserData         = this;

    if (ma_device_init(nullptr, &cfg, device_) != MA_SUCCESS)
    {
        delete device_;
        device_ = nullptr;
        err = "audio: failed to initialize playback device";
        return false;
    }
    devRate_ = (double)device_->sampleRate; // may differ from requested
    if (ma_device_start(device_) != MA_SUCCESS)
    {
        ma_device_uninit(device_);
        delete device_;
        device_ = nullptr;
        err = "audio: failed to start playback device";
        return false;
    }
    running_.store(true);
    logWrite("audio: playback started (%.0f Hz mono)", devRate_);
    return true;
}

void AudioSink::stop()
{
    if (device_)
    {
        ma_device_uninit(device_); // stops the callback thread first
        delete device_;
        device_ = nullptr;
    }
    running_.store(false);
}

void AudioSink::setSourceRate(double hz)
{
    if (hz <= 0.0)
        return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (std::fabs(hz - srcRate_) < 1.0)
        return;
    srcRate_ = hz;
    rpos_ = wpos_ = count_ = 0;
    readPhase_ = 0.0;
    lastSample_ = 0.0f;
}

void AudioSink::flush()
{
    std::lock_guard<std::mutex> lk(mtx_);
    rpos_ = wpos_ = count_ = 0;
    readPhase_ = 0.0;
    lastSample_ = 0.0f;
}

void AudioSink::push(const float* mono, int n)
{
    if (n <= 0)
        return;

    // Peak level for the UI meter (with a little decay across blocks).
    float pk = level_.load();
    pk *= 0.6f;
    for (int i = 0; i < n; ++i)
    {
        float a = std::fabs(mono[i]);
        if (a > pk)
            pk = a;
    }
    if (pk > 1.0f)
        pk = 1.0f;
    level_.store(pk);

    std::lock_guard<std::mutex> lk(mtx_);
    for (int i = 0; i < n; ++i)
    {
        buf_[wpos_] = mono[i];
        wpos_ = (wpos_ + 1) % cap_;
        if (count_ < cap_)
            ++count_;
        else
            rpos_ = (rpos_ + 1) % cap_; // overwrite oldest
    }
}

void AudioSink::render(float* out, int frames)
{
    std::lock_guard<std::mutex> lk(mtx_);

    // Decay the meter when the producer has stopped feeding us (underrun).
    if (count_ < 2)
        level_.store(level_.load() * 0.9f);

    const float vol = muted_.load() ? 0.0f : volume_.load();
    const double step = srcRate_ / devRate_; // input samples consumed per output sample

    for (int i = 0; i < frames; ++i)
    {
        // Underrun: not enough buffered input to interpolate -> output silence.
        if (count_ < 2)
        {
            out[i] = 0.0f;
            continue;
        }

        float s0 = buf_[rpos_];
        float s1 = buf_[(rpos_ + 1) % cap_];
        float frac = (float)readPhase_;
        out[i] = (s0 + (s1 - s0) * frac) * vol;
        lastSample_ = s0;

        readPhase_ += step;
        while (readPhase_ >= 1.0 && count_ >= 1)
        {
            readPhase_ -= 1.0;
            rpos_ = (rpos_ + 1) % cap_;
            --count_;
        }
    }
}
