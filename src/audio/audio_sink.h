// Audio output sink: a single miniaudio playback device fed by a ring buffer.
// The WFM decoder (on a worker thread) pushes mono float samples at its audio
// rate; the device callback (miniaudio thread) pulls and linearly resamples to
// the device rate. Only one channel feeds audio at a time (see DecoderManager).
#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

struct ma_device; // opaque; real type lives in the .cpp

class AudioSink
{
public:
    AudioSink();
    ~AudioSink();

    // Start the playback device (48 kHz mono). Safe to call once; returns false
    // and sets err on failure. Audio stays silent until push() is called.
    bool start(std::string& err);
    void stop();
    bool running() const { return running_.load(); }

    // Declare the sample rate of the audio the producer will push(). Resets the
    // buffer when it changes. Called by the demod owner before pushing.
    void setSourceRate(double hz);

    // Push mono float samples (producer/worker thread).
    void push(const float* mono, int n);

    // Drop any buffered audio (e.g. on channel switch) to avoid stale playback.
    void flush();

    void  setVolume(float v) { volume_.store(v); }
    float volume() const { return volume_.load(); }
    void  setMuted(bool m) { muted_.store(m); }
    bool  muted() const { return muted_.load(); }

    // Post-decimation signal level (0..~1), peak with decay, for a UI meter.
    // Reflects the audio being produced regardless of mute/volume.
    float level() const { return level_.load(); }

    // Pulled by the miniaudio callback. Writes 'frames' mono floats to out.
    void render(float* out, int frames);

private:
    ma_device* device_ = nullptr;
    std::atomic<bool> running_{false};

    std::mutex mtx_;
    std::vector<float> buf_;   // ring of mono samples
    size_t cap_ = 0;
    size_t rpos_ = 0, wpos_ = 0, count_ = 0;

    double srcRate_ = 48000.0;
    double devRate_ = 48000.0;
    double readPhase_ = 0.0;   // fractional read cursor for resampling
    float  lastSample_ = 0.0f;

    std::atomic<float> volume_{0.6f};
    std::atomic<bool>  muted_{false};
    std::atomic<float> level_{0.0f};
};
