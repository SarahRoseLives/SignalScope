// OOB (out-of-band) cable EPG decoder core — constants and pipeline types.
#pragma once

#include <complex>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace oob {

// --- EPG result store (singleton, thread-safe) ---
struct EpgSnapshot {
    std::vector<std::pair<std::string, int>> channels; // callsign -> channel#
    std::vector<std::string> serviceNames;
    std::vector<std::string> readableStrings; // ASCII strings from ATM payloads
    std::string epgText; // decompressed EPG data (show names, times, descriptions)
    std::string macText;  // MAC control-channel messages (VCI 0x0021)
    std::string hostConfigText; // OCAP host configuration (VCI 0x0FA2)
    int totalSuperframes = 0;
    int totalCells = 0;
    int cleanCells = 0;
    int correctedCells = 0;
    int failedCells = 0;
    int hecPass = 0;         // cells that pass HEC
    int hecFail = 0;         // cells that fail HEC
    int aal5Frames = 0;      // reassembled AAL5 frames
    int ipPkts = 0;          // IP/UDP packets extracted
    int dsmccMods = 0;       // DSM-CC modules reassembled
    int biopObjs = 0;        // BIOP objects found
    int carouselVpi = -1;    // detected carousel VPI
    int carouselVci = -1;    // detected carousel VCI
    bool gotLock = false;
    double lastUpdate = 0.0; // epoch seconds
};

class EpgStore {
public:
    static EpgStore& instance() { static EpgStore s; return s; }
    void update(EpgSnapshot&& d) {
        std::lock_guard<std::mutex> lk(mtx_);
        // Merge channels: deduplicate by callsign, new values overwrite old
        if (!d.channels.empty()) {
            for (auto& p : d.channels) {
                bool found = false;
                for (auto& e : data_.channels) {
                    if (e.first == p.first) { e.second = p.second; found = true; break; }
                }
                if (!found) data_.channels.push_back(p);
            }
        }
        // Merge service names: accumulate into set
        if (!d.serviceNames.empty()) {
            std::set<std::string> svc(data_.serviceNames.begin(), data_.serviceNames.end());
            for (auto& s : d.serviceNames) svc.insert(s);
            data_.serviceNames.assign(svc.begin(), svc.end());
        }
        // Merge readable strings: accumulate
        if (!d.readableStrings.empty()) {
            std::set<std::string> rs(data_.readableStrings.begin(), data_.readableStrings.end());
            for (auto& s : d.readableStrings) rs.insert(s);
            data_.readableStrings.assign(rs.begin(), rs.end());
        }
        // Merge EPG text: concatenate with separator
        if (!d.epgText.empty()) {
            if (!data_.epgText.empty()) data_.epgText += "\n";
            data_.epgText += d.epgText;
            if (data_.epgText.size() > 65536)
                data_.epgText = data_.epgText.substr(data_.epgText.size() - 65536);
        }
        // MAC messages: only replace on successful decode
        if (!d.macText.empty()) data_.macText = d.macText;
        // Host config: only replace on successful decode
        if (!d.hostConfigText.empty()) data_.hostConfigText = d.hostConfigText;
        // Stats and lock status always overwrite (latest run)
        data_.gotLock = d.gotLock;
        data_.lastUpdate = d.lastUpdate;
        data_.totalSuperframes = d.totalSuperframes;
        data_.totalCells = d.totalCells;
        data_.cleanCells = d.cleanCells;
        data_.correctedCells = d.correctedCells;
        data_.failedCells = d.failedCells;
        data_.hecPass = d.hecPass;
        data_.hecFail = d.hecFail;
        data_.aal5Frames = d.aal5Frames;
        data_.ipPkts = d.ipPkts;
        data_.dsmccMods = d.dsmccMods;
        data_.biopObjs = d.biopObjs;
        data_.carouselVpi = d.carouselVpi;
        data_.carouselVci = d.carouselVci;
    }
    void reset() {
        std::lock_guard<std::mutex> lk(mtx_);
        data_ = EpgSnapshot{};
    }
    EpgSnapshot snapshot() {
        std::lock_guard<std::mutex> lk(mtx_);
        return data_;
    }
private:
    mutable std::mutex mtx_;
    EpgSnapshot data_;
};

