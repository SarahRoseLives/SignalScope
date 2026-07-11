// OOB (out-of-band) cable EPG channel decoder.
// Implements the full SCTE 55-2 pipeline: QPSK demod -> SL-ESF frame sync ->
// FEC -> ATM -> AAL5 -> DSM-CC -> BIOP -> EPG extraction.
//
// Heavy pipeline runs on a background thread so process() never blocks the
// decoder worker, preventing drops. Results accumulate across runs so a
// brief lock loss won't wipe the channel map.
#include "decode/channel/channel_registry.h"
#include "decode/oob/oob_core.h"
#include "util/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

double nowSec()
{
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

class OobChannel : public IChannelDecoder
{
public:
    explicit OobChannel(const ChannelContext& ctx)
        : channelId_(ctx.channelId), freqMHz_(ctx.freqHz / 1e6),
          rate_(ctx.sampleRate)
    {
        iqBuf_.reserve((size_t)(rate_ * 3.0));
        worker_.store(true);
        workerThread_ = std::thread(&OobChannel::workerLoop, this);
    }

    ~OobChannel() override
    {
        worker_.store(false);
        workCv_.notify_one();
        if (workerThread_.joinable())
            workerThread_.join();
    }

    void process(const double* iq, int n) override
    {
        if (n <= 0) return;
        {
            std::lock_guard<std::mutex> lk(bufMtx_);
            iqBuf_.insert(iqBuf_.end(), iq, iq + n * 2);
        }
        workCv_.notify_one();
    }

    bool locked() const override { return locked_.load(); }

private:
    void workerLoop()
    {
        while (worker_.load())
        {
            std::vector<double> chunk;
            {
                std::unique_lock<std::mutex> lk(bufMtx_);
                workCv_.wait_for(lk, std::chrono::milliseconds(500), [this] {
                    return (iqBuf_.size() / 2) >= size_t(rate_ * 2.0) || !worker_.load();
                });
                if (!worker_.load()) break;
                if (iqBuf_.size() / 2 < size_t(rate_ * 2.0)) continue;
                chunk.swap(iqBuf_);
            }

            size_t nComplex = chunk.size() / 2;
            logWrite("OOB: pipeline start — %zu IQ samples (%.1fs @ %.0f Hz)", nComplex,
                     nComplex / rate_, rate_);

            std::vector<std::complex<double>> sig(nComplex);
            for (size_t k = 0; k < nComplex; k++)
                sig[k] = std::complex<double>(chunk[k * 2], chunk[k * 2 + 1]);
            chunk.clear();

            // Stage 1: QPSK demodulation — use sps=4 (matches Python code, was working)
            auto sym = oob::demodulateQpsk(sig, rate_);
            logWrite("OOB: demod -> %zu symbols", sym.size());
            if (sym.size() < 200) { locked_.store(false); continue; }

            // Stage 2: Differential decode + derandomize
            auto bits = oob::symbolsToBits(sym);
            sym.clear();
            logWrite("OOB: diff-decode -> %zu bits", bits.size());
            if (bits.size() < oob::SF_BITS) { locked_.store(false); continue; }

            // Stage 3: Frame sync -> superframe payloads
            auto payloads = oob::trackSuperframes(bits, 6, 4); // drift=6 accept=4 (more sensitive)
            bits.clear();
            logWrite("OOB: frame sync -> %zu superframes", payloads.size());
            if (payloads.empty()) { locked_.store(false); continue; }

            locked_.store(true);

            // Stage 4: FEC -> ATM cells
            int clean = 0, corrected = 0, failed = 0;
            auto cells = oob::decodeCells(payloads, &clean, &corrected, &failed);
            logWrite("OOB: FEC -> %zu ATM cells (clean:%d corr:%d fail:%d)", cells.size(),
                     clean, corrected, failed);

            // Flow histogram & carousel flow detection
            int hecPass = 0, hecFail = 0;
            std::map<std::pair<int,int>, int> flowCounts;
            for (const auto& cell : cells) {
                if (oob::hecOk(cell.data())) {
                    hecPass++;
                    auto h = oob::parseAtmHeader(cell.data());
                    flowCounts[{h.vpi, h.vci}]++;
                } else {
                    hecFail++;
                }
            }

            logWrite("OOB: HEC pass=%d fail=%d (%.1f%% valid)", hecPass, hecFail,
                     100.0 * hecPass / std::max(1, hecPass + hecFail));

            int carouselVpi = -1, carouselVci = -1;
            {
                int best = 0;
                for (auto& [flow, cnt] : flowCounts) {
                    if (flow.first == 0 && flow.second == 0) continue;
                    if (cnt > best) { best = cnt; carouselVpi = flow.first; carouselVci = flow.second; }
                }
            }
            logWrite("OOB: carousel flow = VPI=%02X VCI=%04X", carouselVpi, carouselVci);

            // Build snapshot (EpgStore merges with previous data)
            oob::EpgSnapshot snap;
            snap.gotLock = true;
            snap.lastUpdate = nowSec();
            snap.totalSuperframes = (int)payloads.size();
            snap.totalCells = (int)cells.size();
            snap.cleanCells = clean;
            snap.correctedCells = corrected;
            snap.failedCells = failed;
            snap.hecPass = hecPass;
            snap.hecFail = hecFail;
            snap.carouselVpi = carouselVpi;
            snap.carouselVci = carouselVci;

            if (!cells.empty() && carouselVpi >= 0) {
                auto aal5 = oob::reassembleAal5(cells, carouselVpi, carouselVci);
                snap.aal5Frames = (int)aal5.size();
                logWrite("OOB: AAL5 -> %d frames", snap.aal5Frames);

                if (!aal5.empty()) {
                    auto pkts = oob::ipUdpPayloads(aal5);
                    snap.ipPkts = (int)pkts.size();
                    logWrite("OOB: IP/UDP -> %d packets", snap.ipPkts);

                    auto modules = oob::reassembleModules(pkts);
                    snap.dsmccMods = (int)modules.size();
                    logWrite("OOB: DSM-CC -> %d modules", snap.dsmccMods);
                    for (auto& [key, blob] : modules) {
                        char tagHex[16];
                        std::snprintf(tagHex, sizeof(tagHex), "%02X%02X%02X%02X",
                                      key.first[0], key.first[1], key.first[2], key.first[3]);
                        logWrite("OOB:   mod tag=%s modId=%02X%02X%02X size=%zu",
                                 tagHex, key.second[0], key.second[1], key.second[2], blob.size());
                    }

                    auto [channels, serviceNames] = oob::extractEpg(modules);
                    logWrite("OOB: EPG -> %zu channels, %zu services",
                             channels.size(), serviceNames.size());
                    if (!channels.empty()) {
                        int sample = 0;
                        for (const auto& ch : channels) {
                            if (sample++ < 10)
                                logWrite("OOB:   ch: %s -> %d", ch.callsign.c_str(), ch.channelNumber);
                        }
                    }

                    if (!channels.empty()) {
                        for (const auto& ch : channels)
                            snap.channels.push_back({ch.callsign, ch.channelNumber});
                    }
                    snap.serviceNames = std::move(serviceNames);
                }
            } else {
                logWrite("OOB: skipping carousel — cells=%zu flow_present=%d",
                         cells.size(), carouselVpi >= 0);
            }

            // Extract readable ASCII strings ONLY from HEC-valid carousel-flow cells
            {
                std::vector<std::string> readableStrings;
                for (const auto& cell : cells) {
                    if (!oob::hecOk(cell.data())) continue;
                    auto h = oob::parseAtmHeader(cell.data());
                    if (carouselVpi >= 0 && (h.vpi != carouselVpi || h.vci != carouselVci)) continue;
                    std::string run;
                    for (int b = oob::ATM_HEADER_LEN; b < oob::ATM_CELL_LEN; b++) {
                        unsigned char c = cell[b];
                        if (c >= 0x20 && c < 0x7F) {
                            run += (char)c;
                        } else {
                            if (run.size() >= 5) {
                                int alpha = 0;
                                for (char ch : run) if ((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')) alpha++;
                                if (alpha >= 4 && double(alpha)/run.size() > 0.5)
                                    readableStrings.push_back(run);
                            }
                            run.clear();
                        }
                    }
                    if (run.size() >= 5) {
                        int alpha = 0;
                        for (char ch : run) if ((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')) alpha++;
                        if (alpha >= 4 && double(alpha)/run.size() > 0.5)
                            readableStrings.push_back(run);
                    }
                }
                snap.readableStrings = std::move(readableStrings);
            }

            logWrite("OOB: updating store — %zu ch, %zu svc, %zu strings",
                     snap.channels.size(), snap.serviceNames.size(), snap.readableStrings.size());
            oob::EpgStore::instance().update(std::move(snap));
        }
    }

    int channelId_ = 0;
    double freqMHz_ = 0.0;
    double rate_ = 48000.0;

    // Background worker
    std::atomic<bool> worker_{false};
    std::thread workerThread_;
    std::mutex bufMtx_;
    std::condition_variable workCv_;
    std::vector<double> iqBuf_;

    std::atomic<bool> locked_{false};
};

} // namespace

REGISTER_CHANNEL_DECODER(ChannelDecoderInfo{
    5, // kTypeOobEpg
    "OOB EPG (cable TV)", "OOB-EPG",
    /*ddcRate*/ 2000000.0, /*ddcBandwidth*/ 1000000.0, /*weight*/ 8,
    /*isAudio*/ false, /*dedicatedSubband*/ true,
    [](const ChannelContext& c) { return std::make_unique<OobChannel>(c); }});
