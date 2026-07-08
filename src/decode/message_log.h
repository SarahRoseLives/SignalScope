// Thread-safe log of decoded pager messages.
//
// NOTE: this is legacy and slated to be replaced by MessageBus (decode/channel/
// message_bus.h). The Inmarsat-C/EGC/ACARS message types that used to live here
// were removed with the EGC decoder.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// POCSAG / FLEX pager message.
struct PagerMessage
{
    double timeSec = 0.0;
    int channelId = 0;
    double freqMHz = 0.0;
    int protocol = 0;      // 0=POCSAG, 1=FLEX
    int baud = 0;
    uint32_t address = 0;  // POCSAG address / FLEX capcode
    int function = 0;      // POCSAG function bits (0-3) / FLEX message type
    std::string text;      // alpha/decoded message
    std::string numeric;   // numeric rendering
    std::string protocolName; // "POCSAG", "FLEX"
    int errors = 0;
    bool inverted = false;
};

class PagerLog
{
public:
    void add(const PagerMessage& m)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        msgs_.push_back(m);
        if (msgs_.size() > kMax)
            msgs_.erase(msgs_.begin(), msgs_.begin() + (msgs_.size() - kMax));
        ++count_;
    }

    std::vector<PagerMessage> snapshot()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return msgs_;
    }

    uint64_t count()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return count_;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        msgs_.clear();
    }

private:
    static constexpr size_t kMax = 2000;
    std::mutex mtx_;
    std::vector<PagerMessage> msgs_;
    uint64_t count_ = 0;
};