// --- Protocol constants (SCTE 55-2) ---
constexpr double SYMBOL_RATE = 772000.0;
constexpr double ROLLOFF = 0.30;
constexpr int    DEFAULT_SPS = 4;           // samples per symbol after resampling
constexpr int    SF_BITS = 4632;            // superframe bits
constexpr int    FRAME_BITS = 193;          // frame bits
constexpr int    NUM_FRAMES = 24;
constexpr int    SF_PAYLOAD_BITS = SF_BITS - NUM_FRAMES; // 4608
constexpr int    SF_PAYLOAD_BYTES = SF_PAYLOAD_BITS / 8; // 576

// FAS bit pattern in overhead bit positions of frames 4,8,12,16,20,24
constexpr int    FAS_BITS[6] = {0, 0, 1, 0, 1, 1};
constexpr int    FAS_POS[6] = {579, 1351, 2123, 2895, 3667, 4439};
constexpr int    OH_POS[NUM_FRAMES] = {0, 193, 386, 579, 772, 965, 1158, 1351,
                                       1544, 1737, 1930, 2123, 2316, 2509, 2702,
                                       2895, 3088, 3281, 3474, 3667, 3860, 4053,
                                       4246, 4439};

// Differential phase -> dibit map: 0deg->[0,0], +90->[1,0], 180->[1,1], -90->[0,1]
// (MSB=A, LSB=B. Index: quantized phase quadrant 0,1,2,3 -> 0deg, +90, 180, -90)
constexpr int    PHASE_TO_DIBIT[4] = {0, 2, 3, 1};

// Self-sync derandomizer: x^6 + x + 1  => out[n] = in[n] ^ in[n-1] ^ in[n-6]
constexpr int    DERAND_D1 = 1;
constexpr int    DERAND_D2 = 6;

// FEC
constexpr int    RS_N = 55;
constexpr int    RS_K = 53;
constexpr int    RS_T = 1;
constexpr int    INTERLEAVE_I = 5;
constexpr int    INTERLEAVE_M = 11;
constexpr int    INTERLEAVE_PRIME = (INTERLEAVE_I - 1) * INTERLEAVE_M * INTERLEAVE_I; // 220

// 10 RS codewords per SL-ESF payload, at these byte offsets:
constexpr int    PACKET_OFFSETS[10] = {2, 59, 117, 174, 232, 289, 347, 404, 462, 519};

// GF(256) primitive: x^8 + x^4 + x^3 + x^2 + 1
constexpr int    GF_PRIM = 0x11D;

// ATM
constexpr int    ATM_CELL_LEN = 53;
constexpr int    ATM_HEADER_LEN = 5;
constexpr int    ATM_PAYLOAD_LEN = 48;

// Carousel
constexpr int    CAROUSEL_VPI = 0xFF;
constexpr int    CAROUSEL_VCI = 0xFFFF;

// A stage callback reports progress or decoded records to the decoder.
struct EpgEntry {
    std::string callsign;
    int         channelNumber = 0;
};

struct PipelineResult {
    std::vector<EpgEntry>  channels;
    std::vector<std::string> serviceNames;
    std::vector<std::string> readableStrings;
    std::string epgText; // decompressed EPG data (show names, times, descriptions)
    int     totalSuperframes = 0;
    int     totalCells = 0;
    int     cleanCells = 0;
    int     correctedCells = 0;
    int     failedCells = 0;
    bool    gotLock = false;
};

// --- RRC filter tap generation ---
std::vector<float> makeRrcTaps(double beta, int sps, int span = 10);

// --- QPSK demodulation ---
// coarseEstimate: find carrier offset via 4th-power method
double coarseCarrierOffset(const std::vector<std::complex<double>>& sig, double fs);
// fullDemod: coarse freq removal -> resample -> RRC filter -> Gardner -> Costas -> normalize
std::vector<std::complex<double>> demodulateQpsk(
    const std::vector<std::complex<double>>& sig, double fs,
    double symbolRate = SYMBOL_RATE, int sps = DEFAULT_SPS, double rolloff = ROLLOFF);

// --- Differential decode + derandomize ---
std::vector<uint8_t> symbolsToBits(const std::vector<std::complex<double>>& sym);

