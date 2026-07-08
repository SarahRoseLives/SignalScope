// Owns the active decoders, grouped into sub-bands. A sub-band decimates the
// wideband stream ONCE (shared front-end) to a moderate IF; its decoders then
// run cheap per-channel DDCs from that IF. Decoders far apart in frequency get
// their own sub-band. Sub-bands are spread across a worker-thread pool.
#pragma once

#include "decode/decoder.h"
#include "decode/message_log.h"
#include "dsp/ddc.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class AudioSink;

class DecoderManager
{
public:
    struct Status
    {
        int channelId;
        double freqMHz;
        int baud;
        bool locked;
        uint64_t msgs;
        int egcBer;     // -1 unless EGC
        int egcFrames;  // 0 unless EGC
        int egcCType;   // 0=unknown, 1=NCS, 2=LES TDM, 3=Joint, 4=Standby
        bool isB       = false; // from decodersB (dual RTL)
        bool isWfm     = false; // wideband-FM audio channel
        bool audioOn   = false; // currently routed to the speaker
    };

    ~DecoderManager() { stop(); }

    void configure(double Fs, double centerHz);
    void start();
    void stop();

    // Attach the shared audio sink; WFM decoders route their audio here when
    // selected. Set before adding WFM decoders.
    void setAudioSink(AudioSink* sink) { audioSink_ = sink; }

    // Make the given channel the one that plays audio (or 0 to mute all).
    // Only one channel plays at a time.
    void setAudioChannel(int channelId);
    int  audioChannel() const { return audioChannel_.load(); }

    // Cap the worker-thread count (set before start()).
    void setMaxWorkers(int n) { maxWorkers_ = (n > 0) ? n : 1; }

    void feed(const float* iq, int nComplex);

    int addDecoder(double freqHz, int baud, uint32_t aesId = 0);    void removeDecoder(int channelId);
    void setDecoderFreq(int channelId, double freqHz);
    void removeAll();
    int  decoderCount();
    int  workerCount() const { return (int)workers_.size(); }
    int  subbandCount();

    std::vector<Status> status();
    int getConstellation(int channelId, std::vector<float>& out, int maxPairs);
    uint64_t drops() const { return drops_.load(); }
    MessageLog& log() { return log_; }
    MessageLog& suLog() { return suLog_; }
    CassignLog& cassignLog() { return cassign_; }
    ChannelTable& channelTable() { return netTable_; }
    EgcLog& egcLog() { return egcLog_; }
    MesLog& mesLog() { return mesLog_; }
    LesLog& lesLog() { return lesLog_; }
    AircraftTable& aircraftTable() { return acTable_; }
    const AircraftTable& aircraftTable() const { return acTable_; }
    LesFreqTable& lesFreqTable() { return lesFreqTable_; }
    PagerLog& pagerLog() { return pagerLog_; }

private:
    struct SubBand
    {
        SubBand(double Fs, double wideCenterHz, double center, double rateTarget,
                double bw)
            : centerHz(center), wideCenterHz(wideCenterHz),
              frontEnd(Fs, center - wideCenterHz, rateTarget, bw)
        {
            subRate = frontEnd.outputRate();
        }
        // Re-centre the shared front-end on a new absolute frequency (Hz) and
        // re-point every decoder in this sub-band. Used to make a wideband (WFM)
        // channel follow the station instead of aliasing on the fixed IF.
        void recenter(double newCenterHz)
        {
            centerHz = newCenterHz;
            frontEnd.setOffset(newCenterHz - wideCenterHz);
            for (auto& d : decoders)
                d->setSubCenter(newCenterHz);
        }
        double centerHz;
        double wideCenterHz;          // absolute centre of the wideband stream
        double subRate = 0.0;
        Ddc frontEnd;                 // Fs -> subRate (shared by all decoders)
        std::vector<double> subIQ;    // scratch (worker thread only)
        std::vector<std::shared_ptr<Decoder>> decoders;
    };

    struct Worker
    {
        std::thread thread;
        std::mutex qMtx;
        std::condition_variable cv;
        std::deque<std::shared_ptr<const std::vector<float>>> queue;

        std::mutex dMtx; // guards subbands
        std::vector<std::shared_ptr<SubBand>> subbands;
        std::atomic<int> count{0};   // total decoders on this worker
        std::atomic<int> weight{0};  // weighted load (MSK=3, OQPSK=2, EGC=1)
    };

    void workerLoop(Worker* w);

    int addDecoderImpl(double freqHz, int baud, uint32_t aesId);

    double Fs_ = 0.0;
    double centerHz_ = 0.0;

    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<bool> run_{false};

    std::mutex idMtx_;
    int nextId_ = 1;

    std::atomic<uint64_t> drops_{0};
    static constexpr size_t kMaxQueue = 192;
    MessageLog log_;
    MessageLog suLog_;
    CassignLog cassign_;
    ChannelTable netTable_;
    EgcLog egcLog_;
    MesLog mesLog_;
    LesLog lesLog_;
    AircraftTable acTable_;
    LesFreqTable lesFreqTable_;
    int maxWorkers_ = 8;

    PagerLog pagerLog_;
    AudioSink* audioSink_ = nullptr;
    std::atomic<int> audioChannel_{0}; // channelId currently playing audio (0 = none)
};
