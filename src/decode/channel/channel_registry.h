// Registry of channel decoder types. Each decoder .cpp registers a
// ChannelDecoderInfo at static-init time via REGISTER_CHANNEL_DECODER; the GUI
// builds its "Decode type" list from ChannelRegistry::all() and the manager
// reads DDC/weight/audio settings from the selected info. Adding a decoder is
// one new .cpp + one REGISTER block — nothing else changes.
//
// Registration relies on static initializers, which are preserved because the
// decoder .cpp files are compiled directly into the executable target (not a
// static library that could drop unreferenced objects).
#pragma once

#include "decode/channel/ichannel_decoder.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Stable type ids, persisted in config. Never renumber existing values.
enum ChannelType
{
    kTypeWfm   = 1,
    kTypeAm    = 2,
    kTypeNfm   = 3,
    kTypePager = 4,
    kTypeOobEpg = 5,
    kTypeNrsc5 = 6,
};

struct ChannelDecoderInfo
{
    int         typeId = 0;          // stable id, persisted in config
    std::string name;                // combo label, e.g. "WFM (broadcast audio)"
    std::string shortLabel;          // table label, e.g. "WFM"
    std::string category;            // group label, e.g. "Audio", "Digital"
    double      ddcRate = 48000.0;   // per-channel DDC output rate (Hz)
    double      ddcBandwidth = 12500.0; // DDC passband (double-sided, Hz)
    int         weight = 3;          // CPU load weight for worker balancing
    bool        isAudio = false;     // routes audio to the shared AudioSink
    bool        dedicatedSubband = false; // wideband: own sub-band, re-centred on retune
    std::function<std::unique_ptr<IChannelDecoder>(const ChannelContext&)> make;
};

class ChannelRegistry
{
public:
    static ChannelRegistry& instance()
    {
        static ChannelRegistry r;
        return r;
    }

    void add(const ChannelDecoderInfo& info) { infos_.push_back(info); }

    const std::vector<ChannelDecoderInfo>& all() const { return infos_; }

    const ChannelDecoderInfo* byType(int typeId) const
    {
        for (const auto& i : infos_)
            if (i.typeId == typeId)
                return &i;
        return nullptr;
    }

    // Index in all() for a typeId, or -1.
    int indexOf(int typeId) const
    {
        for (int i = 0; i < (int)infos_.size(); ++i)
            if (infos_[i].typeId == typeId)
                return i;
        return -1;
    }

private:
    std::vector<ChannelDecoderInfo> infos_;
};

// Helper for static registration.
struct ChannelDecoderRegistrar
{
    explicit ChannelDecoderRegistrar(const ChannelDecoderInfo& info)
    {
        ChannelRegistry::instance().add(info);
    }
};

#define REGISTER_CHANNEL_DECODER_CAT2(a, b) a##b
#define REGISTER_CHANNEL_DECODER_CAT(a, b) REGISTER_CHANNEL_DECODER_CAT2(a, b)
// Variadic so the braced ChannelDecoderInfo{...} initializer (which contains
// commas) can be passed directly.
#define REGISTER_CHANNEL_DECODER(...) \
    static ChannelDecoderRegistrar REGISTER_CHANNEL_DECODER_CAT(_channel_registrar_, __COUNTER__)(__VA_ARGS__)
