// A single narrowband channel: a per-channel DDC feeding one pluggable
// IChannelDecoder (selected by type id from the channel registry). The concrete
// demod/decode logic lives in decode/channel/*.cpp — this class is just the host
// that down-converts and forwards samples.
#pragma once

#include "decode/channel/channel_registry.h"
#include "decode/channel/ichannel_decoder.h"
#include "dsp/ddc.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

class MessageBus;
class AudioSink;

class Decoder
{
public:
    // subRate/subCenterHz describe the shared front-end sub-band stream this
    // decoder consumes; chanFreqHz is the absolute channel frequency; typeId
    // selects the registered decoder type.
    Decoder(double subRate, double subCenterHz, double chanFreqHz, int typeId,
            int channelId, MessageBus* bus, AudioSink* audio);
    ~Decoder();

    // Process a block of sub-band interleaved double IQ (decode thread).
    void process(const double* iq, int nComplex);

    // Retune to a new absolute channel frequency (Hz).
    void setFreq(double chanFreqHz);

    // Re-point this decoder at a new sub-band centre (Hz). Used when the shared
    // front-end DDC is re-centred to follow a wideband channel.
    void setSubCenter(double subCenterHz);

    bool   locked() const { return impl_ ? impl_->locked() : false; }
    double freqMHz() const { return chanFreqHz_ / 1e6; }
    int    typeId() const { return typeId_; }
    int    channelId() const { return channelId_; }

    // Audio routing (WFM/AM/NFM types).
    bool   isAudio() const { return isAudio_; }
    double audioRate() const { return impl_ ? impl_->audioRate() : 0.0; }
    void   setAudioActive(bool on) { if (impl_) impl_->setAudioActive(on); }
    bool   audioActive() const { return impl_ ? impl_->audioActive() : false; }

private:
    Ddc ddc_;
    std::vector<double> ddcOut_;

    double subCenterHz_;
    double chanFreqHz_;
    int typeId_;
    int channelId_;
    bool isAudio_ = false;

    std::unique_ptr<IChannelDecoder> impl_;
};