// --- Frame sync ---
int fasScore(const std::vector<uint8_t>& bits, int pos);
// Find FAS lock near |start| within |search| bits. Returns (score, position).
std::pair<int, int> acquireFas(const std::vector<uint8_t>& bits, int start, int search);
std::vector<std::vector<uint8_t>> trackSuperframes(
    const std::vector<uint8_t>& bits, int drift = 6, int accept = 5,
    int reacquire = 30 * SF_BITS);

// --- FEC ---
void initGfTables();
int  gfMul(int a, int b);
// Decode one RS(55,53) codeword -> (data53, nErrors, ok)
std::tuple<std::vector<uint8_t>, int, bool> rsDecode(const uint8_t* code);
// Full FEC pipeline: payloads -> atm cells
std::vector<std::vector<uint8_t>> decodeCells(const std::vector<std::vector<uint8_t>>& payloads,
                                              int* cleanOut, int* correctedOut, int* failedOut);

// --- ATM parsing ---
struct AtmHeader { int gfc; int vpi; int vci; int pt; int clp; int hec; };
AtmHeader parseAtmHeader(const uint8_t* cell);
bool hecOk(const uint8_t* cell);

// --- AAL5 / IP / UDP / DSM-CC reassembly ---
std::vector<std::vector<uint8_t>> reassembleAal5(const std::vector<std::vector<uint8_t>>& cells,
                                                  int vpi = CAROUSEL_VPI, int vci = CAROUSEL_VCI);
// Returns (src, dst, dstPort, udpPayload) tuples
struct IpUdpPkt { std::string src; std::string dst; int dstPort; std::vector<uint8_t> payload; };
std::vector<IpUdpPkt> ipUdpPayloads(const std::vector<std::vector<uint8_t>>& aal5Frames);
// Returns {(tag, moduleId) -> data} map
std::map<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>, std::vector<uint8_t>>
reassembleModules(const std::vector<IpUdpPkt>& pkts, int port = 13821);

// --- BIOP parsing ---
struct BiopObject { std::string kind; std::vector<uint8_t> key; std::vector<uint8_t> content; };
std::vector<BiopObject> parseBiop(const std::vector<uint8_t>& blob);

// --- Zlib decompression ---
// Try to decompress zlib-compressed data. Returns true on success.
bool inflateZlib(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out);

// --- EPG data extraction from all modules ---
// Concatenate all modules into one blob, parse BIOP objects, try decompression,
// and return human-readable text (show names, descriptions, guide data).
std::string extractEpgText(
    const std::map<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>, std::vector<uint8_t>>& modules);

// Scan concatenated raw cell payloads for zlib-compressed blobs (like inflate.py)
std::string scanZlibForText(const std::vector<uint8_t>& payloadBlob);

// --- MAC control-channel decoder (VCI 0x0021) ---
std::string decodeMacMessages(const std::vector<std::vector<uint8_t>>& cells);

// --- OCAP host-config extractor (VCI 0x0FA2) ---
std::string decodeHostConfig(const std::vector<std::vector<uint8_t>>& cells);

// --- NRSC-5 HD Radio store (thread-safe singleton) ---
#ifdef HAS_NRSC5

struct Nrsc5Info {
    std::string stationName;
    std::string stationSlogan;
    std::string stationMessage;
    std::string stationLocation; // lat, lon, alt
    std::string stationId;       // country, facility ID
    std::string trackTitle;
    std::string trackArtist;
    std::string trackAlbum;
    std::string trackGenre;
    std::string programType;     // program type name for current program
    std::string codecInfo;       // codec mode, blend, gain, latency
    std::string localTime;       // local time string
    float merLower = 0;
    float merUpper = 0;
    float ber = 0;
    bool sync = false;
    int activeProgram = 0;
    std::vector<std::string> programs;
};

class Nrsc5Store {
public:
    static Nrsc5Store& instance() { static Nrsc5Store s; return s; }
    void update(const Nrsc5Info& d) { std::lock_guard<std::mutex> lk(mtx_); data_ = d; }
    void setProgram(int p) { std::lock_guard<std::mutex> lk(mtx_); data_.activeProgram = p; }
    Nrsc5Info snapshot() { std::lock_guard<std::mutex> lk(mtx_); return data_; }
private:
    mutable std::mutex mtx_;
    Nrsc5Info data_;
};
#endif

// --- EPG extraction ---
std::pair<std::vector<EpgEntry>, std::vector<std::string>>
extractEpg(const std::map<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>, std::vector<uint8_t>>& modules);

} // namespace oob
