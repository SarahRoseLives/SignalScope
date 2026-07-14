// Audio channel decoders: WFM (broadcast), AM, and NFM. Each demodulates its
// channel to mono audio and pushes to the shared AudioSink when it is the active
// audio channel. Registered with the channel registry so the GUI/manager pick
// them up automatically.
#include "decode/channel/channel_registry.h"
#include "audio/audio_sink.h"
#include "dsp/wfm_demod.h"
#include "dsp/am_nfm_demod.h"

#include <atomic>
#include <memory>
#include <vector>

namespace {

// WFM broadcast: wide channel, decimates internally to 48 kHz.
class WfmChannel : public IChannelDecoder
{
public:
    explicit WfmChannel(const ChannelContext& ctx) : sink_(ctx.audio)
    {
        demod_.configure(ctx.sampleRate, 48000.0);
        audioBuf_.reserve(8192);
    }
    void process(const double* iq, int n) override
    {
        if (!active_.load() || !sink_) return;
        audioBuf_.clear();
        demod_.process(iq, n, audioBuf_);
        if (!audioBuf_.empty())
            sink_->push(audioBuf_.data(), (int)audioBuf_.size());
    }
    bool   isAudio() const override { return true; }
    double audioRate() const override { return demod_.audioRate(); }
    void   setAudioActive(bool on) override { active_.store(on); }
    bool   audioActive() const override { return active_.load(); }
private:
    WfmDemod demod_;
    AudioSink* sink_ = nullptr;
    std::vector<float> audioBuf_;
    std::atomic<bool> active_{false};
};

// AM / NFM: run at the DDC IF rate (no extra decimation).
class AmNfmChannel : public IChannelDecoder
{
public:
    AmNfmChannel(const ChannelContext& ctx, AmNfmDemod::Mode mode) : sink_(ctx.audio)
    {
        demod_.configure(mode, ctx.sampleRate, 5000.0);
        audioBuf_.reserve(8192);
    }
    void process(const double* iq, int n) override
    {
        if (!active_.load() || !sink_) return;
        audioBuf_.clear();
        demod_.process(iq, n, audioBuf_);
        if (!audioBuf_.empty())
            sink_->push(audioBuf_.data(), (int)audioBuf_.size());
    }
    bool   isAudio() const override { return true; }
    double audioRate() const override { return demod_.audioRate(); }
    void   setAudioActive(bool on) override { active_.store(on); }
    bool   audioActive() const override { return active_.load(); }
private:
    AmNfmDemod demod_;
    AudioSink* sink_ = nullptr;
    std::vector<float> audioBuf_;
    std::atomic<bool> active_{false};
};

} // namespace

REGISTER_CHANNEL_DECODER(ChannelDecoderInfo{
    kTypeWfm, "WFM (broadcast audio)", "WFM",
    "Audio",
    /*ddcRate*/ 240000.0, /*ddcBandwidth*/ 200000.0, /*weight*/ 3,
    /*isAudio*/ true, /*dedicatedSubband*/ true,
    [](const ChannelContext& c) { return std::make_unique<WfmChannel>(c); }});

REGISTER_CHANNEL_DECODER(ChannelDecoderInfo{
    kTypeAm, "AM (audio)", "AM",
    "Audio",
    /*ddcRate*/ 48000.0, /*ddcBandwidth*/ 10000.0, /*weight*/ 2,
    /*isAudio*/ true, /*dedicatedSubband*/ false,
    [](const ChannelContext& c) { return std::make_unique<AmNfmChannel>(c, AmNfmDemod::AM); }});

REGISTER_CHANNEL_DECODER(ChannelDecoderInfo{
    kTypeNfm, "NFM (audio)", "NFM",
    "Audio",
    /*ddcRate*/ 48000.0, /*ddcBandwidth*/ 12500.0, /*weight*/ 2,
    /*isAudio*/ true, /*dedicatedSubband*/ false,
    [](const ChannelContext& c) { return std::make_unique<AmNfmChannel>(c, AmNfmDemod::NFM); }});
