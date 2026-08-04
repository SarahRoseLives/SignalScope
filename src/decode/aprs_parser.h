// APRS payload parser — ported from WaveGate.
// Parses APRS information fields (position, weather, telemetry, messages, etc.)
// and AX.25 frame headers. No dependencies beyond the C++ standard library.
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace aprs {

enum class PacketType {
    Unknown,
    Position,
    MicE,
    Object,
    Item,
    Message,
    Weather,
    Telemetry,
    Status,
    Query,
    Capabilities,
    ThirdParty,
    UserDefined,
};

struct AprsPacket {
    PacketType  type = PacketType::Unknown;

    std::string source;
    std::string destination;
    std::vector<std::string> digipeaters;

    double      lat = 0.0;
    double      lon = 0.0;
    bool        has_position = false;
    char        symbol_table = '/';
    char        symbol_code  = '/';

    std::string comment;
    std::string raw_payload;

    std::string timestamp;

    int         course   = -1;
    int         speed    = -1;
    int         altitude = -1;

    std::string message_text;

    std::string msg_addressee;
    std::string msg_body;
    std::string msg_id;

    std::string object_name;
    bool        object_alive = true;

    int         wx_wind_dir   = -1;
    int         wx_wind_speed = -1;
    int         wx_wind_gust  = -1;
    int         wx_temp_f     = 0;
    int         wx_rain_1h    = -1;
    int         wx_rain_24h   = -1;
    int         wx_rain_midnt = -1;
    int         wx_humidity   = -1;
    float       wx_baro_mb    = -1.0f;
    bool        has_weather   = false;

    int         telem_seq     = -1;
    int         telem_a[5]    = {-1,-1,-1,-1,-1};
    uint8_t     telem_digital = 0;

    std::vector<uint8_t> info_bytes;
};

// Parse binary AX.25 frame (without flag bytes or FCS)
AprsPacket parse_ax25_frame(const std::vector<uint8_t>& data);

// Parse APRS information field (payload only, no AX.25 header)
AprsPacket parse_payload(const std::string& source, const std::string& dest,
                          const std::string& payload);

// Format a packet as a one-line human-readable summary
std::string format_summary(const AprsPacket& pkt);

} // namespace aprs
