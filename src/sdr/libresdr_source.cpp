#include "sdr/libresdr_source.h"

#include "util/log.h"

#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/usrp/subdev_spec.hpp>
#include <uhd/stream.hpp>
#include <uhd/types/tune_request.hpp>
#include <uhd/types/device_addr.hpp>
#include <uhd/device.hpp>
#include <uhd/exception.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Real UHD handles live here so the public header stays UHD/Boost-free.
struct LibreSdrSource::Impl
{
    uhd::usrp::multi_usrp::sptr usrp;
    uhd::rx_streamer::sptr      stream;
};

namespace {

bool dirHasImage(const std::string& dir)
{
    std::string p = dir + "/usrp_b200_fw.hex";
    if (FILE* f = std::fopen(p.c_str(), "rb")) { std::fclose(f); return true; }
    return false;
}

#ifdef _WIN32
// Directory containing the running executable (UTF-8).
std::string exeDir()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring w(buf, n);
    size_t slash = w.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    w.resize(slash);
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string s((size_t)len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
}
#endif

// UHD needs its firmware/FPGA images (usrp_b200_fw.hex) to bring up the B200.
// When libuhd.dll loads from next to our exe, UHD's built-in search (relative to
// the DLL) misses them and find()/make() report "No UHD Devices Found". Point
// UHD_IMAGES_DIR at the images we bundle next to the exe. Runs once; respects an
// already-valid UHD_IMAGES_DIR. UHD re-reads the env var on each call.
void ensureUhdImagesDir()
{
    static bool done = false;
    if (done) return;
    done = true;

    if (const char* env = std::getenv("UHD_IMAGES_DIR"))
        if (env[0] && dirHasImage(env))
            return;

    std::vector<std::string> candidates;
#ifdef _WIN32
    std::string exe = exeDir();
    if (!exe.empty())
    {
        candidates.push_back(exe + "/uhd-images");
        candidates.push_back(exe + "/../share/uhd/images");
    }
#endif
    candidates.push_back("uhd-images");
    candidates.push_back("../uhd-images");

    for (const std::string& c : candidates)
    {
        if (dirHasImage(c))
        {
#ifdef _WIN32
            _putenv_s("UHD_IMAGES_DIR", c.c_str());
#else
            setenv("UHD_IMAGES_DIR", c.c_str(), 1);
#endif
            logWrite("LibreSDR: UHD_IMAGES_DIR=%s", c.c_str());
            return;
        }
    }
    logWrite("LibreSDR: UHD images dir not found; firmware load may fail");
}

// Resolve the FPGA image path, trying the given path and a "../" fallback so it
// works whether the app runs from the build dir or the project root.
std::string resolveFpga(const std::string& path)
{
    std::string alt = "../" + path;
    for (const std::string& c : {path, alt})
    {
        if (FILE* f = std::fopen(c.c_str(), "rb"))
        {
            std::fclose(f);
            return c;
        }
    }
    return path; // let UHD report a clear error if truly missing
}
} // namespace

LibreSdrSource::LibreSdrSource() : impl_(new Impl()) {}

LibreSdrSource::~LibreSdrSource()
{
    stop();
}

std::vector<SdrDeviceInfo> LibreSdrSource::listDevices()
{
    ensureUhdImagesDir();
    std::vector<SdrDeviceInfo> out;
    try
    {
        uhd::device_addr_t hint;
        hint["type"] = "b200";
        uhd::device_addrs_t found = uhd::device::find(hint);
        for (size_t i = 0; i < found.size(); ++i)
        {
            SdrDeviceInfo info;
            info.index = (int)i;
            info.name = found[i].has_key("product") ? found[i]["product"] : "LibreSDR";
            info.serial = found[i].has_key("serial") ? found[i]["serial"] : "";
            out.push_back(info);
        }
    }
    catch (const std::exception& e)
    {
        logWrite("LibreSDR find failed: %s", e.what());
    }
    return out;
}

