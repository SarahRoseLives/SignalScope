// APRS payload parser — ported from WaveGate (src/aprs/parser.cpp + base91.h).
// Stripped of spdlog, namespaces, and Windows-specific code. Pure C++17.
#include "decode/aprs_parser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace aprs {

// ═══════════════════════════════════════════════════════════════════
// Base-91 decoder (from WaveGate aprs/base91.h)
// ═══════════════════════════════════════════════════════════════════

static inline int b91val(char c) { return (int)c - 33; }

static int32_t b91d4(const std::string& s) {
    if (s.size() < 4) return 0;
    return b91val(s[0]) * 91 * 91 * 91
         + b91val(s[1]) * 91 * 91
         + b91val(s[2]) * 91
         + b91val(s[3]);
}

static int32_t b91d2(const std::string& s) {
    if (s.size() < 2) return 0;
    return b91val(s[0]) * 91 + b91val(s[1]);
}

static double b91lat(const std::string& s) {
    return 90.0 - (double)b91d4(s) / 380926.0;
}

static double b91lon(const std::string& s) {
    return (double)b91d4(s) / 190463.0 - 180.0;
}

static void b91cs(const std::string& cs, int& course, int& speed) {
    if (cs.size() < 2) { course = -1; speed = -1; return; }
    int c = b91val(cs[0]), s = b91val(cs[1]);
    if (c == 0) { course = -1; speed = -1; return; }
    if (c == 90) { course = -1; speed = -1; return; }
    course = c * 4;
    speed = (int)(std::pow(1.08, s) - 1.0 + 0.5);
}

static int b91alt(const std::string& cs) {
    if (cs.size() < 2) return -1;
    return (int)(std::pow(1.002, b91d2(cs)) + 0.5);
}

// ═══════════════════════════════════════════════════════════════════
// Forward declarations
// ═══════════════════════════════════════════════════════════════════

static AprsPacket parse_standard_position(AprsPacket pkt, const std::string& info,
                                           bool has_timestamp, bool has_messaging);
static AprsPacket parse_mice(AprsPacket pkt, const std::string& info,
                              const std::string& dest);
static AprsPacket parse_object(AprsPacket pkt, const std::string& info);
static AprsPacket parse_item(AprsPacket pkt, const std::string& info);
static AprsPacket parse_message_payload(AprsPacket pkt, const std::string& info);
static AprsPacket parse_weather(AprsPacket pkt, const std::string& info);
static AprsPacket parse_telemetry(AprsPacket pkt, const std::string& info);
static AprsPacket parse_status(AprsPacket pkt, const std::string& info);
static void parse_weather_fields(AprsPacket& pkt, const std::string& data);

// ═══════════════════════════════════════════════════════════════════
// AX.25 Frame Parser
// ═══════════════════════════════════════════════════════════════════

static std::string extract_call(const uint8_t* addr, int& ssid, bool& h_bit) {
    char call[7]{};
    for (int i = 0; i < 6; i++)
        call[i] = (char)((addr[i] >> 1) & 0x7F);
    std::string result(call);
    while (!result.empty() && result.back() == ' ')
        result.pop_back();
    ssid = (addr[6] >> 1) & 0x0F;
    h_bit = (addr[6] & 0x80) != 0;
    if (ssid > 0) result += "-" + std::to_string(ssid);
    return result;
}

