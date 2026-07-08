#include "decode/decoder.h"

#include "decode/egc/egc_decoder.h"
#include "audio/audio_sink.h"

static double ddcRate(int baud)
{
    // WFM broadcast needs the full ~200 kHz channel to recover audio; keep the
    // sub-band rate. Everything else runs at the narrow 48 kHz channel IF.
    if (baud == kWfmBaud)
        return 240000.0;
    return 48000.0; // channel IF rate
}

static double ddcBw(int baud)
{
    // Per-channel DDC passband (double-sided).
    if (baud == kWfmBaud)
        return 200000.0; // broadcast FM channel
    if (baud == 10500)
        return 21000.0;
    if (baud == 8400)
        return 9000.0;
    return 6000.0;
}

Decoder::Decoder(double subRate, double subCenterHz, double chanFreqHz, int baud,
                 int channelId, MessageLog* log, MessageLog* suLog,
                 CassignLog* cassignLog, ChannelTable* netTable, EgcLog* egcLog,
                 AircraftTable* acTable, MesLog* mesLog, LesLog* lesLog,
                 LesFreqTable* lesFreqTable)
    : ddc_(subRate, chanFreqHz - subCenterHz, ddcRate(baud), ddcBw(baud)),
      log_(log),
      suLog_(suLog),
      cassignLog_(cassignLog),
      netTable_(netTable),
      acTable_(acTable),
      subCenterHz_(subCenterHz),
      chanFreqHz_(chanFreqHz),
      baud_(baud),
      channelId_(channelId),
      egcLog_(egcLog)
{
    if (baud == kEgcBaud)
        egc_ = std::make_unique<EgcDecoder>(channelId, chanFreqHz / 1e6, ddc_.outputRate(),
                                            egcLog_, mesLog, lesLog, lesFreqTable);
    else if (baud == kWfmBaud)
    {
        wfm_ = std::make_unique<WfmDemod>();
        wfm_->configure(ddc_.outputRate(), 48000.0);
        audioBuf_.reserve(8192);
    }
    // Any other baud currently has no demodulator. A decode module can be wired
    // in here later; it would consume the ddc_ output.
    ddcOut_.reserve(8192);
}

Decoder::~Decoder() = default;

void Decoder::process(const double* iq, int nComplex)
{
    if (wfm_)
    {
        ddcOut_.clear();
        ddc_.process(iq, nComplex, ddcOut_);
        if (ddcOut_.empty())
            return;
        int n = (int)(ddcOut_.size() / 2);
        if (audioActive_.load() && audioSink_)
        {
            audioBuf_.clear();
            wfm_->process(ddcOut_.data(), n, audioBuf_);
            if (!audioBuf_.empty())
                audioSink_->push(audioBuf_.data(), (int)audioBuf_.size());
        }
        return;
    }
    if (!egc_)
        return; // no demodulator for this baud
    ddcOut_.clear();
    ddc_.process(iq, nComplex, ddcOut_);
    if (ddcOut_.empty())
        return;
    int n = (int)(ddcOut_.size() / 2);
    egc_->process(ddcOut_.data(), n);
    msgCount_.store(egc_->messageCount());
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

bool Decoder::locked() const
{
    return egc_ ? egc_->locked() : false;
}

int Decoder::getConstellation(double* iqOut, int maxPairs) const
{
    return egc_ ? egc_->getConstellation(iqOut, maxPairs) : 0;
}

int Decoder::egcBer() const { return egc_ ? egc_->lastBer() : -1; }
int Decoder::egcFrames() const { return egc_ ? egc_->framesSynced() : 0; }
int Decoder::egcChannelType() const { return egc_ ? egc_->channelType() : 0; }

double Decoder::wfmAudioRate() const { return wfm_ ? wfm_->audioRate() : 0.0; }
