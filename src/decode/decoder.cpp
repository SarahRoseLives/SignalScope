#include "decode/decoder.h"

#include "decode/bch.h"
#include "decode/egc/egc_decoder.h"
#include "decode/pocsag_decoder.h"
#include "audio/audio_sink.h"
#include "util/log.h"

#include <chrono>
#include <cmath>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double ddcBw(int baud)
{
    // Per-channel DDC passband (double-sided).
    if (baud == kWfmBaud)
        return 200000.0;    // broadcast FM channel
    if (baud == kEgcBaud)
        return 6000.0;      // Inmarsat-C / EGC
    if (baud == 10500)
        return 21000.0;
    if (baud == 8400)
        return 9000.0;
    return 25000.0;         // POCSAG / FLEX / auto
}

// multimon-ng callback for FLEX messages. Parses multimon's text line into
// capcode / type / message and forwards to the owning Decoder.
static void flexMultimonCallback(const char* decoder_name, const char* message, void* user_data)
{
    if (!message || !decoder_name) return;
    if (strcmp(decoder_name, "FLEX") != 0) return;

    Decoder* self = static_cast<Decoder*>(user_data);
    if (!self) return;

    const char* p = message;
    while (*p && *p != ':' && *p != '|') p++;
    if (*p == ':' || *p == '|') p++;

    if (strchr(p, '|')) {
        while (*p && *p != '|') p++; if (*p == '|') p++;
        while (*p && *p != '|') p++; if (*p == '|') p++;
        while (*p && *p != '|') p++; if (*p == '|') p++;
        int64_t capcode = strtoll(p, (char**)&p, 10);
        if (*p == '|') p++;
        const char* typeStr = p;
        while (*p && *p != '|') p++;
        std::string type(typeStr, p - typeStr);
        if (*p == '|') p++;
        std::string text(p);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        if (type == "UNK" || type == "UNKNOWN" || text.empty()) return;
        self->onFlexMessage(capcode, type, text);
    } else {
        while (*p && *p != ' ') p++;
        if (*p == ' ') { p++; while (*p && *p != ' ') p++; }
        if (*p == ' ') { p++; while (*p && *p != ' ') p++; }
        while (*p && *p != ' ') p++; if (*p == ' ') p++;
        while (*p && *p != '[' && *p != 0) p++;
        int64_t capcode = 0;
        if (*p == '[') { p++; capcode = strtoll(p, (char**)&p, 10); if (*p == ']') p++; }
        while (*p == ' ') p++;
        const char* typeStr = p;
        while (*p && *p != ' ') p++;
        std::string type(typeStr, p - typeStr);
        while (*p == ' ') p++;
        std::string text(p);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        if (type == "UNK" || type == "UNKNOWN" || text.empty()) return;
        self->onFlexMessage(capcode, type, text);
    }
}

double Decoder::ddcRate(int baud)
{
    // WFM broadcast needs the full ~200 kHz channel to recover audio; keep the
    // sub-band rate. Everything else runs at the narrow 48 kHz channel IF.
    if (baud == kWfmBaud)
        return 240000.0;
    return 48000.0;
}