bool LibreSdrSource::prepare(int deviceIndex, std::string& err)
{
    if (running_.load() || prepared_)
        return true;

    ensureUhdImagesDir();
    std::string fpga = resolveFpga(fpgaPath_);

    try
    {
        uhd::device_addr_t args;
        args["type"] = "b200";
        args["fpga"] = fpga; // load OUR bitstream onto the device at make()

        // Select a specific device by discovery order when more than one.
        uhd::device_addr_t hint;
        hint["type"] = "b200";
        uhd::device_addrs_t found = uhd::device::find(hint);
        if (found.empty())
        {
            err = "No LibreSDR/B210 device found";
            return false;
        }
        if (deviceIndex < 0 || deviceIndex >= (int)found.size())
            deviceIndex = 0;
        if (found[deviceIndex].has_key("serial"))
            args["serial"] = found[deviceIndex]["serial"];

        logWrite("LibreSDR: opening via UHD (fpga=%s)", fpga.c_str());
        impl_->usrp = uhd::usrp::multi_usrp::make(args);

        // Map the housing port label to a B210 frontend (subdev) + antenna port.
        //   0=TRXA -> A:A / TX/RX   1=RXA -> A:A / RX2
        //   2=TRXB -> A:B / TX/RX   3=RXB -> A:B / RX2
        const char* subdev = (port_ >= 2) ? "A:B" : "A:A";
        const char* antPort = (port_ == 0 || port_ == 2) ? "TX/RX" : "RX2";
        try
        {
            impl_->usrp->set_rx_subdev_spec(uhd::usrp::subdev_spec_t(subdev));
        }
        catch (const std::exception& e) { logWrite("LibreSDR set_rx_subdev_spec: %s", e.what()); }

        impl_->usrp->set_rx_rate(sampleRate_);
        sampleRate_ = impl_->usrp->get_rx_rate();

        impl_->usrp->set_rx_freq(uhd::tune_request_t(centerFreq_));
        centerFreq_ = impl_->usrp->get_rx_freq();

        if (gainDb_ < 0.0)
        {
            try { impl_->usrp->set_rx_agc(true); }
            catch (const std::exception&) { impl_->usrp->set_rx_gain(40.0); }
        }
        else
        {
            impl_->usrp->set_rx_gain(gainDb_);
        }

        double bw = bandwidth_ > 0.0 ? bandwidth_ : sampleRate_;
        impl_->usrp->set_rx_bandwidth(bw);

        try { impl_->usrp->set_rx_antenna(antPort); }
        catch (const std::exception&) { /* antenna name not supported */ }

        uhd::stream_args_t sargs("fc32", "sc16");
        impl_->stream = impl_->usrp->get_rx_stream(sargs);
    }
    catch (const std::exception& e)
    {
        err = std::string("LibreSDR open failed: ") + e.what();
        if (impl_) { impl_->stream.reset(); impl_->usrp.reset(); }
        return false;
    }

    prepared_ = true;
    return true;
}

bool LibreSdrSource::start(int deviceIndex, SdrSampleCb cb, std::string& err)
{
    if (running_.load())
        return true;

    // Open the device if prepare() was not called ahead of time (e.g. the
    // generic/synchronous start path). When prepare() already ran on a worker
    // thread this is a no-op and streaming begins immediately.
    if (!prepared_ && !prepare(deviceIndex, err))
        return false;

    cb_ = std::move(cb);
    dcOffRe_ = dcOffIm_ = 0.0f;
    dcRate_ = (float)(50.0 / sampleRate_);
    stopReq_.store(false);
    running_.store(true);

    try
    {
        uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
        cmd.stream_now = true;
        impl_->stream->issue_stream_cmd(cmd);
    }
    catch (const std::exception& e)
    {
        err = std::string("LibreSDR stream start failed: ") + e.what();
        running_.store(false);
        prepared_ = false;
        impl_->stream.reset();
        impl_->usrp.reset();
        cb_ = nullptr;
        return false;
    }

    prepared_ = false; // consumed
    rxThread_ = std::thread([this]() { rxLoop(); });
    return true;
}

void LibreSdrSource::stop()
{
    stopReq_.store(true);
    if (impl_ && impl_->stream)
    {
        try
        {
            uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
            impl_->stream->issue_stream_cmd(cmd);
        }
        catch (const std::exception&) {}
    }
    if (rxThread_.joinable())
        rxThread_.join();

    running_.store(false);
    cb_ = nullptr;
    prepared_ = false;
    if (impl_)
    {
        impl_->stream.reset();
        impl_->usrp.reset();
    }
}

