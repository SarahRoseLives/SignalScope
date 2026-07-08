// Unified decoded-output bus. Every channel decoder posts DecodedRecord entries
// here; the generic Messages panel renders them (filterable by 'source'). This
// replaces the per-type log classes (PagerLog, EgcLog, ...) so new decoders need
// no bespoke storage or panel.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// One decoded message/event from any decoder.
struct DecodedRecord
{
    double timeSec = 0.0;                    // wall-clock seconds (epoch)
    int    channelId = 0;                    // owning decoder channel id
    double freqMHz = 0.0;                    // channel frequency
    std::string source;                      // "POCSAG", "FLEX", "ADS-B", ...
    std::string text;                        // human-readable message body
    std::map<std::string, std::string> fields; // optional structured fields
};

class MessageBus
{
public:
    void post(const DecodedRecord& r)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        recs_.push_back(r);
        if (recs_.size() > kMax)
            recs_.erase(recs_.begin(), recs_.begin() + (recs_.size() - kMax));
        ++count_;
    }

    std::vector<DecodedRecord> snapshot()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return recs_;
    }

    uint64_t count()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return count_;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        recs_.clear();
    }

private:
    static constexpr size_t kMax = 5000;
    std::mutex mtx_;
    std::vector<DecodedRecord> recs_;
    uint64_t count_ = 0;
};