Decoder::Decoder(double subRate, double subCenterHz, double chanFreqHz, int baud,
                 int channelId, MessageLog* log, MessageLog* suLog,
                 CassignLog* cassignLog, ChannelTable* netTable, EgcLog* egcLog,
                 AircraftTable* acTable, MesLog* mesLog, LesLog* lesLog,
                 LesFreqTable* lesFreqTable, PagerLog* pagerLog)
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
      egcLog_(egcLog),
      pagerLog_(pagerLog)
{
    if (baud == kEgcBaud)
    {
        egc_ = std::make_unique<EgcDecoder>(channelId, chanFreqHz / 1e6, ddc_.outputRate(),
                                            egcLog_, mesLog, lesLog, lesFreqTable);
    }
    else if (baud == kWfmBaud)
    {
        wfm_ = std::make_unique<WfmDemod>();
        wfm_->configure(ddc_.outputRate(), 48000.0);
        audioBuf_.reserve(8192);
    }
    else
    {
        // Pager path: POCSAG + FLEX. In auto mode both run together and whichever
        // protocol is present produces messages; explicit bauds enable one path.
        if (isAuto() || baud == 512 || baud == 1200 || baud == 2400)
        {
            hasPocsag_ = true;
            bch_init();
            pocsag_ = std::make_unique<PocsagDecoder>();
            pocsag_->setErrorCorrection(2);
            pocsag_->onMessage = [this](uint32_t address, int function,
                                        const std::string& text,
                                        const std::string& numeric, bool sync) {
                (void)sync;
                if (!pagerLog_) return;
                PagerMessage m;
                m.timeSec = std::chrono::duration<double>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                m.channelId = channelId_;
                m.freqMHz = chanFreqHz_ / 1e6;
                m.protocol = 0;
                m.protocolName = "POCSAG";
                m.baud = 1200;
                m.address = address;
                m.function = function;
                m.text = text;
                m.numeric = numeric;
                m.errors = (int)pocsag_->uncorrectedErrors();
                m.inverted = false;
                pagerLog_->add(m);
                msgCount_.fetch_add(1);
            };
            pocsag_->onSync = [this]() { pagerLocked_.store(true); };
            pocsag_->onLostSync = [this]() { pagerLocked_.store(false); };
            pocsag_->onRecoveredSync = [this]() { pagerLocked_.store(true); };

            double outRate = ddc_.outputRate();
            pllFreq_ = (outRate > 0.0) ? 1200.0 / outRate : 0.0;
        }

        if (isAuto() || baud == 1600 || baud == 3200 || baud == 6400)
        {
            hasFlex_ = true;
            multimonCtx_ = multimon_create(flexMultimonCallback, this, 22050);
            if (multimonCtx_)
                multimon_enable_decoder(multimonCtx_, MULTIMON_FLEX);
        }
    }

    ddcOut_.reserve(8192);
}

Decoder::~Decoder()
{
    if (multimonCtx_)
    {
        multimon_destroy(multimonCtx_);
        multimonCtx_ = nullptr;
    }
}