AprsPacket parse_ax25_frame(const std::vector<uint8_t>& data) {
    AprsPacket pkt;
    if (data.size() < 16) return pkt;

    int dest_ssid; bool dest_h;
    pkt.destination = extract_call(data.data(), dest_ssid, dest_h);
    bool has_more = (data[6] & 0x01) == 0;

    int src_ssid; bool src_h;
    pkt.source = extract_call(data.data() + 7, src_ssid, src_h);
    has_more = (data[13] & 0x01) == 0;

    size_t pos = 14;
    while (has_more && pos + 7 <= data.size()) {
        int digi_ssid; bool digi_h;
        std::string digi = extract_call(data.data() + pos, digi_ssid, digi_h);
        if (digi_h) digi += "*";
        pkt.digipeaters.push_back(digi);
        has_more = (data[pos + 6] & 0x01) == 0;
        pos += 7;
        if (pkt.digipeaters.size() >= 8) break;
    }

    if (pkt.digipeaters.empty() && dest_ssid >= 1 && dest_ssid <= 7) {
        char buf[16];
        snprintf(buf, sizeof(buf), "WIDE%d-%d", dest_ssid, dest_ssid);
        pkt.digipeaters.push_back(buf);
    }

    if (pos >= data.size()) return pkt;
    pos++; // control
    if (pos >= data.size()) return pkt;
    pos++; // PID

    pkt.info_bytes.assign(data.begin() + pos, data.end());
    std::string info(pkt.info_bytes.begin(), pkt.info_bytes.end());

    if (!info.empty()) {
        pkt.raw_payload = info;
        pkt = parse_payload(pkt.source, pkt.destination, info);
    }
    return pkt;
}

// ═══════════════════════════════════════════════════════════════════
// Data Type Dispatch
// ═══════════════════════════════════════════════════════════════════

AprsPacket parse_payload(const std::string& source, const std::string& dest,
                          const std::string& info) {
    AprsPacket pkt;
    pkt.source = source;
    pkt.destination = dest;
    pkt.raw_payload = info;
    if (info.empty()) return pkt;

    char dti = info[0];
    switch (dti) {
        case '!': return parse_standard_position(pkt, info, false, false);
        case '=': return parse_standard_position(pkt, info, false, true);
        case '/': return parse_standard_position(pkt, info, true, false);
        case '@': return parse_standard_position(pkt, info, true, true);
        case '`': case '\'': case 0x1C: case 0x1D:
            return parse_mice(pkt, info, dest);
        case ';': return parse_object(pkt, info);
        case ')': return parse_item(pkt, info);
        case ':': return parse_message_payload(pkt, info);
        case '_': return parse_weather(pkt, info);
        case '>': return parse_status(pkt, info);
        case 'T': return parse_telemetry(pkt, info);
        case '<': pkt.type = PacketType::Capabilities; pkt.comment = info.substr(1); return pkt;
        case '?': pkt.type = PacketType::Query;        pkt.comment = info.substr(1); return pkt;
        case '}': pkt.type = PacketType::ThirdParty;   pkt.comment = info.substr(1); return pkt;
        case '{': pkt.type = PacketType::UserDefined;  pkt.comment = info.substr(1); return pkt;
        default:  pkt.type = PacketType::Unknown;      pkt.comment = info;           return pkt;
    }
}

// ═══════════════════════════════════════════════════════════════════
// Standard Position
// ═══════════════════════════════════════════════════════════════════

static double parse_ddmm_hh(const std::string& s, int deg_digits) {
    if (s.size() < (size_t)(deg_digits + 2)) return 0;
    auto dot = s.find('.');
    if (dot == std::string::npos || dot <= (size_t)deg_digits) return 0;
    double deg = strtod(s.substr(0, deg_digits).c_str(), nullptr);
    double min = strtod(s.substr(deg_digits).c_str(), nullptr);
    return deg + min / 60.0;
}

