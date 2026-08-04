// APRS channel decoder.
// Signal chain: FM demodulation → bandpass FIR filter → Goertzel tone
// correlators (1200/2200 Hz) → zero-crossing bit recovery → AX.25 HDLC
// frame assembly → CRC-16 → APRS payload parsing → MessageBus.
// AFSK + AX.25 ported from WaveGate (src/sdr/afsk_demod.cpp).
#include "decode/channel/channel_registry.h"
#include "decode/channel/message_bus.h"
#include "decode/aprs_parser.h"

#include <algorithm>
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

double nowSec() {
    return std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ═══════════════════════════════════════════════════════════════════
// CRC-16 CCITT (from WaveGate utils/crc16.h)
// ═══════════════════════════════════════════════════════════════════

static constexpr uint16_t CRC_CCITT_TABLE[256] = {
    0x0000,0x1189,0x2312,0x329B,0x4624,0x57AD,0x6536,0x74BF,
    0x8C48,0x9DC1,0xAF5A,0xBED3,0xCA6C,0xDBE5,0xE97E,0xF8F7,
    0x1081,0x0108,0x3393,0x221A,0x56A5,0x472C,0x75B7,0x643E,
    0x9CC9,0x8D40,0xBFDB,0xAE52,0xDAED,0xCB64,0xF9FF,0xE876,
    0x2102,0x308B,0x0210,0x1399,0x6726,0x76AF,0x4434,0x55BD,
    0xAD4A,0xBCC3,0x8E58,0x9FD1,0xEB6E,0xFAE7,0xC87C,0xD9F5,
    0x3183,0x200A,0x1291,0x0318,0x77A7,0x662E,0x54B5,0x453C,
    0xBDCB,0xAC42,0x9ED9,0x8F50,0xFBEF,0xEA66,0xD8FD,0xC974,
    0x4204,0x538D,0x6116,0x709F,0x0420,0x15A9,0x2732,0x36BB,
    0xCE4C,0xDFC5,0xED5E,0xFCD7,0x8868,0x99E1,0xAB7A,0xBAF3,
    0x5285,0x430C,0x7197,0x601E,0x14A1,0x0528,0x37B3,0x263A,
    0xDECD,0xCF44,0xFDDF,0xEC56,0x98E9,0x8960,0xBBFB,0xAA72,
    0x6306,0x728F,0x4014,0x519D,0x2522,0x34AB,0x0630,0x17B9,
    0xEF4E,0xFEC7,0xCC5C,0xDDD5,0xA96A,0xB8E3,0x8A78,0x9BF1,
    0x7387,0x620E,0x5095,0x411C,0x35A3,0x242A,0x16B1,0x0738,
    0xFFCF,0xEE46,0xDCDD,0xCD54,0xB9EB,0xA862,0x9AF9,0x8B70,
    0x8408,0x9581,0xA71A,0xB693,0xC22C,0xD3A5,0xE13E,0xF0B7,
    0x0840,0x19C9,0x2B52,0x3ADB,0x4E64,0x5FED,0x6D76,0x7CFF,
    0x9489,0x8500,0xB79B,0xA612,0xD2AD,0xC324,0xF1BF,0xE036,
    0x18C1,0x0948,0x3BD3,0x2A5A,0x5EE5,0x4F6C,0x7DF7,0x6C7E,
    0xA50A,0xB483,0x8618,0x9791,0xE32E,0xF2A7,0xC03C,0xD1B5,
    0x2942,0x38CB,0x0A50,0x1BD9,0x6F66,0x7EEF,0x4C74,0x5DFD,
    0xB58B,0xA402,0x9699,0x8710,0xF3AF,0xE226,0xD0BD,0xC134,
    0x39C3,0x284A,0x1AD1,0x0B58,0x7FE7,0x6E6E,0x5CF5,0x4D7C,
    0xC60C,0xD785,0xE51E,0xF497,0x8028,0x91A1,0xA33A,0xB2B3,
    0x4A44,0x5BCD,0x6956,0x78DF,0x0C60,0x1DE9,0x2F72,0x3EFB,
    0xD68D,0xC704,0xF59F,0xE416,0x90A9,0x8120,0xB3BB,0xA232,
    0x5AC5,0x4B4C,0x79D7,0x685E,0x1CE1,0x0D68,0x3FF3,0x2E7A,
    0xE70E,0xF687,0xC41C,0xD595,0xA12A,0xB0A3,0x8238,0x93B1,
    0x6B46,0x7ACF,0x4854,0x59DD,0x2D62,0x3CEB,0x0E70,0x1FF9,
    0xF78F,0xE606,0xD49D,0xC514,0xB1AB,0xA022,0x92B9,0x8330,
    0x7BC7,0x6A4E,0x58D5,0x495C,0x3DE3,0x2C6A,0x1EF1,0x0F78
};

static inline uint16_t crc16_update(uint16_t crc, uint8_t byte) {
    return (uint16_t)((crc >> 8) ^ CRC_CCITT_TABLE[(crc ^ byte) & 0xFF]);
}

static constexpr uint16_t CRC16_GOOD = 0xF0B8;

// ═══════════════════════════════════════════════════════════════════
// Simple FIR filter (from WaveGate utils/fir_filter.h)
// ═══════════════════════════════════════════════════════════════════

class FirFilter {
public:
    FirFilter() = default;
    FirFilter(std::vector<float> coeffs)
        : coeffs_(std::move(coeffs)), state_(coeffs_.size(), 0.0f) {}

    float process(float sample) {
        if (coeffs_.empty()) return sample;
        state_[idx_] = sample;
        idx_ = (idx_ + 1) % state_.size();
        float r = 0.0f;
        int j = (int)idx_;
        for (size_t i = 0; i < coeffs_.size(); i++) {
            if (--j < 0) j = (int)state_.size() - 1;
            r += state_[j] * coeffs_[i];
        }
        return r;
    }

    size_t size() const { return coeffs_.size(); }

private:
    std::vector<float> coeffs_;
    std::vector<float> state_;
    size_t idx_{0};
};

// Generate a Hann window
static std::vector<float> hann_window(int N) {
    std::vector<float> w(N);
    for (int i = 0; i < N; i++)
        w[i] = 0.5f - 0.5f * std::cos(2.0f * (float)M_PI * i / (N - 1));
    return w;
}

// Bandpass FIR filter coefficients
static std::vector<float> make_bandpass(int N, float lo, float hi, float fs) {
    auto w = hann_window(N);
    std::vector<float> h(N);
    float fl = lo / fs, fh = hi / fs;
    for (int i = 0; i < N; i++) {
        float t = (float)i - (N - 1) / 2.0f;
        float sl = (t == 0.0f) ? 2.0f * fl
                                : std::sin(2.0f * (float)M_PI * fl * t) / ((float)M_PI * t);
        float sh = (t == 0.0f) ? 2.0f * fh
                                : std::sin(2.0f * (float)M_PI * fh * t) / ((float)M_PI * t);
        h[i] = (sh - sl) * w[i];
    }
    float peak = 0.0f;
    for (auto v : h) if (std::abs(v) > peak) peak = std::abs(v);
    if (peak > 0.0f) for (auto& v : h) v /= peak;
    return h;
}

// Lowpass FIR filter coefficients
static std::vector<float> make_lowpass(int N, float cutoff, float fs) {
    auto w = hann_window(N);
    std::vector<float> h(N);
    float fc = cutoff / fs;
    for (int i = 0; i < N; i++) {
        float t = (float)i - (N - 1) / 2.0f;
        float s = (t == 0.0f) ? 2.0f * fc
                              : std::sin(2.0f * (float)M_PI * fc * t) / ((float)M_PI * t);
        h[i] = s * w[i];
    }
    float sum = 0.0f;
    for (auto v : h) sum += v;
    if (sum > 0.0f) for (auto& v : h) v /= sum;
    return h;
}

// ═══════════════════════════════════════════════════════════════════
// AFSK Demodulator + AX.25 Frame Decoder
// ═══════════════════════════════════════════════════════════════════

class AfskDemodulator {
public:
    using PacketCb = std::function<void(const std::vector<uint8_t>&)>;

    AfskDemodulator(int sampleRate, float baudRate, float markHz, float spaceHz)
        : sr_(sampleRate), spb_(sampleRate / baudRate)
    {
        int len = (int)std::floor(spb_);
        if (len < 9) len = 9;
        if (len > 255) len = 255;
        if ((len & 1) == 0) len--;

        float spacing = std::abs(spaceHz - markHz);
        float loHz = std::min(markHz, spaceHz) - spacing;
        float hiHz = std::max(markHz, spaceHz) + spacing;

        bpFilter_ = FirFilter(make_bandpass(len, loHz, hiHz, (float)sr_));
        lpFilter_ = FirFilter(make_lowpass(len, baudRate * 0.75f, (float)sr_));

        int corrLen = (int)std::floor(spb_);
        c0r_.resize(corrLen, 0.0f);
        c0i_.resize(corrLen, 0.0f);
        c1r_.resize(corrLen, 0.0f);
        c1i_.resize(corrLen, 0.0f);
        xBuf_.resize((size_t)len, 0.0f);
        diffBuf_.resize((size_t)len, 0.0f);

        phi0_ = 2.0f * (float)M_PI * markHz  / sr_;
        phi1_ = 2.0f * (float)M_PI * spaceHz / sr_;
    }

    void setCallback(PacketCb cb) { cb_ = std::move(cb); }
    int packetCount() const { return packets_; }

    void addSample(float s) {
        // Bandpass filter (isolate AFSK tones)
        float x = bpFilter_.process(s);

        // Goertzel correlators at mark and space frequencies
        c0r_[jcorr_] = x * std::cos(ph0_);
        c0i_[jcorr_] = x * std::sin(ph0_);
        c1r_[jcorr_] = x * std::cos(ph1_);
        c1i_[jcorr_] = x * std::sin(ph1_);

        ph0_ += phi0_;
        if (ph0_ > 2.0f * (float)M_PI) ph0_ -= 2.0f * (float)M_PI;
        ph1_ += phi1_;
        if (ph1_ > 2.0f * (float)M_PI) ph1_ -= 2.0f * (float)M_PI;

        float cr = sumCirc(c0r_, jcorr_);
        float ci = sumCirc(c0i_, jcorr_);
        float c0 = std::sqrt(cr * cr + ci * ci);
        cr = sumCirc(c1r_, jcorr_);
        ci = sumCirc(c1i_, jcorr_);
        float c1 = std::sqrt(cr * cr + ci * ci);

        diffBuf_[jcd_] = c0 - c1;
        float fdiff = lpFilter_.process(diffBuf_[jcd_]);

        if (prevFdiff_ * fdiff < 0.0f || prevFdiff_ == 0.0f)
            handleZeroCross();

        prevFdiff_ = fdiff;
        t_++;

        if (++jcorr_ >= (int)c0r_.size()) jcorr_ = 0;
        if (++jcd_ >= (int)diffBuf_.size()) jcd_ = 0;
    }

    void addSamples(const float* buf, int n) {
        for (int i = 0; i < n; i++) addSample(buf[i]);
    }

private:
    float sumCirc(const std::vector<float>& buf, int idx) {
        float total = 0.0f;
        for (size_t i = 0; i < buf.size(); i++) {
            total += buf[idx--];
            if (idx < 0) idx = (int)buf.size() - 1;
        }
        return total;
    }

    void handleZeroCross() {
        int period = t_ - lastTrans_;
        lastTrans_ = t_;
        int bits = (int)std::round(period / spb_);

        if (bits == 0 || bits > 7) {
            state_ = WAITING;
            flagCount_ = 0;
            return;
        }

        if (bits == 7) {
            flagCount_++;
            flagsSeen_++;
            flagSep_ = false;
            data_ = 0;
            bitCount_ = 0;

            switch (state_) {
            case WAITING:
                state_ = JUST_SEEN_FLAG;
                break;
            case DECODING:
                if (checkCRC() && cb_) {
                    packets_++;
                    cb_(pktBytes_);
                }
                resetPacket();
                state_ = JUST_SEEN_FLAG;
                break;
            default:
                break;
            }
            return;
        }

        if (state_ == JUST_SEEN_FLAG)
            state_ = DECODING;

        if (state_ == DECODING) {
            if (bits != 1)
                flagCount_ = 0;
            else if (flagCount_ > 0 && !flagSep_)
                flagSep_ = true;
            else
                flagCount_ = 0;

            // Output bits, handling bit-stuffing
            for (int k = 0; k < bits - 1; k++) {
                bitCount_++;
                data_ >>= 1;
                data_ |= 0x80;
                if (bitCount_ == 8) {
                    addByte((uint8_t)data_);
                    data_ = 0; bitCount_ = 0;
                }
            }
            if (bits - 1 != 5) {
                bitCount_++;
                data_ >>= 1;
                if (bitCount_ == 8) {
                    addByte((uint8_t)data_);
                    data_ = 0; bitCount_ = 0;
                }
            }
        }
    }

    void addByte(uint8_t b) {
        pktBytes_.push_back(b);
        crc_ = crc16_update(crc_, b);
    }

    bool checkCRC() const {
        return pktBytes_.size() >= 2 && crc_ == CRC16_GOOD;
    }

    void resetPacket() {
        pktBytes_.clear();
        crc_ = 0xFFFF;
    }

    enum State { WAITING, JUST_SEEN_FLAG, DECODING };

    int sr_;
    float spb_;
    PacketCb cb_;

    FirFilter bpFilter_, lpFilter_;
    std::vector<float> c0r_, c0i_, c1r_, c1i_;
    std::vector<float> xBuf_, diffBuf_;

    float phi0_{0}, phi1_{0};
    float ph0_{0}, ph1_{0};
    float prevFdiff_{0};

    State state_{WAITING};
    int t_{0}, lastTrans_{0};
    int jcorr_{0}, jcd_{0};
    int data_{0}, bitCount_{0};
    int flagCount_{0}, flagsSeen_{0};
    bool flagSep_{false};
    int packets_{0};

    std::vector<uint8_t> pktBytes_;
    uint16_t crc_{0xFFFF};
};

// ═══════════════════════════════════════════════════════════════════
// IChannelDecoder wrapper
// ═══════════════════════════════════════════════════════════════════

class AprsChannel : public IChannelDecoder {
public:
    explicit AprsChannel(const ChannelContext& ctx)
        : bus_(ctx.bus), channelId_(ctx.channelId),
          freqMHz_(ctx.freqHz / 1e6), rate_(ctx.sampleRate)
    {
        afsk_ = std::make_unique<AfskDemodulator>((int)rate_, 1200.0f, 1200.0f, 2200.0f);
        afsk_->setCallback([this](const std::vector<uint8_t>& data) {
            auto pkt = aprs::parse_ax25_frame(data);
            if (bus_ && !pkt.source.empty()) {
                DecodedRecord r;
                r.timeSec   = nowSec();
                r.channelId = channelId_;
                r.freqMHz   = freqMHz_;
                r.source    = "APRS";
                r.text      = pkt.source + ": " + aprs::format_summary(pkt);

                if (pkt.has_position) {
                    char latbuf[64], lonbuf[64];
                    snprintf(latbuf, sizeof(latbuf), "%.5f", pkt.lat);
                    snprintf(lonbuf, sizeof(lonbuf), "%.5f", pkt.lon);
                    r.fields["lat"] = latbuf;
                    r.fields["lon"] = lonbuf;
                    r.fields["sym"] = std::string(1, pkt.symbol_table) + pkt.symbol_code;
                }
                r.fields["type"] = typeName(pkt.type);
                r.fields["src"]  = pkt.source;
                r.fields["raw"]  = pkt.raw_payload;

                if (!pkt.destination.empty())
                    r.fields["dest"] = pkt.destination;

                bus_->post(r);
            }
        });
    }

    void process(const double* iq, int n) override {
        for (int k = 0; k < n; k++) {
            double I = iq[2 * k], Q = iq[2 * k + 1];
            // FM discriminator (atan2 cross-dot)
            double di = I * prevI_ + Q * prevQ_;
            double dq = Q * prevI_ - I * prevQ_;
            prevI_ = I; prevQ_ = Q;
            double fm = std::atan2(dq, di) / M_PI;

            // Clamp and feed to AFSK demodulator
            if (fm < -1.0) fm = -1.0;
            if (fm >  1.0) fm =  1.0;
            afsk_->addSample((float)fm);
        }
    }

    bool locked() const override { return locked_.load(); }

private:
    static const char* typeName(aprs::PacketType t) {
        switch (t) {
            case aprs::PacketType::Position:  return "Position";
            case aprs::PacketType::MicE:      return "MicE";
            case aprs::PacketType::Object:    return "Object";
            case aprs::PacketType::Item:      return "Item";
            case aprs::PacketType::Message:   return "Message";
            case aprs::PacketType::Weather:   return "Weather";
            case aprs::PacketType::Telemetry: return "Telemetry";
            case aprs::PacketType::Status:    return "Status";
            default: return "Unknown";
        }
    }

    MessageBus* bus_ = nullptr;
    int channelId_ = 0;
    double freqMHz_ = 0.0;
    double rate_ = 48000.0;
    double prevI_ = 0.0, prevQ_ = 0.0;
    std::atomic<bool> locked_{false};

    std::unique_ptr<AfskDemodulator> afsk_;
};

} // namespace

REGISTER_CHANNEL_DECODER(ChannelDecoderInfo{
    kTypeAprs, "APRS (AX.25 AFSK)", "APRS",
    "Digital",
    /*ddcRate*/ 48000.0, /*ddcBandwidth*/ 25000.0, /*weight*/ 4,
    /*isAudio*/ false, /*dedicatedSubband*/ false,
    [](const ChannelContext& c) { return std::make_unique<AprsChannel>(c); }});
