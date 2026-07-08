// LibreSDR source backend (USRP B210 clone) using UHD.
//
// The LibreSDR enumerates as a USRP B210 (same FX3 firmware + CHDR protocol),
// so UHD drives it directly. On start() we pass fpga=<libresdr_b210.bin> in the
// device args, which makes UHD load our custom bitstream onto the device during
// multi_usrp::make(). UHD handles all streaming/flow-control. The UHD handles
// are hidden behind a PImpl so this header (pulled in via core/app.h) stays
// free of UHD/Boost includes.
#pragma once

#include "sdr/sdr_source.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class LibreSdrSource : public SdrSource
{
public:
    LibreSdrSource();
    ~LibreSdrSource() override;

    std::vector<SdrDeviceInfo> listDevices() override;

    void setCenterFreq(double hz) override;
    void setSampleRate(double hz) override;
    void setGain(double db) override;      // <0 => AGC
    void setBiasTee(bool on) override {}   // N/A on B210
    void setPpm(double ppm) override { ppm_ = ppm; }

    void setDcBlock(bool on) { dcBlock_.store(on); }
    bool dcBlock() const { return dcBlock_.load(); }

    // LibreSDR / B210 specific configuration (applied on next start()).
    void setFpgaImage(const std::string& path) { fpgaPath_ = path; }
    void setAntenna(const std::string& ant) { antenna_ = ant; }
    void setBandwidth(double hz) { bandwidth_ = hz; }

    double centerFreq() const override { return centerFreq_; }
    double sampleRate() const override { return sampleRate_; }

    bool start(int deviceIndex, SdrSampleCb cb, std::string& err) override;
    void stop() override;
    bool running() const override { return running_.load(); }

    // Slow device open (multi_usrp::make() loads firmware + FPGA — several
    // seconds). Safe to call from a worker thread: it touches only this
    // object, no shared app state. start() reuses the prepared device if
    // prepare() already ran, so streaming can begin instantly on the GUI thread.
    bool prepare(int deviceIndex, std::string& err);

private:
    void rxLoop();

    struct Impl;                 // UHD handles (defined in the .cpp)
    std::unique_ptr<Impl> impl_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopReq_{false};
    bool prepared_ = false;      // device opened by prepare(), awaiting start()
    std::thread rxThread_;
    std::mutex  ctrlMutex_;      // serialize live control calls vs. streaming
    SdrSampleCb cb_;

    double centerFreq_ = 1545.0e6;
    double sampleRate_ = 4.0e6;
    double bandwidth_ = 0.0;     // 0 => track sample rate
    double gainDb_ = 40.0;       // <0 => AGC
    double ppm_ = 0.0;
    std::string antenna_ = "RX2";
    std::string fpgaPath_ = "libresdr_b210.bin";

    std::atomic<bool> dcBlock_{true};
    float dcOffRe_ = 0.0f, dcOffIm_ = 0.0f;
    float dcRate_ = 2.0e-5f;

    std::vector<float> scratch_;
};