AprsPacket parse_standard_position(AprsPacket pkt, const std::string& info,
                                    bool has_timestamp, bool has_messaging) {
    pkt.type = PacketType::Position;
    size_t p = has_timestamp && info.size() > 7 ? 8 : 1;
    if (p >= info.size()) return pkt;

    // Timestamp
    if (has_timestamp && info.size() > 7)
        pkt.timestamp = info.substr(1, 7);

    // Check for compressed format
    char c0 = info[p];
    bool is_compressed = (c0 == '/' || c0 == '\\' || isalpha((unsigned char)c0));

    if (is_compressed && info.size() >= p + 13) {
        pkt.symbol_table = c0;
        pkt.symbol_code  = info[p + 9];
        pkt.has_position = true;
        pkt.lat = b91lat(info.substr(p + 1, 4));
        pkt.lon = b91lon(info.substr(p + 5, 4));
        if (info.size() >= p + 13) {
            std::string cs = info.substr(p + 10, 2);
            int Tb = b91val(info[p + 12]);
            bool is_gga = ((Tb >> 3) & 0x03) == 2;
            if (is_gga) pkt.altitude = b91alt(cs);
            else        b91cs(cs, pkt.course, pkt.speed);
        }
        pkt.comment = info.substr(p + 13);
    } else {
        // Uncompressed: ddmm.hhN/dddmm.hhW$c
        auto lat_end = info.find_first_of("NSns", p);
        if (lat_end == std::string::npos || lat_end < p + 4) return pkt;
        double lat = parse_ddmm_hh(info.substr(p, lat_end - p), 2);
        if (std::toupper((unsigned char)info[lat_end]) == 'S') lat = -lat;
        p = lat_end + 1;
        if (p >= info.size()) return pkt;

        pkt.symbol_table = info[p++];
        if (p >= info.size()) return pkt;

        auto lon_end = info.find_first_of("EWew", p);
        if (lon_end == std::string::npos || lon_end < p + 5) return pkt;
        double lon = parse_ddmm_hh(info.substr(p, lon_end - p), 3);
        if (std::toupper((unsigned char)info[lon_end]) == 'W') lon = -lon;
        p = lon_end + 1;

        pkt.lat = lat; pkt.lon = lon;
        pkt.has_position = true;
        if (p < info.size()) pkt.symbol_code = info[p++];

        pkt.comment = info.substr(p);
    }
    return pkt;
}

// ═══════════════════════════════════════════════════════════════════
// Mic-E
// ═══════════════════════════════════════════════════════════════════

AprsPacket parse_mice(AprsPacket pkt, const std::string& info,
                       const std::string& dest) {
    pkt.type = PacketType::MicE;
    if (info.size() < 9 || dest.size() < 6) return pkt;

    char d[6];
    for (int i = 0; i < 6; i++) d[i] = dest[i];

    int msg_bits[3] = {0,0,0};
    bool msg_std[3] = {false,false,false};
    int lat_digits[6] = {};
    char ns = 'N';
    int long_offset = 0;
    char ew = 'E';

    for (int i = 0; i < 6; i++) {
        unsigned char c = (unsigned char)d[i];
        if (c >= '0' && c <= '9') {
            lat_digits[i] = c - '0';
        } else if (c >= 'A' && c <= 'J') {
            lat_digits[i] = c - 'A';
            if (i < 3) { msg_bits[i] = 1; msg_std[i] = false; }
        } else if (c >= 'P' && c <= 'Y') {
            lat_digits[i] = c - 'P';
            if (i < 3) { msg_bits[i] = 1; msg_std[i] = true; }
            if (i == 3) ns = 'N';
            if (i == 4) long_offset = 100;
            if (i == 5) ew = 'W';
        } else if (c == 'Z') {
            lat_digits[i] = 0;
            if (i < 3) { msg_bits[i] = 1; msg_std[i] = true; }
            if (i == 3) ns = 'N';
            if (i == 4) long_offset = 100;
            if (i == 5) ew = 'W';
        } else if (c == 'K' || c == 'L') {
            lat_digits[i] = 0;
            if (i == 3) ns = 'S';
        } else {
            lat_digits[i] = 0;
        }
    }

    double lat_deg = lat_digits[0] * 10.0 + lat_digits[1];
    double lat_min = lat_digits[2] * 10.0 + lat_digits[3] + lat_digits[4] * 0.1 + lat_digits[5] * 0.01;
    double lat_val = lat_deg + lat_min / 60.0;
    if (ns == 'S') lat_val = -lat_val;

    int code = (msg_bits[0] << 2) | (msg_bits[1] << 1) | msg_bits[2];
    bool has_standard = msg_std[0] || msg_std[1] || msg_std[2];
    if (code == 0) pkt.message_text = "Emergency";
    else if (has_standard) {
        static const char* msgs[] = {"Off Duty","En Route","In Service","Returning",
                                     "Committed","Special","Priority","Custom"};
        pkt.message_text = msgs[code];
    }

    unsigned char ib[16]{};
    for (size_t i = 0; i < info.size() && i < 16; i++)
        ib[i] = (unsigned char)info[i];

    int d_val = ib[1] - 28, m_val = ib[2] - 28, h_val = ib[3] - 28;
    if (m_val >= 60) m_val -= 60;
    if (long_offset == 100) d_val += 100;
    if (d_val >= 180 && d_val <= 189) d_val -= 80;
    if (d_val >= 190 && d_val <= 199) d_val -= 190;
    double lon_val = d_val + m_val / 60.0 + h_val / 6000.0;
    if (ew == 'W') lon_val = -lon_val;

    pkt.lat = lat_val; pkt.lon = lon_val;
    pkt.has_position = true;

    if (info.size() >= 8) {
        int sp = ib[4] - 28, dc = ib[5] - 28, se = ib[6] - 28;
        int st = (sp >= 80) ? (sp - 80) * 10 : sp * 10;
        int speed = st + dc / 10;
        if (speed >= 800) speed -= 800;
        int course = (dc % 10) * 100 + se;
        if (course >= 400) course -= 400;
        pkt.speed = speed; pkt.course = course;
    }
    if (info.size() >= 9) {
        pkt.symbol_code = info[7];
        pkt.symbol_table = info[8];
    }
    if (info.size() >= 10) {
        std::string rest = info.substr(9);
        auto brace = rest.find('}');
        if (brace != std::string::npos && brace >= 3 && brace <= 5)
            pkt.altitude = b91alt(rest.substr(0, brace));
        pkt.comment = rest;
    }
    return pkt;
}