void LibreSdrSource::rxLoop()
{
    const size_t maxSamps = impl_->stream->get_max_num_samps();
    std::vector<std::complex<float>> buff(maxSamps);
    uhd::rx_metadata_t md;

    while (!stopReq_.load())
    {
        size_t n = 0;
        try
        {
            n = impl_->stream->recv(&buff.front(), buff.size(), md, 1.0);
        }
        catch (const std::exception& e)
        {
            logWrite("LibreSDR recv exception: %s", e.what());
            break;
        }

        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT)
            continue;
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW)
            continue; // UHD auto-recovers; dropped samples are expected under load
        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE)
        {
            logWrite("LibreSDR recv error: %s", md.strerror().c_str());
            continue;
        }
        if (n == 0 || !cb_)
            continue;

        if (scratch_.size() < n * 2)
            scratch_.resize(n * 2);
        float* out = scratch_.data();

        if (dcBlock_.load())
        {
            float offRe = dcOffRe_, offIm = dcOffIm_;
            const float rate = dcRate_;
            for (size_t i = 0; i < n; ++i)
            {
                float re = buff[i].real();
                float im = buff[i].imag();
                float ore = re - offRe;
                offRe += ore * rate;
                float oim = im - offIm;
                offIm += oim * rate;
                out[i * 2]     = ore;
                out[i * 2 + 1] = oim;
            }
            dcOffRe_ = offRe;
            dcOffIm_ = offIm;
        }
        else
        {
            for (size_t i = 0; i < n; ++i)
            {
                out[i * 2]     = buff[i].real();
                out[i * 2 + 1] = buff[i].imag();
            }
        }

        cb_(out, (int)n);
    }
}

void LibreSdrSource::setCenterFreq(double hz)
{
    centerFreq_ = hz;
    std::lock_guard<std::mutex> lk(ctrlMutex_);
    if (impl_ && impl_->usrp)
    {
        try
        {
            impl_->usrp->set_rx_freq(uhd::tune_request_t(hz));
            centerFreq_ = impl_->usrp->get_rx_freq();
        }
        catch (const std::exception& e) { logWrite("LibreSDR set_rx_freq: %s", e.what()); }
    }
}

void LibreSdrSource::setSampleRate(double hz)
{
    sampleRate_ = hz;
    std::lock_guard<std::mutex> lk(ctrlMutex_);
    if (!impl_ || !impl_->usrp)
        return;

    // Changing the RX rate rebuilds the B200's streaming DSP chain. Doing that
    // while the rx thread is blocked in recv() corrupts the streamer and
    // crashes, so pause streaming first: stop the stream, join the rx thread
    // (no one is in recv() after this), then reconfigure and resume.
    const bool wasStreaming = running_.load() && rxThread_.joinable();
    if (wasStreaming)
    {
        stopReq_.store(true);
        try
        {
            uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
            impl_->stream->issue_stream_cmd(cmd);
        }
        catch (const std::exception&) {}
        rxThread_.join();
    }

    try
    {
        impl_->usrp->set_rx_rate(hz);
        sampleRate_ = impl_->usrp->get_rx_rate();
        double bw = bandwidth_ > 0.0 ? bandwidth_ : sampleRate_;
        impl_->usrp->set_rx_bandwidth(bw);

        // The rx streamer is bound to the old rate/DSP config; rebuild it so the
        // new rate takes effect cleanly.
        uhd::stream_args_t sargs("fc32", "sc16");
        impl_->stream = impl_->usrp->get_rx_stream(sargs);
    }
    catch (const std::exception& e)
    {
        logWrite("LibreSDR set_rx_rate: %s", e.what());
    }

    if (wasStreaming)
    {
        dcOffRe_ = dcOffIm_ = 0.0f;
        dcRate_ = (float)(50.0 / sampleRate_);
        stopReq_.store(false);
        try
        {
            uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
            cmd.stream_now = true;
            impl_->stream->issue_stream_cmd(cmd);
            rxThread_ = std::thread([this]() { rxLoop(); });
        }
        catch (const std::exception& e)
        {
            logWrite("LibreSDR stream restart failed: %s", e.what());
            running_.store(false);
        }
    }
}

void LibreSdrSource::setGain(double db)
{
    gainDb_ = db;
    std::lock_guard<std::mutex> lk(ctrlMutex_);
    if (impl_ && impl_->usrp)
    {
        try
        {
            if (db < 0.0)
                impl_->usrp->set_rx_agc(true);
            else
            {
                impl_->usrp->set_rx_agc(false);
                impl_->usrp->set_rx_gain(db);
            }
        }
        catch (const std::exception& e) { logWrite("LibreSDR set_rx_gain: %s", e.what()); }
    }
}
