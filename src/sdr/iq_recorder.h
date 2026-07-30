// IQ recorder: writes live IQ samples to a WAV file in 8-bit stereo format.
// Includes an optional pre-buffer ring that captures the last N seconds of IQ
// so recording always gets the moments just before the user hit Record.
// Also supports VFO-based band-slice recording via DDC.
#pragma once

#include "dsp/ddc.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class IqRecorder
{
public:
    IqRecorder() = default;
    ~IqRecorder() { stop(); }

    IqRecorder(const IqRecorder&) = delete;
    IqRecorder& operator=(const IqRecorder&) = delete;

    void configurePrebuffer(double sampleRate, double bufferSec);
    void prebuffer(const float* iq, int nComplex);

    // Full-bandwidth recording
    bool start(const std::string& path, double sampleRate);
    void write(const float* iq, int nComplex);

    // DDC-sliced sub-band recording
    bool startSelection(const std::string& path, double sampleRate,
                        double centerHz, double loHz, double hiHz);
    void writeSelection(const float* iq, int nComplex);
    void stopSelection();

    void stop();

    bool isRecording() const { return recording_.load(); }
    bool isRecordingSelection() const { return selRecording_.load(); }
    double elapsed() const;
    const std::string& path() const { return path_; }

    double prebufferSec() const { return bufferSec_; }
    size_t prebufferBytes() const { return ring_.capacity() * sizeof(float); }

    double selSampleRate() const { return ddc_ ? ddc_->outputRate() : 0.0; }
    double selElapsed() const;

private:
    std::atomic<bool> recording_{false};
    std::string path_;
    std::FILE* f_ = nullptr;
    double sampleRate_ = 0.0;
    double startTime_ = 0.0;
    uint32_t dataBytes_ = 0;

    // Selection (DDC slice) recording
    std::atomic<bool> selRecording_{false};
    std::FILE* selF_ = nullptr;
    double selStartTime_ = 0.0;
    uint32_t selDataBytes_ = 0;
    std::unique_ptr<Ddc> ddc_;

    std::mutex ringMtx_;
    std::vector<float> ring_;
    size_t ringCap_ = 0;
    size_t ringWrite_ = 0;
    double bufferSec_ = 0.0;
};
