#include "decode/decoder.h"

#include "decode/channel/message_bus.h"

namespace {
// DDC settings for a channel type, falling back to sane narrowband defaults if
// the type isn't registered (shouldn't happen).
struct DdcCfg { double rate; double bw; };
DdcCfg ddcCfgFor(int typeId)
{
    if (const ChannelDecoderInfo* info = ChannelRegistry::instance().byType(typeId))
        return {info->ddcRate, info->ddcBandwidth};
    return {48000.0, 12500.0};
}
} // namespace

Decoder::Decoder(double subRate, double subCenterHz, double chanFreqHz, int typeId,
                 int channelId, MessageBus* bus, AudioSink* audio)
    : ddc_(subRate, chanFreqHz - subCenterHz, ddcCfgFor(typeId).rate, ddcCfgFor(typeId).bw),
      subCenterHz_(subCenterHz),
      chanFreqHz_(chanFreqHz),
      typeId_(typeId),
      channelId_(channelId)
{
    ddcOut_.reserve(8192);

    const ChannelDecoderInfo* info = ChannelRegistry::instance().byType(typeId);
    if (info)
    {
        isAudio_ = info->isAudio;
        ChannelContext ctx;
        ctx.sampleRate = ddc_.outputRate();
        ctx.freqHz = chanFreqHz;
        ctx.channelId = channelId;
        ctx.bus = bus;
        ctx.audio = audio;
        if (info->make)
            impl_ = info->make(ctx);
    }
}

Decoder::~Decoder() = default;

void Decoder::process(const double* iq, int nComplex)
{
    if (!iq || nComplex <= 0 || !impl_)
        return;
    ddcOut_.clear();
    ddc_.process(iq, nComplex, ddcOut_);
    if (ddcOut_.empty())
        return;
    impl_->process(ddcOut_.data(), (int)(ddcOut_.size() / 2));
}

void Decoder::setFreq(double chanFreqHz)
{
    chanFreqHz_ = chanFreqHz;
    ddc_.setOffset(chanFreqHz - subCenterHz_);
}

void Decoder::setSubCenter(double subCenterHz)
{
    subCenterHz_ = subCenterHz;
    ddc_.setOffset(chanFreqHz_ - subCenterHz_);
}
