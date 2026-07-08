// Pager channel decoder: runs POCSAG and FLEX simultaneously and reports
// whichever protocol is present (auto-detection). Posts decoded messages to the
// unified MessageBus. Registered with the channel registry.
#include "decode/channel/channel_registry.h"
#include "decode/channel/message_bus.h"
#include "decode/bch.h"
#include "decode/pocsag_decoder.h"
#include "multimon_ng/multimon_lib.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

double nowSec()
{
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

class PagerChannel : public IChannelDecoder
{
public:
    explicit PagerChannel(const ChannelContext& ctx)
        : bus_(ctx.bus), channelId_(ctx.channelId), freqMHz_(ctx.freqHz / 1e6),
          rate_(ctx.sampleRate)
    {
        bch_init();
        pocsag_ = std::make_unique<PocsagDecoder>();
        pocsag_->setErrorCorrection(2);
        pocsag_->onMessage = [this](uint32_t address, int function,
                                    const std::string& text,
                                    const std::string& numeric, bool /*sync*/) {
            if (!bus_) return;
            DecodedRecord r;
            r.timeSec = nowSec();
            r.channelId = channelId_;
            r.freqMHz = freqMHz_;
            r.source = "POCSAG";
            r.text = !text.empty() ? text : numeric;
            r.fields["addr"] = std::to_string(address);
            r.fields["func"] = std::to_string(function);
            if (!numeric.empty()) r.fields["numeric"] = numeric;
            bus_->post(r);
        };
        pocsag_->onSync = [this]() { locked_.store(true); };
        pocsag_->onLostSync = [this]() { locked_.store(false); };
        pocsag_->onRecoveredSync = [this]() { locked_.store(true); };
        pllFreq_ = (rate_ > 0.0) ? 1200.0 / rate_ : 0.0;

        multimonCtx_ = multimon_create(&PagerChannel::flexCb, this, 22050);
        if (multimonCtx_)
            multimon_enable_decoder(multimonCtx_, MULTIMON_FLEX);
    }

    ~PagerChannel() override
    {
        if (multimonCtx_)
            multimon_destroy(multimonCtx_);
    }

    void process(const double* iq, int n) override
    {
        if (n <= 0) return;
        for (int k = 0; k < n; ++k)
        {
            double I = iq[2 * k], Q = iq[2 * k + 1];
            double di = I * prevI_ + Q * prevQ_;
            double dq = Q * prevI_ - I * prevQ_;
            prevI_ = I; prevQ_ = Q;
            double fm = std::atan2(dq, di) / M_PI;

            // POCSAG: PLL bit-timing recovery + feedBit.
            dcAvg_ = 0.999 * dcAvg_ + 0.001 * fm;
            double dcRemoved = fm - dcAvg_;
            bool prevPos = (prevSample_ >= dcAvg_);
            bool curPos = (dcRemoved >= 0.0);
            if (prevPos != curPos)
            {
                double err = pllPhase_ - 0.5;
                pllPhase_ -= 0.25 * err;
                pllFreq_ -= 0.001 * err;
                double nom = 1200.0 / rate_;
                double lo = nom * 0.35, hi = nom * 3.0;
                if (pllFreq_ < lo) pllFreq_ = lo;
                if (pllFreq_ > hi) pllFreq_ = hi;
            }
            prevSample_ = fm;
            pllPhase_ += pllFreq_;
            if (pllPhase_ >= 1.0)
            {
                pllPhase_ -= 1.0;
                pocsag_->feedBit((dcRemoved >= 0.0) ? 1 : 0);
            }

            // FLEX: buffer FM samples for resampling to 22050.
            if (fm < -1.0) fm = -1.0;
            if (fm > 1.0) fm = 1.0;
            flexFmBuf_.push_back((float)fm);
        }

        locked_.store(pocsag_->inSync());

        if (!flexFmBuf_.empty() && multimonCtx_)
        {
            double ratio = rate_ / 22050.0;
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

    bool locked() const override { return locked_.load(); }

private:
    static void flexCb(const char* decoder_name, const char* message, void* user)
    {
        if (!message || !decoder_name) return;
        if (strcmp(decoder_name, "FLEX") != 0) return;
        static_cast<PagerChannel*>(user)->onFlex(message);
    }

    void onFlex(const char* message)
    {
        const char* p = message;
        while (*p && *p != ':' && *p != '|') p++;
        if (*p == ':' || *p == '|') p++;

        int64_t capcode = 0;
        std::string type, text;
        if (strchr(p, '|')) {
            for (int f = 0; f < 3; ++f) { while (*p && *p != '|') p++; if (*p == '|') p++; }
            capcode = strtoll(p, (char**)&p, 10);
            if (*p == '|') p++;
            const char* typeStr = p;
            while (*p && *p != '|') p++;
            type.assign(typeStr, p - typeStr);
            if (*p == '|') p++;
            text = p;
        } else {
            while (*p && *p != ' ') p++;
            if (*p == ' ') { p++; while (*p && *p != ' ') p++; }
            if (*p == ' ') { p++; while (*p && *p != ' ') p++; }
            while (*p && *p != ' ') p++; if (*p == ' ') p++;
            while (*p && *p != '[' && *p != 0) p++;
            if (*p == '[') { p++; capcode = strtoll(p, (char**)&p, 10); if (*p == ']') p++; }
            while (*p == ' ') p++;
            const char* typeStr = p;
            while (*p && *p != ' ') p++;
            type.assign(typeStr, p - typeStr);
            while (*p == ' ') p++;
            text = p;
        }
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        if (type == "UNK" || type == "UNKNOWN" || text.empty()) return;

        if (!bus_) return;
        DecodedRecord r;
        r.timeSec = nowSec();
        r.channelId = channelId_;
        r.freqMHz = freqMHz_;
        r.source = "FLEX";
        r.text = text;
        r.fields["capcode"] = std::to_string(capcode);
        r.fields["type"] = type;
        bus_->post(r);
    }

    MessageBus* bus_ = nullptr;
    int channelId_ = 0;
    double freqMHz_ = 0.0;
    double rate_ = 48000.0;

    std::unique_ptr<PocsagDecoder> pocsag_;
    multimon_ctx_t* multimonCtx_ = nullptr;
    std::vector<float> flexFmBuf_;
    double flexResampleFrac_ = 0.0;

    double prevI_ = 0.0, prevQ_ = 0.0;
    double pllPhase_ = 0.0, pllFreq_ = 0.0, prevSample_ = 0.0, dcAvg_ = 0.0;
    std::atomic<bool> locked_{false};
};

} // namespace

REGISTER_CHANNEL_DECODER(ChannelDecoderInfo{
    kTypePager, "Pager (POCSAG/FLEX auto)", "Pager",
    /*ddcRate*/ 48000.0, /*ddcBandwidth*/ 25000.0, /*weight*/ 3,
    /*isAudio*/ false, /*dedicatedSubband*/ false,
    [](const ChannelContext& c) { return std::make_unique<PagerChannel>(c); }});