// ═══════════════════════════════════════════════════════════════════
// Object / Item
// ═══════════════════════════════════════════════════════════════════

AprsPacket parse_object(AprsPacket pkt, const std::string& info) {
    pkt.type = PacketType::Object;
    if (info.size() < 11) return pkt;
    pkt.object_name = info.substr(1, 9);
    while (!pkt.object_name.empty() && pkt.object_name.back() == ' ')
        pkt.object_name.pop_back();
    size_t p = 10;
    if (p < info.size()) { pkt.object_alive = (info[p] == '*'); p++; }
    std::string rest = info.substr(p);
    if (rest.size() >= 7 && isdigit((unsigned char)rest[0]) && rest[6] == 'z') {
        pkt.timestamp = rest.substr(0, 7);
        rest = rest.substr(7);
    }
    if (!rest.empty()) {
        if (rest[0] == '/') rest = "!" + rest;
        else if (rest[0] >= '0' && rest[0] <= '9') rest = "!" + rest;
        pkt = parse_standard_position(pkt, rest, false, true);
        pkt.type = PacketType::Object;
    }
    return pkt;
}

AprsPacket parse_item(AprsPacket pkt, const std::string& info) {
    pkt.type = PacketType::Item;
    if (info.size() < 5) return pkt;
    auto sep = info.find_first_of("!_");
    if (sep == std::string::npos || sep < 1) return pkt;
    pkt.object_name = info.substr(1, sep - 1);
    pkt.object_alive = (info[sep] == '!');
    std::string rest = "!" + info.substr(sep + 1);
    pkt = parse_standard_position(pkt, rest, false, true);
    pkt.type = PacketType::Item;
    return pkt;
}

// ═══════════════════════════════════════════════════════════════════
// Message
// ═══════════════════════════════════════════════════════════════════

