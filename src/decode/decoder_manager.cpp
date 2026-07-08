#include "decode/decoder_manager.h"

#include "decode/channel/channel_registry.h"
#include "audio/audio_sink.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

// Shared front-end IF rate and passband. ~250 kHz IF -> ±150 kHz usable
// coverage per sub-band; decoders within ±135 kHz of a sub-band centre share it.
static constexpr double kSubRateTarget = 250000.0;
static constexpr double kSubBW = 200000.0;

// Per-type properties come from the channel registry.
static int typeWeight(int typeId)
{
    const ChannelDecoderInfo* info = ChannelRegistry::instance().byType(typeId);
    return info ? info->weight : 3;
}
static bool typeDedicatedSubband(int typeId)
{
    const ChannelDecoderInfo* info = ChannelRegistry::instance().byType(typeId);
    return info && info->dedicatedSubband;
}
static bool typeIsAudio(int typeId)
{
    const ChannelDecoderInfo* info = ChannelRegistry::instance().byType(typeId);
    return info && info->isAudio;
}

void DecoderManager::configure(double Fs, double centerHz)
{
    Fs_ = Fs;
    centerHz_ = centerHz;
}

void DecoderManager::start()
{
    if (run_.load())
        return;

    unsigned hw = std::thread::hardware_concurrency();
    int nWorkers = (hw > 2) ? (int)hw : 1;
    if (nWorkers > maxWorkers_)
        nWorkers = maxWorkers_;
    if (nWorkers < 1)
        nWorkers = 1;

    run_.store(true);
    workers_.clear();
    for (int i = 0; i < nWorkers; ++i)
        workers_.push_back(std::make_unique<Worker>());
    for (auto& w : workers_)
        w->thread = std::thread([this, wp = w.get()]() { workerLoop(wp); });
}

void DecoderManager::stop()
{
    run_.store(false);
    for (auto& w : workers_)
        w->cv.notify_all();
    for (auto& w : workers_)
        if (w->thread.joinable())
            w->thread.join();
    workers_.clear();
}

void DecoderManager::feed(const float* iq, int nComplex)
{
    if (!run_.load())
        return;
    // One shared block — all workers reference the same memory, zero per-worker copies.
    auto shared = std::make_shared<const std::vector<float>>(iq, iq + (size_t)nComplex * 2);
    for (auto& w : workers_)
    {
        if (w->count.load() == 0)
            continue;
        {
            std::lock_guard<std::mutex> lk(w->qMtx);
            if (w->queue.size() >= kMaxQueue)
            {
                w->queue.pop_front();
                drops_.fetch_add(1);
            }
            w->queue.push_back(shared);
        }
        w->cv.notify_one();
    }
}

int DecoderManager::addDecoder(double freqHz, int typeId)
{
    int id = addDecoderImpl(freqHz, typeId);
    // Auto-play: dropping an audio channel (WFM/AM/NFM) immediately routes it to
    // the speaker (no separate "Listen" click needed). Done after the add so all
    // locks are released. setAudioChannel would toggle off if id already active,
    // so only call it when this id isn't already the audio channel.
    if (id > 0 && typeIsAudio(typeId) && audioChannel_.load() != id)
        setAudioChannel(id);
    return id;
}

