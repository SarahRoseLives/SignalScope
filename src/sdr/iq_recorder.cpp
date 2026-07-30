#include "sdr/iq_recorder.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using clock = std::chrono::steady_clock;

void put32(std::FILE* f, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)(v), (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    std::fwrite(b, 1, 4, f);
}
void put16(std::FILE* f, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)(v), (uint8_t)(v >> 8)};
    std::fwrite(b, 1, 2, f);
}

void writeWavHdr(std::FILE* f, double sampleRate)
{
    const uint16_t channels = 2;
    const uint16_t bits = 8;
    const uint32_t byteRate = (uint32_t)(sampleRate * channels * (bits / 8));
    const uint16_t blockAlign = (uint16_t)(channels * (bits / 8));

    std::fwrite("RIFF", 1, 4, f);
    put32(f, 36);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, channels);
    put32(f, (uint32_t)sampleRate);
    put32(f, byteRate);
    put16(f, blockAlign);
    put16(f, bits);
    std::fwrite("data", 1, 4, f);
    put32(f, 0);
}

void patchSizes(std::FILE* f, uint32_t dataBytes)
{
    std::fseek(f, 4, SEEK_SET);
    put32(f, 36 + dataBytes);
    std::fseek(f, 40, SEEK_SET);
    put32(f, dataBytes);
}

} // namespace

void IqRecorder::configurePrebuffer(double sampleRate, double bufferSec)
{
    std::lock_guard<std::mutex> lk(ringMtx_);
    bufferSec_ = bufferSec;
    if (bufferSec <= 0.0 || sampleRate > 3.0e6)
    {
        ring_.clear();
        ring_.shrink_to_fit();
        ringCap_ = 0;
        ringWrite_ = 0;
        return;
    }
    size_t n = (size_t)(sampleRate * bufferSec);
    if (n < 1024) n = 1024;
    ring_.resize(n * 2, 0.0f);
    ringCap_ = n;
    ringWrite_ = 0;
}

void IqRecorder::prebuffer(const float* iq, int nComplex)
{
    if (ringCap_ == 0 || nComplex <= 0)
        return;
    std::lock_guard<std::mutex> lk(ringMtx_);
    for (int i = 0; i < nComplex; ++i)
    {
        size_t w = (ringWrite_ % ringCap_) * 2;
        ring_[w]     = iq[i * 2];
        ring_[w + 1] = iq[i * 2 + 1];
        ++ringWrite_;
    }
}

bool IqRecorder::start(const std::string& path, double sampleRate)
{
    stop();
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return false;
    path_ = path;
    sampleRate_ = sampleRate;

    writeWavHdr(f_, sampleRate);
    dataBytes_ = 0;

    {
        std::lock_guard<std::mutex> lk(ringMtx_);
        if (ringCap_ > 0)
        {
            size_t avail = ringWrite_ < ringCap_ ? ringWrite_ : ringCap_;
            size_t start = ringWrite_ - avail;
            std::vector<uint8_t> buf(avail * 2);
            for (size_t i = 0; i < avail; ++i)
            {
                size_t idx = ((start + i) % ringCap_) * 2;
                float fi = ring_[idx] * 127.0f + 128.0f;
                float fq = ring_[idx + 1] * 127.0f + 128.0f;
                if (fi < 0.0f) fi = 0.0f; if (fi > 255.0f) fi = 255.0f;
                if (fq < 0.0f) fq = 0.0f; if (fq > 255.0f) fq = 255.0f;
                buf[i * 2]     = (uint8_t)fi;
                buf[i * 2 + 1] = (uint8_t)fq;
            }
            std::fwrite(buf.data(), 1, buf.size(), f_);
            dataBytes_ += (uint32_t)buf.size();
        }
    }

    startTime_ = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    recording_.store(true);
    return true;
}

void IqRecorder::write(const float* iq, int nComplex)
{
    if (!f_ || nComplex <= 0) return;
    int nBytes = nComplex * 2;
    std::vector<uint8_t> buf((size_t)nBytes);
    for (int i = 0; i < nComplex; ++i)
    {
        float fi = iq[i * 2] * 127.0f + 128.0f;
        float fq = iq[i * 2 + 1] * 127.0f + 128.0f;
        if (fi < 0.0f) fi = 0.0f; if (fi > 255.0f) fi = 255.0f;
        if (fq < 0.0f) fq = 0.0f; if (fq > 255.0f) fq = 255.0f;
        buf[(size_t)i * 2] = (uint8_t)fi;
        buf[(size_t)i * 2 + 1] = (uint8_t)fq;
    }
    std::fwrite(buf.data(), 1, (size_t)nBytes, f_);
    dataBytes_ += (uint32_t)nBytes;
}

bool IqRecorder::startSelection(const std::string& path, double sampleRate,
                                 double centerHz, double loHz, double hiHz)
{
    stopSelection();
    selF_ = std::fopen(path.c_str(), "wb");
    if (!selF_) return false;

    double bw = hiHz - loHz;
    double offset = (loHz + hiHz) * 0.5 - centerHz;
    double targetRate = bw;

    if (targetRate < 1000.0) targetRate = 1000.0;
    if (targetRate > sampleRate) targetRate = sampleRate;

    ddc_ = std::make_unique<Ddc>(sampleRate, offset, targetRate, bw);

    writeWavHdr(selF_, ddc_->outputRate());
    selDataBytes_ = 0;
    selStartTime_ = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    selRecording_.store(true);
    return true;
}

void IqRecorder::writeSelection(const float* iq, int nComplex)
{
    if (!selF_ || nComplex <= 0 || !ddc_) return;

    std::vector<double> dec;
    ddc_->process(iq, nComplex, dec);

    if (dec.empty()) return;

    int nOut = (int)dec.size() / 2;
    std::vector<uint8_t> buf((size_t)nOut * 2);
    for (int i = 0; i < nOut; ++i)
    {
        float fi = (float)dec[i * 2] * 127.0f + 128.0f;
        float fq = (float)dec[i * 2 + 1] * 127.0f + 128.0f;
        if (fi < 0.0f) fi = 0.0f; if (fi > 255.0f) fi = 255.0f;
        if (fq < 0.0f) fq = 0.0f; if (fq > 255.0f) fq = 255.0f;
        buf[i * 2]     = (uint8_t)fi;
        buf[i * 2 + 1] = (uint8_t)fq;
    }
    std::fwrite(buf.data(), 1, buf.size(), selF_);
    selDataBytes_ += (uint32_t)buf.size();
}

void IqRecorder::stopSelection()
{
    selRecording_.store(false);
    if (selF_)
    {
        patchSizes(selF_, selDataBytes_);
        std::fclose(selF_);
        selF_ = nullptr;
    }
    ddc_.reset();
}

void IqRecorder::stop()
{
    recording_.store(false);
    stopSelection();
    if (f_)
    {
        patchSizes(f_, dataBytes_);
        std::fclose(f_);
        f_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(ringMtx_);
        ringWrite_ = 0;
    }
}

double IqRecorder::elapsed() const
{
    if (!recording_.load()) return 0.0;
    double now = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    return now - startTime_;
}

double IqRecorder::selElapsed() const
{
    if (!selRecording_.load()) return 0.0;
    double now = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    return now - selStartTime_;
}