void Decoder::process(const double* iq, int nComplex)
{
    if (!iq || nComplex <= 0) return;

    // ---- WFM broadcast audio ----
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

    // ---- Inmarsat-C / EGC ----
    if (egc_)
    {
        ddcOut_.clear();
        ddc_.process(iq, nComplex, ddcOut_);
        if (ddcOut_.empty())
            return;
        int n = (int)(ddcOut_.size() / 2);
        egc_->process(ddcOut_.data(), n);
        msgCount_.store(egc_->messageCount());
        return;
    }

    // ---- Pager (POCSAG + FLEX, auto-detect) ----
    if (!hasPocsag_ && !hasFlex_) return;

    ddcOut_.clear();
    ddc_.process(iq, nComplex, ddcOut_);
    int n = (int)(ddcOut_.size() / 2);
    if (n <= 0) return;

    // FM discriminate — shared by both POCSAG and FLEX paths.
    for (int k = 0; k < n; ++k)
    {
        double I = ddcOut_[2 * k];
        double Q = ddcOut_[2 * k + 1];
        double fm = fmDiscriminate(I, Q, prevI_, prevQ_);

        if (hasPocsag_)
        {
            dcAvg_ = 0.999 * dcAvg_ + 0.001 * fm;
            double dcRemoved = fm - dcAvg_;
            env_ = 0.999 * env_ + 0.001 * fabs(dcRemoved) * 3.0;
            if (env_ < 0.01) env_ = 0.01;

            bool prevPos = (prevSample_ >= dcAvg_);
            bool curPos = (dcRemoved >= 0.0);
            if (prevPos != curPos)
            {
                double err = pllPhase_ - 0.5;
                pllPhase_ -= 0.25 * err;
                pllFreq_ -= 0.001 * err;
                double nom = 1200.0 / ddc_.outputRate();
                double lo = nom * 0.35, hi = nom * 3.0;
                if (pllFreq_ < lo) pllFreq_ = lo;
                if (pllFreq_ > hi) pllFreq_ = hi;
            }
            prevSample_ = fm;

            pllPhase_ += pllFreq_;
            if (pllPhase_ >= 1.0)
            {
                pllPhase_ -= 1.0;
                int sym = (dcRemoved >= 0.0) ? 1 : 0;
                if (pocsag_) pocsag_->feedBit(sym);
            }
        }

        if (hasFlex_)
        {
            if (fm < -1.0) fm = -1.0;
            if (fm > 1.0) fm = 1.0;
            flexFmBuf_.push_back((float)fm);
        }
    }

    if (hasPocsag_ && pocsag_)
        pagerLocked_.store(pocsag_->inSync());

    // Resample accumulated FLEX samples to 22050 Hz for multimon-ng.
    if (hasFlex_ && !flexFmBuf_.empty() && multimonCtx_)
    {
        double outRate = ddc_.outputRate();
        double ratio = outRate / 22050.0;
        size_t outSamples = (size_t)((double)flexFmBuf_.size() / ratio);
        std::vector<float> outBuf(outSamples);
        for (size_t i = 0; i < outSamples; i++)
        {
            double srcPos = (double)i * ratio + flexResampleFrac_;
            size_t idx = (size_t)srcPos;
            if (idx + 1 < flexFmBuf_.size())
            {
                double t = srcPos - (double)idx;
                outBuf[i] = flexFmBuf_[idx] * (float)(1.0 - t) + flexFmBuf_[idx + 1] * (float)t;
            }
            else if (idx < flexFmBuf_.size())
            {
                outBuf[i] = flexFmBuf_[idx];
            }
        }
        flexResampleFrac_ = fmod((double)flexFmBuf_.size(), ratio);
        flexFmBuf_.clear();

        if (!outBuf.empty())
            multimon_process_float(multimonCtx_, outBuf.data(), outBuf.size());
    }
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
    if (egc_) return egc_->locked();
    return pagerLocked_.load();
}

int Decoder::getConstellation(double* iqOut, int maxPairs) const
{
    return egc_ ? egc_->getConstellation(iqOut, maxPairs) : 0;
}

double Decoder::fmDiscriminate(double i, double q, double& prevI, double& prevQ)
{
    double di = i * prevI + q * prevQ;
    double dq = q * prevI - i * prevQ;
    prevI = i; prevQ = q;
    return atan2(dq, di) / M_PI;
}

int Decoder::egcBer() const { return egc_ ? egc_->lastBer() : -1; }
int Decoder::egcFrames() const { return egc_ ? egc_->framesSynced() : 0; }
int Decoder::egcChannelType() const { return egc_ ? egc_->channelType() : 0; }

double Decoder::wfmAudioRate() const { return wfm_ ? wfm_->audioRate() : 0.0; }

void Decoder::onFlexMessage(int64_t capcode, const std::string& type, const std::string& text)
{
    if (!pagerLog_) return;
    PagerMessage m;
    m.timeSec = std::chrono::duration<double>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
    m.channelId = channelId_;
    m.freqMHz = chanFreqHz_ / 1e6;
    m.protocol = 1;
    m.protocolName = "FLEX";
    m.baud = 3200;
    m.address = (uint32_t)(capcode & 0xFFFFFFFF);
    m.function = (type == "ALN" || type == "ALPHA") ? 5 :
                 (type == "NUM") ? 3 :
                 (type == "TON") ? 2 : 0;
    m.text = text;
    m.numeric = "";
    m.errors = 0;
    m.inverted = false;
    pagerLog_->add(m);
    msgCount_.fetch_add(1);
}
