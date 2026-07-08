// A single-channel decoder. Supports the Inmarsat-C / EGC path and a wideband-FM
// audio path; other decoder types can be wired in later. Each decoder
// down-converts its channel from the shared sub-band stream via its own DDC.
#pragma once

#include "decode/message_log.h"
#include "dsp/ddc.h"
#include "dsp/wfm_demod.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

class EgcDecoder;
class LesFreqTable;
class AudioSink;

// Special "baud" code selecting the Inmarsat-C / EGC decoder.
static constexpr int kEgcBaud = 1;

// Special "baud" code for a wideband-FM (broadcast) audio channel. The demod
// output is played through the shared AudioSink when this channel is selected
// as the active audio source.
static constexpr int kWfmBaud = 3;

class Decoder
{
public:
    // subRate/subCenterHz describe the shared front-end sub-band stream this
    // decoder consumes; chanFreqHz is the absolute channel frequency.
    Decoder(double subRate, double subCenterHz, double chanFreqHz, int baud,
            int channelId, MessageLog* log, MessageLog* suLog,
            CassignLog* cassignLog, ChannelTable* netTable, EgcLog* egcLog = nullptr,
            AircraftTable* acTable = nullptr,
            MesLog* mesLog = nullptr, LesLog* lesLog = nullptr,
            LesFreqTable* lesFreqTable = nullptr);
    ~Decoder();

    // Process a block of sub-band interleaved double IQ (decode thread).
    void process(const double* iq, int nComplex);

    // Retune to a new absolute channel frequency (Hz).
    void setFreq(double chanFreqHz);

    bool   locked() const;
    // Copy up to maxPairs constellation points (interleaved I,Q doubles into
    // iqOut, capacity >= 2*maxPairs). Returns the number of pairs written.
    int    getConstellation(double* iqOut, int maxPairs) const;
    double freqMHz() const { return chanFreqHz_ / 1e6; }
    int    baud() const { return baud_; }
    int    channelId() const { return channelId_; }
    uint64_t msgCount() const { return msgCount_.load(); }
    bool   isEgc() const { return baud_ == kEgcBaud; }
    bool   isWfm() const { return baud_ == kWfmBaud; }
    int    egcBer() const;    // -1 if not EGC
    int    egcFrames() const; // 0 if not EGC
    int    egcChannelType() const; // 0=unknown, 1=NCS, 2=LES TDM, 3=Joint, 4=Standby

    // WFM audio: route this decoder's demodulated audio to the given sink when
    // 'on' is true. Only one decoder should have this enabled at a time.
    void setAudioSink(AudioSink* sink) { audioSink_ = sink; }
    void setAudioActive(bool on) { audioActive_.store(on); }
    bool audioActive() const { return audioActive_.load(); }
    double wfmAudioRate() const; // 0 if not a WFM decoder

private:
    Ddc ddc_;
    std::vector<double> ddcOut_;
    MessageLog* log_;
    MessageLog* suLog_;
    CassignLog* cassignLog_;
    ChannelTable* netTable_;
    AircraftTable* acTable_ = nullptr;

    double subCenterHz_;
    double chanFreqHz_;
    int baud_;
    int channelId_;
    std::atomic<uint64_t> msgCount_{0};

    std::unique_ptr<EgcDecoder> egc_; // Inmarsat-C / EGC only
    EgcLog* egcLog_ = nullptr;

    // WFM audio path (baud == kWfmBaud only).
    std::unique_ptr<WfmDemod> wfm_;
    std::vector<float> audioBuf_;
    AudioSink* audioSink_ = nullptr;
    std::atomic<bool> audioActive_{false};
};
