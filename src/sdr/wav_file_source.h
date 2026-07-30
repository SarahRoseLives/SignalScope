// WAV file IQ source. Plays back 8-bit (unsigned) or 16-bit (signed) PCM WAV
// captures as complex IQ, feeding the same ring -> FFT -> waterfall path as a
// live SDR. Stereo = I (ch0) / Q (ch1); mono = real signal (Q = 0).
#pragma once

#include "sdr/sdr_source.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

class WavFileSource : public SdrSource
{
public:
    WavFileSource() = default;
    ~WavFileSource() override;

    void setPath(const std::string& path) { path_ = path; }
    void setLoop(bool on) { loop_.store(on); }
    bool loop() const { return loop_.load(); }

    std::vector<SdrDeviceInfo> listDevices() override { return {}; }

    void setCenterFreq(double hz) override { centerFreq_ = hz; }
    void setSampleRate(double hz) override { sampleRate_ = hz; }
    void setGain(double) override {}
    void setBiasTee(bool) override {}
    void setPpm(double) override {}

    double centerFreq() const override { return centerFreq_; }
    double sampleRate() const override { return sampleRate_; }

    int    channels() const { return channels_; }
    int    bits() const { return bits_; }
    double progress() const { return progress_.load(); } // 0..1
    double wavCenterFreq() const { return wavCenterFreq_; }

    // Scrub support
    void seekToFrame(uint64_t frame);
    uint64_t currentFrame() const { return currentFrame_.load(); }
    uint64_t totalFrames() const { return totalFrames_; }
    void setOnSeek(std::function<void()> fn) { onSeek_ = std::move(fn); }

    bool start(int deviceIndex, SdrSampleCb cb, std::string& err) override;
    void stop() override;
    bool running() const override { return running_.load(); }

private:
    void playLoop();

    std::string path_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> loop_{true};
    std::atomic<double> progress_{0.0};
    std::atomic<uint64_t> currentFrame_{0};
    SdrSampleCb cb_;

    std::atomic<bool> seekPending_{false};
    std::atomic<uint64_t> seekTarget_{0};
    std::function<void()> onSeek_;

    double centerFreq_ = 750.0e6;
    double sampleRate_ = 2.4e6;

    int      channels_  = 2;
    int      bits_      = 8;
    uint64_t dataOffset_ = 0;
    uint64_t dataBytes_  = 0;
    uint64_t totalFrames_ = 0;
    double   wavCenterFreq_ = 0.0;
};
