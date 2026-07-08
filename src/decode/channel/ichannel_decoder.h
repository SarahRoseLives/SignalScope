// Interface every narrowband "channel" decoder implements. A channel decoder
// consumes complex baseband IQ already down-converted to its own IF (the DDC
// output rate declared in its ChannelDecoderInfo) and posts results to the bus
// and/or produces audio for the shared AudioSink.
//
// New decoder types implement this + register a ChannelDecoderInfo (see
// channel_registry.h). No edits to Decoder/DecoderManager/GUI are needed.
#pragma once

class MessageBus;
class AudioSink;

// Everything a channel decoder needs at construction time.
struct ChannelContext
{
    double   sampleRate = 0.0;  // DDC output rate feeding process() (Hz)
    double   freqHz     = 0.0;  // absolute channel centre frequency
    int      channelId  = 0;    // unique id for this channel
    MessageBus* bus     = nullptr;
    AudioSink*  audio   = nullptr;
};

class IChannelDecoder
{
public:
    virtual ~IChannelDecoder() = default;

    // Process nComplex interleaved (I,Q) double samples at ctx.sampleRate.
    virtual void process(const double* iq, int nComplex) = 0;

    // Optional: sync/lock indicator for the UI.
    virtual bool locked() const { return false; }

    // Audio decoders (WFM/AM/NFM) override these so the manager can route one of
    // them to the shared speaker at a time.
    virtual bool   isAudio() const { return false; }
    virtual double audioRate() const { return 0.0; }
    virtual void   setAudioActive(bool /*on*/) {}
    virtual bool   audioActive() const { return false; }
};
