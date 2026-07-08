// Owns the active decoders, grouped into sub-bands. A sub-band decimates the
// wideband stream ONCE (shared front-end) to a moderate IF; its decoders then
// run cheap per-channel DDCs from that IF. Decoders far apart in frequency get
// their own sub-band. Sub-bands are spread across a worker-thread pool.
#pragma once

#include "decode/decoder.h"
#include "decode/channel/message_bus.h"
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
        int typeId;
        bool locked;
        bool isB       = false; // from decodersB (dual RTL)
        bool isAudio   = false; // audio channel (WFM/AM/NFM) routable to speaker
        bool audioOn   = false; // currently routed to the speaker
    };

    ~DecoderManager() { stop(); }

    void configure(double Fs, double centerHz);
    void start();
    void stop();

    // Attach the shared audio sink; audio decoders route their output here when
    // selected. Set before adding audio decoders.
    void setAudioSink(AudioSink* sink) { audioSink_ = sink; }

    // Make the given channel the one that plays audio (or 0 to mute all).
    // Only one channel plays at a time.
    void setAudioChannel(int channelId);
    int  audioChannel() const { return audioChannel_.load(); }

    // Cap the worker-thread count (set before start()).
    void setMaxWorkers(int n) { maxWorkers_ = (n > 0) ? n : 1; }

    void feed(const float* iq, int nComplex);

    int  addDecoder(double freqHz, int typeId);
    void removeDecoder(int channelId);
    void setDecoderFreq(int channelId, double freqHz);
    void removeAll();
    int  decoderCount();
    int  workerCount() const { return (int)workers_.size(); }
    int  subbandCount();

    std::vector<Status> status();
    uint64_t drops() const { return drops_.load(); }
    MessageBus& bus() { return bus_; }

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
        // re-point every decoder in this sub-band. Used to make a wideband
        // channel follow the signal instead of aliasing on the fixed IF.
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
        std::atomic<int> weight{0};  // weighted load
    };

    void workerLoop(Worker* w);

    int addDecoderImpl(double freqHz, int typeId);

    double Fs_ = 0.0;
    double centerHz_ = 0.0;

    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<bool> run_{false};

    std::mutex idMtx_;
    int nextId_ = 1;

    std::atomic<uint64_t> drops_{0};
    static constexpr size_t kMaxQueue = 192;
    MessageBus bus_;
    int maxWorkers_ = 8;

    AudioSink* audioSink_ = nullptr;
    std::atomic<int> audioChannel_{0}; // channelId currently playing audio (0 = none)
};