int DecoderManager::addDecoderImpl(double freqHz, int typeId)
{
    if (Fs_ <= 0.0 || workers_.empty())
        return -1;

    int id;
    {
        std::lock_guard<std::mutex> lk(idMtx_);
        id = nextId_++;
    }

    const bool dedicated = typeDedicatedSubband(typeId);

    // 1) Find the best existing sub-band that covers this frequency.
    //    If a sub-band is overloaded (>4 decoders), prefer a shadow
    //    sub-band on a lighter worker to spread the load.
    //    Wideband (dedicated-subband) channels steer their own front-end, so they
    //    always get a dedicated sub-band and never share one (in either direction).
    Worker* bestW = nullptr;
    std::shared_ptr<SubBand> bestSb;
    int bestLoad = 0x7FFFFFFF; // total decoders on that worker
    if (!dedicated)
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
        {
            if (!sb->decoders.empty() && typeDedicatedSubband(sb->decoders.front()->typeId()))
                continue; // never join a dedicated (wideband) sub-band
            if (std::fabs(freqHz - sb->centerHz) < 0.40 * sb->subRate)
            {
                int cnt = (int)sb->decoders.size();
                if (cnt >= 4)
                {
                    // Overloaded sub-band — prefer a shadow on a lighter worker.
                    // The effective load is the worker's total (decoder weight + sub-band weight).
                    int eff = w->weight.load() + (int)w->subbands.size() * 10 + cnt * 8;
                    if (eff < bestLoad)
                    {
                        bestW = w.get();
                        bestSb = sb;
                        bestLoad = eff;
                    }
                }
                else
                {
                    // Not overloaded — prefer the lowest raw decoder count.
                    if (cnt < bestLoad)
                    {
                        bestW = w.get();
                        bestSb = sb;
                        bestLoad = cnt;
                    }
                }
            }
        }
    }

    if (bestW)
    {
        std::lock_guard<std::mutex> lk(bestW->dMtx);
        // Re-lookup the sub-band (it might have been removed between the two locks).
        for (auto& sb : bestW->subbands)
        {
            if (sb != bestSb) continue;
            auto dec = std::make_shared<Decoder>(
                sb->subRate, sb->centerHz, freqHz, typeId, id, &bus_, audioSink_);
            sb->decoders.emplace_back(std::move(dec));
            bestW->count.fetch_add(1);
            bestW->weight.fetch_add(typeWeight(typeId));
            return id;
        }
        // Sub-band was removed between locks — fall through to creation.
    }

    // 2) No covering or non-overloaded sub-band -> create a new one.
    // Effective weight includes sub-band count (front-end DDC ~= 5 MSK decoders).
    Worker* best = workers_[0].get();
    int bestEff = best->weight.load() + (int)best->subbands.size() * 5;
    for (auto& w : workers_)
    {
        int eff = w->weight.load() + (int)w->subbands.size() * 5;
        if (eff < bestEff) { best = w.get(); bestEff = eff; }
    }

    std::lock_guard<std::mutex> lk(best->dMtx);
    auto sb = std::make_shared<SubBand>(Fs_, centerHz_, freqHz, kSubRateTarget, kSubBW);
    auto dec = std::make_shared<Decoder>(
        sb->subRate, sb->centerHz, freqHz, typeId, id, &bus_, audioSink_);
    sb->decoders.emplace_back(std::move(dec));
    best->subbands.push_back(std::move(sb));
    best->count.fetch_add(1);
    best->weight.fetch_add(typeWeight(typeId));
    return id;
}

void DecoderManager::removeDecoder(int channelId)
{
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto sbIt = w->subbands.begin(); sbIt != w->subbands.end(); ++sbIt)
        {
            auto& decs = (*sbIt)->decoders;
            for (auto it = decs.begin(); it != decs.end(); ++it)
                if ((*it)->channelId() == channelId)
                {
                    int typeOfRemoved = (*it)->typeId();
                    if (audioChannel_.load() == channelId)
                    {
                        (*it)->setAudioActive(false);
                        audioChannel_.store(0);
                        if (audioSink_) audioSink_->flush();
                    }
                    decs.erase(it);
                    w->count.fetch_sub(1);
                    w->weight.fetch_sub(typeWeight(typeOfRemoved));
                    if (decs.empty())
                        w->subbands.erase(sbIt); // drop now-empty sub-band
                    return;
                }
        }
    }
}