AprsPacket parse_message_payload(AprsPacket pkt, const std::string& info) {
    pkt.type = PacketType::Message;
    auto colon2 = info.find(':', 1);
    if (colon2 != std::string::npos) {
        pkt.msg_addressee = info.substr(1, colon2 - 1);
        while (!pkt.msg_addressee.empty() && pkt.msg_addressee.back() == ' ')
            pkt.msg_addressee.pop_back();
        std::string text = info.substr(colon2 + 1);
        auto brace = text.rfind('{');
        if (brace != std::string::npos && brace + 1 < text.size() && text.back() == '}') {
            pkt.msg_id = text.substr(brace + 1, text.size() - brace - 2);
            text = text.substr(0, brace);
        }
        pkt.msg_body = text;
    }
    if (!pkt.msg_addressee.empty())
        pkt.comment = "to " + pkt.msg_addressee + ": " + pkt.msg_body;
    return pkt;
}

// ═══════════════════════════════════════════════════════════════════
// Weather
// ═══════════════════════════════════════════════════════════════════

void parse_weather_fields(AprsPacket& pkt, const std::string& data) {
    size_t pos = 0;
    auto next = [&]() -> std::string {
        while (pos < data.size() && data[pos] == ' ') pos++;
        size_t start = pos;
        while (pos < data.size() && data[pos] != ' ') pos++;
        if (start >= data.size()) return {};
        return data.substr(start, pos - start);
    };
    auto tok = next();
    if (tok.size() >= 4 && tok.find('/') != std::string::npos) tok = next();
    while (!tok.empty()) {
        if (tok.size() >= 2 && tok[0] == 'g')
            pkt.wx_wind_gust = (int)strtol(tok.substr(1).c_str(), nullptr, 10);
        else if (tok.size() >= 2 && tok[0] == 't')
            pkt.wx_temp_f = (int)strtol(tok.substr(1).c_str(), nullptr, 10);
        else if (tok.size() >= 2 && tok[0] == 'r')
            pkt.wx_rain_1h = (int)strtol(tok.substr(1).c_str(), nullptr, 10);
        else if (tok.size() >= 2 && tok[0] == 'p')
            pkt.wx_rain_24h = (int)strtol(tok.substr(1).c_str(), nullptr, 10);
        else if (tok.size() >= 2 && tok[0] == 'P')
            pkt.wx_rain_midnt = (int)strtol(tok.substr(1).c_str(), nullptr, 10);
        else if (tok.size() >= 2 && tok[0] == 'h')
            pkt.wx_humidity = (int)strtol(tok.substr(1).c_str(), nullptr, 10);
        else if (tok.size() >= 2 && tok[0] == 'b')
            pkt.wx_baro_mb = strtof(tok.substr(1).c_str(), nullptr) / 10.0f;
        tok = next();
    }
    pkt.has_weather = true;
}

AprsPacket parse_weather(AprsPacket pkt, const std::string& info) {
    pkt.type = PacketType::Weather;
    if (info.size() < 10) return pkt;
    size_t p = 8; // skip _DDHHMMz
    std::string rest = info.substr(p);
    auto slash = rest.find('/');
    if (slash != std::string::npos && slash < 7) {
        pkt.wx_wind_dir   = (int)strtol(rest.substr(0, slash).c_str(), nullptr, 10);
        pkt.wx_wind_speed = (int)strtol(rest.substr(slash + 1, 3).c_str(), nullptr, 10);
        rest = rest.substr(slash + 4);
    }
    parse_weather_fields(pkt, rest);
    return pkt;
}

// ═══════════════════════════════════════════════════════════════════
// Telemetry
// ═══════════════════════════════════════════════════════════════════

