// HD Radio (NRSC-5) channel decoder using the libnrsc5 library.
// Feeds IQ via pipe API, handles callbacks for audio, station info,
// ID3 metadata, artwork, and signal quality.

#ifdef HAS_NRSC5

#include "decode/channel/channel_registry.h"
#include "decode/channel/message_bus.h"
#include "decode/oob/oob_core.h" // for Nrsc5Store
#include "audio/audio_sink.h"
#include "util/log.h"

extern "C" {
#include <nrsc5.h>
}

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

double nowSec()
{
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

class Nrsc5Channel : public IChannelDecoder
{
public:
    explicit Nrsc5Channel(const ChannelContext& ctx)
        : bus_(ctx.bus), audio_(ctx.audio),
          channelId_(ctx.channelId), freqMHz_(ctx.freqHz / 1e6),
          rate_(ctx.sampleRate)
    {
        int rc = nrsc5_open_pipe(&nrsc5_);
        if (rc != 0) {
            logWrite("NRSC5: open_pipe failed (rc=%d)", rc);
            nrsc5_ = nullptr;
            return;
        }
        nrsc5_set_mode(nrsc5_, NRSC5_MODE_FM);
        nrsc5_set_callback(nrsc5_, &Nrsc5Channel::callback, this);
        nrsc5_start(nrsc5_);
        active_.store(true);
        // nrsc5 cu8 expects 1488375 Hz. Compute exact rational resample ratio.
        const double targetRate = 1488375.0;
        if (rate_ > 0) {
            resampleNum_ = (uint64_t)targetRate;
            resampleDen_ = (uint64_t)rate_;
            // Simplify
            uint64_t a = resampleNum_, b = resampleDen_;
            while (b) { uint64_t t = b; b = a % b; a = t; }
            resampleNum_ /= a; resampleDen_ /= a;
        }
        resamplePhase_ = 0;
        logWrite("NRSC5: opened pipe, DDC rate=%.0f resample %llu/%llu",
                 rate_, (unsigned long long)resampleNum_, (unsigned long long)resampleDen_);
    }

    ~Nrsc5Channel() override
    {
        if (nrsc5_) {
            nrsc5_stop(nrsc5_);
            nrsc5_close(nrsc5_);
        }
    }

    void process(const double* iq, int n) override
    {
        if (!nrsc5_ || !active_.load() || n <= 0) return;

        // Rational resampler: DDC rate -> nrsc5 cu8 rate (1488375 Hz)
        for (int i = 0; i < n; i++) {
            resamplePhase_ += resampleNum_;
            while (resamplePhase_ >= resampleDen_) {
                resamplePhase_ -= resampleDen_;
                double I = iq[i * 2];
                double Q = iq[i * 2 + 1];
                uint8_t u8I = (uint8_t)std::max(0.0, std::min(255.0, (I + 1.0) * 127.5));
                uint8_t u8Q = (uint8_t)std::max(0.0, std::min(255.0, (Q + 1.0) * 127.5));
                resampleBuf_.push_back(u8I);
                resampleBuf_.push_back(u8Q);
            }
        }

        if (resampleBuf_.size() >= 4096) {
            nrsc5_pipe_samples_cu8(nrsc5_, resampleBuf_.data(), (unsigned int)resampleBuf_.size());
            resampleBuf_.clear();
        }
    }

    bool locked() const override { return sync_.load(); }
    bool isAudio() const override { return true; }
    double audioRate() const override { return 44100.0; }
    void setAudioActive(bool on) override
    {
        audioActive_.store(on);
        if (audio_) audio_->flush();
    }
    bool audioActive() const override { return audioActive_.load(); }

private:
    static void callback(const nrsc5_event_t* evt, void* opaque)
    {
        static_cast<Nrsc5Channel*>(opaque)->onEvent(evt);
    }

    void onEvent(const nrsc5_event_t* evt)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        bool changed = false;
        switch (evt->event) {
        case NRSC5_EVENT_SYNC:
            sync_.store(true); changed = true;
            break;
        case NRSC5_EVENT_LOST_SYNC:
            sync_.store(false); changed = true;
            break;
        case NRSC5_EVENT_MER:
            merLower_.store(evt->mer.lower);
            merUpper_.store(evt->mer.upper);
            changed = true;
            break;
        case NRSC5_EVENT_BER:
            ber_.store(evt->ber.cber);
            changed = true;
            break;
        case NRSC5_EVENT_STATION_NAME:
            if (evt->station_name.name) { stationName_ = evt->station_name.name; changed = true; }
            break;
        case NRSC5_EVENT_STATION_SLOGAN:
            if (evt->station_slogan.slogan) { stationSlogan_ = evt->station_slogan.slogan; changed = true; }
            break;
        case NRSC5_EVENT_STATION_MESSAGE:
            if (evt->station_message.message) { stationMessage_ = evt->station_message.message; changed = true; }
            break;
        case NRSC5_EVENT_STATION_LOCATION: {
            char buf[128];
            snprintf(buf, sizeof(buf), "%.4f, %.4f (%dm)",
                     evt->station_location.latitude, evt->station_location.longitude,
                     evt->station_location.altitude);
            stationLocation_ = buf; changed = true;
            break;
        }
        case NRSC5_EVENT_STATION_ID: {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s  Facility #%d",
                     evt->station_id.country_code ? evt->station_id.country_code : "",
                     evt->station_id.fcc_facility_id);
            stationId_ = buf; changed = true;
            break;
        }
        case NRSC5_EVENT_LOCAL_TIME: {
            char buf[128];
            snprintf(buf, sizeof(buf), "UTC%+d  DST=%s%s",
                     evt->local_time.utc_offset, evt->local_time.dst_local ? "Yes" : "No",
                     evt->local_time.dst_schedule == 1 ? " (US)" :
                     evt->local_time.dst_schedule == 2 ? " (EU)" : "");
            localTime_ = buf; changed = true;
            break;
        }
        case NRSC5_EVENT_AUDIO_SERVICE: {
            char buf[128];
            snprintf(buf, sizeof(buf), "codec=%d blend=%d gain=%ddB delay=%d",
                     evt->audio_service.codec_mode, evt->audio_service.blend_control,
                     evt->audio_service.digital_audio_gain, evt->audio_service.common_delay);
            codecInfo_ = buf; changed = true;
            break;
        }
        case NRSC5_EVENT_AUDIO_SERVICE_DESCRIPTOR: {
            const char* ptName = nullptr;
            nrsc5_program_type_name(evt->asd.type, &ptName);
            if (ptName) { programType_ = ptName; changed = true; }
            break;
        }
        case NRSC5_EVENT_EMERGENCY_ALERT:
            if (evt->emergency_alert.message && bus_) {
                DecodedRecord r;
                r.timeSec = nowSec();
                r.channelId = channelId_;
                r.freqMHz = freqMHz_;
                r.source = "HD-Alert";
                r.text = evt->emergency_alert.message;
                bus_->post(r);
            }
            break;
        case NRSC5_EVENT_ID3:
            if (evt->id3.title)  { trackTitle_  = evt->id3.title;  changed = true; }
            if (evt->id3.artist) { trackArtist_ = evt->id3.artist; changed = true; }
            if (evt->id3.album)  { trackAlbum_  = evt->id3.album;  changed = true; }
            if (evt->id3.genre)  { trackGenre_  = evt->id3.genre;  changed = true; }
            if (bus_ && !trackTitle_.empty()) {
                DecodedRecord r;
                r.timeSec = nowSec();
                r.channelId = channelId_;
                r.freqMHz = freqMHz_;
                r.source = "HD-Radio";
                r.text = trackTitle_;
                if (!trackArtist_.empty()) r.text += " - " + trackArtist_;
                bus_->post(r);
            }
            break;
        case NRSC5_EVENT_SIG:
            programs_.clear();
            for (auto* svc = evt->sig.services; svc; svc = svc->next) {
                std::string name = svc->name ? svc->name : "?";
                if (svc->type == NRSC5_SIG_SERVICE_AUDIO && svc->audio_component) {
                    const char* ptName = nullptr;
                    nrsc5_program_type_name(svc->audio_component->audio.type, &ptName);
                    if (ptName) name += std::string(" (") + ptName + ")";
                }
                programs_.push_back(name);
            }
            changed = true;
            break;
        case NRSC5_EVENT_AUDIO:
            if (audio_ && audioActive_.load() && evt->audio.data && evt->audio.count > 0
                && (int)evt->audio.program == oob::Nrsc5Store::instance().snapshot().activeProgram) {
                if (!audioLogged_) {
                    logWrite("NRSC5: audio event — program=%d samples=%zu",
                             evt->audio.program, evt->audio.count);
                    audioLogged_ = true;
                }
                // nrsc5 outputs interleaved stereo int16. Use left channel only.
                size_t stereoCount = evt->audio.count;
                std::vector<float> af(stereoCount / 2);
                for (size_t i = 0; i < stereoCount / 2; i++)
                    af[i] = evt->audio.data[i * 2] / 32768.0f;
                audio_->push(af.data(), (int)af.size());
            }
            break;
        case NRSC5_EVENT_LOT:
            if (evt->lot.data && evt->lot.size > 0 && evt->lot.size < 1048576) {
                artwork_.assign(evt->lot.data, evt->lot.data + evt->lot.size);
            }
            break;
        }
        if (changed) pushToStore();
    }

    void pushToStore()
    {
        oob::Nrsc5Info info;
        info.sync = sync_.load();
        info.merLower = merLower_.load();
        info.merUpper = merUpper_.load();
        info.stationName = stationName_;
        info.stationSlogan = stationSlogan_;
        info.stationMessage = stationMessage_;
        info.stationLocation = stationLocation_;
        info.stationId = stationId_;
        info.trackTitle = trackTitle_;
        info.trackArtist = trackArtist_;
        info.trackAlbum = trackAlbum_;
        info.trackGenre = trackGenre_;
        info.programType = programType_;
        info.codecInfo = codecInfo_;
        info.localTime = localTime_;
        info.activeProgram = oob::Nrsc5Store::instance().snapshot().activeProgram;
        info.programs = programs_;
        oob::Nrsc5Store::instance().update(info);
    }

    nrsc5_t* nrsc5_ = nullptr;
    MessageBus* bus_ = nullptr;
    AudioSink* audio_ = nullptr;
    int channelId_ = 0;
    double freqMHz_ = 0.0;
    double rate_ = 0.0;

    mutable std::mutex mtx_;
    std::atomic<bool> sync_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> audioActive_{true}; // start on for auto-play
    std::atomic<float> merLower_{0}, merUpper_{0}, ber_{0};

    std::string stationName_, stationSlogan_, stationMessage_;
    std::string stationLocation_, stationId_;
    std::string trackTitle_, trackArtist_, trackAlbum_, trackGenre_;
    std::string programType_, codecInfo_, localTime_;
    std::vector<std::string> programs_;
    int activeProgIdx_ = 0;
    std::vector<uint8_t> artwork_;
    std::vector<float> scratch_;
    std::vector<uint8_t> resampleBuf_;
    uint64_t resampleNum_ = 1, resampleDen_ = 1, resamplePhase_ = 0;
    bool audioLogged_ = false;
};

} // namespace

REGISTER_CHANNEL_DECODER(ChannelDecoderInfo{
    kTypeNrsc5,
    "HD Radio (NRSC-5)", "HD",
    /*ddcRate*/ 1488375.0, /*ddcBandwidth*/ 500000.0, /*weight*/ 10,
    /*isAudio*/ true, /*dedicatedSubband*/ true,
    [](const ChannelContext& c) { return std::make_unique<Nrsc5Channel>(c); }});

#else
// Stub when libnrsc5 not found
REGISTER_CHANNEL_DECODER(ChannelDecoderInfo{
    kTypeNrsc5,
    "HD Radio (NRSC-5)", "HD",
    /*ddcRate*/ 48000.0, /*ddcBandwidth*/ 12500.0, /*weight*/ 0,
    /*isAudio*/ false, /*dedicatedSubband*/ false,
    [](const ChannelContext&) { return nullptr; }});
#endif