void DecoderManager::setDecoderFreq(int channelId, double freqHz)
{
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
            for (auto& d : sb->decoders)
                if (d->channelId() == channelId)
                {
                    if (typeDedicatedSubband(d->typeId()))
                    {
                        // Wideband: move the whole front-end onto the signal so
                        // it gets real selectivity (no aliasing from a fixed IF).
                        d->setFreq(freqHz);
                        sb->recenter(freqHz);
                    }
                    else
                    {
                        d->setFreq(freqHz); // stays within the sub-band's IF window
                    }
                    return;
                }
    }
}

void DecoderManager::removeAll()
{
    audioChannel_.store(0);
    if (audioSink_)
        audioSink_->flush();
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        w->subbands.clear();
        w->count.store(0);
        std::lock_guard<std::mutex> ql(w->qMtx);
        w->queue.clear();
    }
}

// Route audio from exactly one audio channel (WFM/AM/NFM), or none if 0/not
// found. Toggling the same channel again turns audio off.
void DecoderManager::setAudioChannel(int channelId)
{
    int prev = audioChannel_.load();
    int target = (channelId == prev) ? 0 : channelId; // toggle off if re-selected
    double activeRate = 0.0;
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
            for (auto& d : sb->decoders)
            {
                if (!d->isAudio())
                    continue;
                bool on = (d->channelId() == target);
                d->setAudioActive(on);
                if (on)
                    activeRate = d->audioRate();
            }
    }
    audioChannel_.store(target);
    if (audioSink_)
    {
        if (target != 0 && activeRate > 0.0)
            audioSink_->setSourceRate(activeRate);
        audioSink_->flush();
    }
}

int DecoderManager::decoderCount()
{
    int n = 0;
    for (auto& w : workers_)
        n += w->count.load();
    return n;
}

int DecoderManager::subbandCount()
{
    int n = 0;
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        n += (int)w->subbands.size();
    }
    return n;
}

std::vector<DecoderManager::Status> DecoderManager::status()
{
    std::vector<Status> out;
    for (auto& w : workers_)
    {
        std::lock_guard<std::mutex> lk(w->dMtx);
        for (auto& sb : w->subbands)
            for (auto& d : sb->decoders)
            {
                Status s{d->channelId(), d->freqMHz(), d->typeId(), d->locked()};
                s.isAudio = d->isAudio();
                s.audioOn = d->audioActive();
                out.push_back(s);
            }
    }
    std::sort(out.begin(), out.end(),
              [](const Status& a, const Status& b) { return a.freqMHz < b.freqMHz; });
    return out;
}

void DecoderManager::workerLoop(Worker* w)
{
    while (run_.load())
    {
        std::shared_ptr<const std::vector<float>> blockPtr;
        {
            std::unique_lock<std::mutex> lk(w->qMtx);
            w->cv.wait(lk, [&]() { return !run_.load() || !w->queue.empty(); });
            if (!run_.load())
                break;
            blockPtr = w->queue.front();
            w->queue.pop_front();
        }
        const std::vector<float>& block = *blockPtr;
        int nWide = (int)(block.size() / 2);

        // Snapshot sub-band + decoder shared_ptrs under dMtx, then release
        // before processing so the UI thread never blocks on status().
        struct Snap { std::shared_ptr<SubBand> sb; std::vector<std::shared_ptr<Decoder>> decs; };
        std::vector<Snap> snap;
        {
            std::lock_guard<std::mutex> lk(w->dMtx);
            snap.reserve(w->subbands.size());
            for (auto& sb : w->subbands)
            {
                Snap s;
                s.sb = sb;
                s.decs = sb->decoders; // shared_ptr copies keep decoders alive
                snap.push_back(std::move(s));
            }
        }
        // Process outside dMtx — UI thread can now read status instantly.
        for (auto& s : snap)
        {
            s.sb->subIQ.clear();
            s.sb->frontEnd.process(block.data(), nWide, s.sb->subIQ);
            int nSub = (int)(s.sb->subIQ.size() / 2);
            if (nSub <= 0) continue;
            for (auto& d : s.decs)
                d->process(s.sb->subIQ.data(), nSub);
        }
    }
}