AprsPacket parse_telemetry(AprsPacket pkt, const std::string& info) {
    pkt.type = PacketType::Telemetry;
    if (info.size() < 4 || info[1] != '#') return pkt;
    pkt.telem_seq = (int)strtol(info.substr(2, 3).c_str(), nullptr, 10);

    auto c1 = info.find(',', 5);
    auto c2 = info.find(',', c1 + 1);
    auto c3 = info.find(',', c2 + 1);
    auto c4 = info.find(',', c3 + 1);
    auto c5 = info.find(',', c4 + 1);
    if (c1 == std::string::npos || c5 == std::string::npos) return pkt;

    pkt.telem_a[0] = (int)strtol(info.substr(c1 + 1, 3).c_str(), nullptr, 10);
    pkt.telem_a[1] = (int)strtol(info.substr(c2 + 1, 3).c_str(), nullptr, 10);
    pkt.telem_a[2] = (int)strtol(info.substr(c3 + 1, 3).c_str(), nullptr, 10);
    pkt.telem_a[3] = (int)strtol(info.substr(c4 + 1, 3).c_str(), nullptr, 10);
    pkt.telem_a[4] = (int)strtol(info.substr(c5 + 1, 3).c_str(), nullptr, 10);

    auto bits_start = c5 + 4;
    if (bits_start + 8 <= info.size()) {
        pkt.telem_digital = 0;
        for (int i = 0; i < 8; i++)
            if (info[bits_start + i] == '1')
                pkt.telem_digital |= (1 << i);
    }
    return pkt;
}

// ═══════════════════════════════════════════════════════════════════
// Status
// ═══════════════════════════════════════════════════════════════════

AprsPacket parse_status(AprsPacket pkt, const std::string& info) {
    pkt.type = PacketType::Status;
    size_t p = 1;
    if (info.size() >= 8 && isdigit((unsigned char)info[1]) && info[7] == 'z') {
        pkt.timestamp = info.substr(1, 7);
        p = 8;
    }
    pkt.comment = info.substr(p);
    return pkt;
}

// ═══════════════════════════════════════════════════════════════════
// Summary formatter — one-line human-readable packet
// ═══════════════════════════════════════════════════════════════════

static const char* type_name(PacketType t) {
    switch (t) {
        case PacketType::Position:  return "Pos";
        case PacketType::MicE:      return "MicE";
        case PacketType::Object:    return "Obj";
        case PacketType::Item:      return "Item";
        case PacketType::Message:   return "Msg";
        case PacketType::Weather:   return "WX";
        case PacketType::Telemetry: return "TLM";
        case PacketType::Status:    return "Sts";
        case PacketType::Query:     return "Qry";
        case PacketType::Capabilities: return "Cap";
        case PacketType::ThirdParty:   return "3rd";
        case PacketType::UserDefined:  return "Def";
        default: return "Unk";
    }
}

std::string format_summary(const AprsPacket& pkt) {
    char buf[512];
    if (pkt.has_position) {
        if (pkt.type == PacketType::Message && !pkt.msg_body.empty()) {
            snprintf(buf, sizeof(buf), "%s: %s @ %.4f,%.4f  %s",
                     type_name(pkt.type), pkt.comment.c_str(),
                     pkt.lat, pkt.lon, pkt.raw_payload.c_str());
        } else if (pkt.type == PacketType::Weather && pkt.has_weather) {
            char wx[128]{};
            if (pkt.wx_temp_f != 0) {
                char t[32]; snprintf(t, sizeof(t), "%dF ", pkt.wx_temp_f);
                strcat(wx, t);
            }
            if (pkt.wx_wind_speed >= 0) {
                char w[48]; snprintf(w, sizeof(w), "%d@%d ", pkt.wx_wind_speed, pkt.wx_wind_dir);
                strcat(wx, w);
            }
            snprintf(buf, sizeof(buf), "%s %s", wx, pkt.comment.c_str());
        } else {
            snprintf(buf, sizeof(buf), "%s @ %.4f,%.4f  %s",
                     type_name(pkt.type), pkt.lat, pkt.lon, pkt.comment.c_str());
        }
    } else if (!pkt.comment.empty()) {
        snprintf(buf, sizeof(buf), "%s: %s", type_name(pkt.type), pkt.comment.c_str());
    } else {
        snprintf(buf, sizeof(buf), "%s", pkt.raw_payload.c_str());
    }
    std::string result(buf);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

} // namespace aprs
