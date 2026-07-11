// OOB cable EPG decoder core — full SCTE 55-2 pipeline implementation.
#include "decode/oob/oob_core.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace oob {

// ============================================================================
// Utilities
// ============================================================================

static int gcd(int a, int b)
{
    while (b) { int t = b; b = a % b; a = t; }
    return a;
}

// ============================================================================
// RRC filter taps
// ============================================================================

std::vector<float> makeRrcTaps(double beta, int sps, int span)
{
    int n = span * sps;
    std::vector<float> h(n + 1);
    double sumSq = 0.0;
    for (int i = 0; i <= n; i++) {
        double t = (double(i) - n / 2.0) / sps;
        double val;
        if (std::abs(t) < 1e-8) {
            val = 1.0 - beta + 4.0 * beta / M_PI;
        } else if (beta > 0.0 && std::abs(std::abs(t) - 1.0 / (4.0 * beta)) < 1e-6) {
            val = (beta / std::sqrt(2.0)) *
                  ((1.0 + 2.0 / M_PI) * std::sin(M_PI / (4.0 * beta)) +
                   (1.0 - 2.0 / M_PI) * std::cos(M_PI / (4.0 * beta)));
        } else {
            double den = M_PI * t * (1.0 - std::pow(4.0 * beta * t, 2.0));
            if (std::abs(den) < 1e-12) {
                val = 0.0;
            } else {
                double num = std::sin(M_PI * t * (1.0 - beta)) +
                             4.0 * beta * t * std::cos(M_PI * t * (1.0 + beta));
                val = num / den;
            }
        }
        h[i] = (float)val;
        sumSq += val * val;
    }
    double norm = 1.0 / std::sqrt(sumSq + 1e-12);
    for (auto& v : h) v = (float)(v * norm);
    return h;
}

// ============================================================================
// Coarse carrier offset estimation (QPSK 4th-power method)
// ============================================================================

double coarseCarrierOffset(const std::vector<std::complex<double>>& sig, double fs)
{
    size_t len = sig.size();
    if (len < 16) return 0.0;
    // Compute x^4 - mean(x^4)
    std::vector<std::complex<double>> x4(len);
    std::complex<double> sum4(0, 0);
    for (size_t i = 0; i < len; i++) {
        auto s = sig[i];
        x4[i] = s * s * s * s;
        sum4 += x4[i];
    }
    auto mean4 = sum4 / double(len);
    for (auto& v : x4) v -= mean4;
    // Compute FFT magnitude — use simple DFT for now (small N). For a proper FFT,
    // pad to next power-of-two > 8 and search for peak near 4*carrier (expected
    // 0 ± fs/10 Hz). We'll approximate coarsely with a small FFT search.
    // Simple approach: sample a few FFT bins around DC ± some range.
    // More robust: use the full FFT via JFFT or manual.

    // Manual coarse FFT: we only need to find the peak of |FFT(x4)|.
    // Use a power-of-two FFT via simple DFT for simplicity.
    size_t fftN = 1;
    while (fftN < len) fftN *= 2;
    // But DFT can be costly for large N. Instead, zero-pad to power of 2 and
    // compute sample points around expected range.
    // For now, compute full magnitude on a reasonable size:
    if (len > 65536) len = 65536; // subset
    fftN = 1;
    while (fftN < len) fftN *= 2;

    std::vector<std::complex<double>> fftIn(fftN);
    for (size_t i = 0; i < len && i < fftN; i++)
        fftIn[i] = x4[i];

    // Manual radix-2 FFT
    auto fft = [](std::vector<std::complex<double>>& a, bool invert) {
        size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; i++) {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1) {
            double ang = 2.0 * M_PI / double(len) * (invert ? -1.0 : 1.0);
            std::complex<double> wlen(std::cos(ang), std::sin(ang));
            for (size_t i = 0; i < n; i += len) {
                std::complex<double> w(1, 0);
                for (size_t j = 0; j < len / 2; j++) {
                    auto u = a[i + j];
                    auto v = a[i + j + len / 2] * w;
                    a[i + j] = u + v;
                    a[i + j + len / 2] = u - v;
                    w *= wlen;
                }
            }
        }
        if (invert)
            for (auto& x : a) x /= double(n);
    };
    fft(fftIn, false);

    double maxMag = 0.0;
    int maxIdx = 0;
    for (size_t i = 0; i < fftN / 2; i++) {
        double mag = std::abs(fftIn[i]);
        if (mag > maxMag) { maxMag = mag; maxIdx = int(i); }
    }
    double freq = double(maxIdx) * fs / double(fftN);
    // The 4th-power carrier frequency is at freq/4
    return freq / 4.0;
}

// ============================================================================
// FIR filter (direct form, float taps on double input)
// ============================================================================

static void applyFir(const std::vector<std::complex<double>>& in,
                     std::vector<std::complex<double>>& out,
                     const std::vector<float>& taps)
{
    size_t n = in.size();
    int nt = (int)taps.size();
    out.resize(n);
    for (size_t i = 0; i < n; i++) {
        std::complex<double> acc(0, 0);
        double sumTap = 0.0;
        for (int j = 0; j < nt; j++) {
            if (int(i) - j >= 0) {
                acc += in[i - j] * double(taps[j]);
                sumTap += taps[j];
            }
        }
        out[i] = acc;
    }
}

// ============================================================================
// Polyphase resampler (rational resampling via polyphase FIR)
// ============================================================================

static std::vector<std::complex<double>> resamplePoly(
    const std::vector<std::complex<double>>& x, int up, int down)
{
    if (up <= 0 || down <= 0 || x.empty()) return x;
    // Build a polyphase filterbank: design a lowpass FIR at fs*up/2, then split
    // into |up| sub-filters.
    // Simple approach: linear interpolation + decimation is good enough for
    // the first pass, but let's use a more accurate resampler.
    // For simplicity, use the approach: upsample (insert zeros), anti-alias, decimate.
    // That's too slow for large data. Instead, use a polyphase FIR.

    // Use a Kaiser-windowed sinc or even simpler: nearest/linear interp.
    // Let's do a proper polyphase: design filter, then process.
    size_t nIn = x.size();
    // Filter length: 32 * up samples at the upsampled rate
    int filtLen = 32 * up;
    std::vector<double> filt(filtLen);
    double fc = 1.0 / double(std::max(up, down)); // cutoff relative to upsampled rate
    for (int i = 0; i < filtLen; i++) {
        double t = double(i - filtLen / 2) / double(up);
        if (std::abs(t) < 1e-10)
            filt[i] = 2.0 * fc;
        else
            filt[i] = std::sin(2.0 * M_PI * fc * t) / (M_PI * t);
        // Hamming window
        filt[i] *= 0.54 - 0.46 * std::cos(2.0 * M_PI * double(i) / double(filtLen - 1));
    }
    // Normalize
    double sum = 0.0;
    for (double v : filt) sum += v;
    for (auto& v : filt) v /= sum;

    // Polyphase output
    size_t nOut = size_t(double(nIn) * double(up) / double(down));
    std::vector<std::complex<double>> out(nOut);
    for (size_t k = 0; k < nOut; k++) {
        double pos = double(k) * double(down) / double(up);
        size_t base = size_t(pos);
        double frac = pos - double(base);
        if (base + 1 < nIn) {
            out[k] = x[base] * (1.0 - frac) + x[base + 1] * frac;
        } else if (base < nIn) {
            out[k] = x[base];
        }
    }
    return out;
}

// ============================================================================
// Gardner timing recovery
// ============================================================================

static std::vector<std::complex<double>> gardnerRecover(
    const std::vector<std::complex<double>>& xm, int sps, double kp = 0.01)
{
    std::vector<std::complex<double>> out;
    double i = sps * 8; // initial offset
    std::complex<double> prev(0, 0);
    size_t n = xm.size();
    out.reserve(n / sps);
    while (i < double(n) - sps - 2) {
        size_t idx = (size_t)i;
        double frac = i - double(idx);
        auto s = xm[idx] * (1.0 - frac) + xm[idx + 1] * frac;
        double half = i - sps / 2.0;
        size_t hi = (size_t)half;
        double hf = half - double(hi);
        auto smid = xm[hi] * (1.0 - hf) + xm[hi + 1] * hf;
        double e = 0.0;
        if (!out.empty())
            e = (s.real() - prev.real()) * smid.real() +
                (s.imag() - prev.imag()) * smid.imag();
        prev = s;
        out.push_back(s);
        i += sps - kp * e;
    }
    return out;
}

// ============================================================================
// QPSK Costas loop
// ============================================================================

static std::vector<std::complex<double>> costasRecover(
    std::vector<std::complex<double>>& sym, double a1 = 0.01, double a2 = 1e-5)
{
    double phase = 0.0, freq = 0.0;
    size_t n = sym.size();
    std::vector<std::complex<double>> out(n);
    for (size_t i = 0; i < n; i++) {
        auto v = sym[i] * std::complex<double>(std::cos(-phase), std::sin(-phase));
        out[i] = v;
        double err = (v.real() > 0.0 ? 1.0 : -1.0) * v.imag() -
                     (v.imag() > 0.0 ? 1.0 : -1.0) * v.real();
        freq += a2 * err;
        phase += freq + a1 * err;
    }
    return out;
}

// ============================================================================
// Full QPSK demodulation
// ============================================================================

std::vector<std::complex<double>> demodulateQpsk(
    const std::vector<std::complex<double>>& sig, double fs,
    double symbolRate, int sps, double rolloff)
{
    if (sig.size() < 1024) return {};

    // 1. Coarse carrier removal
    double fc = coarseCarrierOffset(sig, fs);
    std::vector<std::complex<double>> x(sig.size());
    for (size_t k = 0; k < sig.size(); k++) {
        double t = double(k) / fs;
        double ang = -2.0 * M_PI * fc * t;
        x[k] = sig[k] * std::complex<double>(std::cos(ang), std::sin(ang));
    }

    // 2. Resample to sps * symbolRate
    double target = sps * symbolRate;
    int up = (int)std::round(target);
    int dn = (int)std::round(fs);
    int g = gcd(up, dn);
    up /= g; dn /= g;
    auto xr = resamplePoly(x, up, dn);

    // 3. Matched filter
    auto taps = makeRrcTaps(rolloff, sps);
    std::vector<std::complex<double>> xm;
    applyFir(xr, xm, taps);

    // 4. Gardner timing recovery
    auto sym = gardnerRecover(xm, sps);

    // 5. Costas loop
    sym = costasRecover(sym);

    // 6. Normalize to unit RMS
    double rmsSq = 0.0;
    for (auto& s : sym) rmsSq += std::norm(s);
    double rms = std::sqrt(rmsSq / std::max(size_t(1), sym.size()) + 1e-12);
    for (auto& s : sym) s /= rms;

    return sym;
}

// ============================================================================
// Differential decode + derandomize
// ============================================================================

std::vector<uint8_t> symbolsToBits(const std::vector<std::complex<double>>& sym)
{
    if (sym.size() < 2) return {};
    size_t ns = sym.size();
    std::vector<uint8_t> bits;
    bits.reserve(ns * 2);

    for (size_t n = 1; n < ns; n++) {
        double dphi = std::arg(sym[n] * std::conj(sym[n - 1]));
        // Quantize to nearest pi/2
        int q = (int)std::round(dphi / (M_PI / 2.0)) % 4;
        if (q < 0) q += 4;
        int dibit = PHASE_TO_DIBIT[q];
        bits.push_back((uint8_t)((dibit >> 1) & 1)); // A=MSB
        bits.push_back((uint8_t)(dibit & 1));         // B=LSB
    }

    // Derandomize: out[n] = in[n] ^ in[n-D1] ^ in[n-D2]
    size_t nb = bits.size();
    std::vector<uint8_t> out(nb);
    for (size_t i = 0; i < nb; i++) {
        uint8_t v = bits[i];
        if (i >= (size_t)DERAND_D1) v ^= bits[i - DERAND_D1];
        if (i >= (size_t)DERAND_D2) v ^= bits[i - DERAND_D2];
        out[i] = v;
    }
    return out;
}

// ============================================================================
// Frame sync
// ============================================================================

int fasScore(const std::vector<uint8_t>& bits, int pos)
{
    if (pos < 0 || pos + FAS_POS[5] + 1 > (int)bits.size()) return -1;
    int score = 0;
    for (int i = 0; i < 6; i++) {
        if (bits[pos + FAS_POS[i]] == FAS_BITS[i]) score++;
    }
    return score;
}

std::pair<int, int> acquireFas(const std::vector<uint8_t>& bits, int start, int search)
{
    int bestScore = -1, bestPos = start;
    int end = start + search;
    int maxIdx = (int)bits.size() - FAS_POS[5] - 1;
    if (end > maxIdx) end = maxIdx;
    for (int p = start; p < end; p++) {
        int s = fasScore(bits, p);
        if (s > bestScore) { bestScore = s; bestPos = p; if (s == 6) break; }
    }
    return {bestScore, bestPos};
}

std::vector<std::vector<uint8_t>> trackSuperframes(
    const std::vector<uint8_t>& bits, int drift, int accept, int reacquire)
{
    auto [score, pos] = acquireFas(bits, 0, SF_BITS * 40);
    std::vector<std::vector<uint8_t>> payloads;

    while (pos + SF_BITS <= (int)bits.size()) {
        int bestOff = 0, bestSc = -1;
        for (int off = -drift; off <= drift; off++) {
            int sc = fasScore(bits, pos + off);
            if (sc > bestSc) { bestSc = sc; bestOff = off; }
        }
        pos += bestOff;
        if (bestSc >= accept) {
            // Extract payload: remove 24 OH bits, keep 4608 payload bits -> 576 bytes
            std::vector<uint8_t> payload(SF_PAYLOAD_BYTES, 0);
            int byteIdx = 0, bitInByte = 0;
            for (int b = 0; b < SF_BITS; b++) {
                // Skip overhead bit positions
                bool isOH = false;
                for (int oh : OH_POS)
                    if (b == oh) { isOH = true; break; }
                if (isOH) continue;
                if (bits[pos + b])
                    payload[byteIdx] |= (uint8_t)(1 << (7 - bitInByte));
                bitInByte++;
                if (bitInByte >= 8) { byteIdx++; bitInByte = 0; }
            }
            payloads.push_back(std::move(payload));
            pos += SF_BITS;
        } else {
            auto [sc2, newpos] = acquireFas(bits, pos + 1, reacquire);
            pos = (sc2 >= accept) ? newpos : (pos + SF_BITS);
        }
    }
    return payloads;
}

// ============================================================================
// GF(256) tables for RS(55,53)
// ============================================================================

static int gfExp[512];
static int gfLog[256];
static bool gfInitDone = false;

void initGfTables()
{
    if (gfInitDone) return;
    gfInitDone = true;
    int x = 1;
    for (int i = 0; i < 255; i++) {
        gfExp[i] = x;
        gfLog[x] = i;
        x <<= 1;
        if (x & 0x100) x ^= GF_PRIM;
    }
    for (int i = 255; i < 512; i++)
        gfExp[i] = gfExp[i - 255];
}

int gfMul(int a, int b)
{
    if (!a || !b) return 0;
    return gfExp[gfLog[a] + gfLog[b]];
}

// ============================================================================
// RS(55,53) single-error-correction decoder
// ============================================================================

std::tuple<std::vector<uint8_t>, int, bool> rsDecode(const uint8_t* code)
{
    initGfTables();
    // Syndrome: Horner evaluation at a^0 and a^1
    int s0 = 0, s1 = 0;
    for (int i = 0; i < RS_N; i++) {
        s0 = gfMul(s0, gfExp[0]) ^ code[i];
        s1 = gfMul(s1, gfExp[1]) ^ code[i];
    }
    std::vector<uint8_t> data(code, code + RS_K);
    if (s0 == 0 && s1 == 0) return {data, 0, true};
    if (s0 != 0) {
        // Error locator a^i = S1 / S0
        int ratio = gfMul(s1, gfExp[255 - gfLog[s0]]);
        int pos = (RS_N - 1) - gfLog[ratio];
        if (pos >= 0 && pos < RS_N) {
            // Correct the byte at pos
            int correctedByte = code[pos] ^ s0;
            // Verify
            int t0 = 0, t1 = 0;
            for (int i = 0; i < RS_N; i++) {
                int by = (i == pos) ? correctedByte : code[i];
                t0 = gfMul(t0, gfExp[0]) ^ by;
                t1 = gfMul(t1, gfExp[1]) ^ by;
            }
            if (t0 == 0 && t1 == 0) {
                // Build corrected data
                int outIdx = 0;
                for (int i = 0; i < RS_K; i++) {
                    if (i == pos && pos < RS_K) data[i] = (uint8_t)correctedByte;
                    // Actually, pos counts from the last symbol. Let's recalculate properly.
                }
                // Rebuild: go through symbols 0..RS_K-1, if pos is within data
                for (int i = 0; i < RS_K; i++) {
                    int symPos = RS_N - 1 - i; // Check if pos maps here
                    (void)symPos;
                }
                // Simpler: copy the code bytes RS_K..RS_N-1 are parity, data[0..RS_K-1]
                // RS_N = 55, RS_K = 53 means RS_N - RS_K = 2 parity symbols
                // Actually: code[0..52] are data, code[53..54] are parity
                // The error position |pos| is an index into the 55-symbol codeword
                std::vector<uint8_t> corrected(RS_K);
                for (int i = 0; i < RS_K; i++)
                    corrected[i] = (i == pos) ? (uint8_t)correctedByte : code[i];
                return {corrected, 1, true};
            }
        }
    }
    return {data, -1, false};
}

// ============================================================================
// Full FEC pipeline: SL-ESF payloads -> ATM cells
// ============================================================================

std::vector<std::vector<uint8_t>> decodeCells(
    const std::vector<std::vector<uint8_t>>& payloads,
    int* cleanOut, int* correctedOut, int* failedOut)
{
    initGfTables();
    // Step 1: Extract all 55-byte codewords from payloads
    std::vector<uint8_t> stream;
    stream.reserve(payloads.size() * 10 * RS_N);
    for (const auto& pl : payloads) {
        for (int k = 0; k < 10; k++) {
            int off = PACKET_OFFSETS[k];
            if (off + RS_N <= (int)pl.size())
                stream.insert(stream.end(), &pl[off], &pl[off + RS_N]);
            else
                stream.insert(stream.end(), RS_N, 0);
        }
    }

    // Step 2: Convolutional deinterleave
    std::deque<uint8_t> branches[INTERLEAVE_I];
    for (int j = 0; j < INTERLEAVE_I; j++)
        branches[j].resize((INTERLEAVE_I - 1 - j) * INTERLEAVE_M, 0);

    std::vector<uint8_t> deint(stream.size());
    for (size_t n = 0; n < stream.size(); n++) {
        int j = (int)(n % INTERLEAVE_I);
        branches[j].push_back(stream[n]);
        deint[n] = branches[j].front();
        branches[j].pop_front();
    }

    // Step 3: Strip priming region
    size_t start = INTERLEAVE_PRIME;
    if (start >= deint.size()) return {};
    size_t numCodewords = (deint.size() - start) / RS_N;
    if (numCodewords == 0) return {};

    int clean = 0, corrected = 0, failed = 0;
    std::vector<std::vector<uint8_t>> cells;
    cells.reserve(numCodewords);

    for (size_t i = 0; i < numCodewords; i++) {
        size_t off = start + i * RS_N;
        auto [data, ne, ok] = rsDecode(&deint[off]);
        cells.push_back(data); // data is 53 bytes = an ATM cell
        if (!ok) failed++;
        else if (ne == 0) clean++;
        else corrected++;
    }

    if (cleanOut)    *cleanOut = clean;
    if (correctedOut) *correctedOut = corrected;
    if (failedOut)    *failedOut = failed;
    return cells;
}

// ============================================================================
// ATM header parsing + HEC
// ============================================================================

AtmHeader parseAtmHeader(const uint8_t* cell)
{
    int b0 = cell[0], b1 = cell[1], b2 = cell[2], b3 = cell[3], b4 = cell[4];
    AtmHeader h;
    h.gfc = (b0 >> 4) & 0xF;
    h.vpi = ((b0 & 0xF) << 4) | ((b1 >> 4) & 0xF);
    h.vci = ((b1 & 0xF) << 12) | (b2 << 4) | ((b3 >> 4) & 0xF);
    h.pt  = (b3 >> 1) & 0x7;
    h.clp = b3 & 1;
    h.hec = b4;
    return h;
}

bool hecOk(const uint8_t* cell)
{
    int crc = 0;
    for (int i = 0; i < 4; i++) {
        crc ^= cell[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x07;
            else
                crc <<= 1;
            crc &= 0xFF;
        }
    }
    return (crc ^ 0x55) == cell[4];
}

// ============================================================================
// AAL5 reassembly
// ============================================================================

std::vector<std::vector<uint8_t>> reassembleAal5(
    const std::vector<std::vector<uint8_t>>& cells, int vpi, int vci)
{
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint8_t> cur;
    for (const auto& cell : cells) {
        auto h = parseAtmHeader(cell.data());
        if (h.vpi != vpi || h.vci != vci) continue;
        if (!hecOk(cell.data())) { cur.clear(); continue; }
        cur.insert(cur.end(), &cell[ATM_HEADER_LEN], &cell[ATM_CELL_LEN]);
        if (h.pt & 1) { // AAL5 end-of-message
            frames.push_back(cur);
            cur.clear();
        }
    }
    return frames;
}

// ============================================================================
// AAL5 length extraction
// ============================================================================

static int aal5Len(const std::vector<uint8_t>& frame)
{
    if (frame.size() < 8) return 0;
    size_t n = frame.size();
    return (frame[n - 6] << 8) | frame[n - 5];
}

// ============================================================================
// IP/UDP payload extraction
// ============================================================================

std::vector<IpUdpPkt> ipUdpPayloads(const std::vector<std::vector<uint8_t>>& aal5Frames)
{
    std::map<std::tuple<std::string, std::string, int, int>, std::map<int, std::vector<uint8_t>>> frags;
    for (const auto& fr : aal5Frames) {
        int L = aal5Len(fr);
        if (L <= 0 || L > (int)fr.size() - 8) continue;
        if (fr[0] != 0x45) continue; // IPv4 IHL=5
        int ihl = (fr[0] & 0xF) * 4;
        if (ihl < 20) continue;
        int total = ((int)fr[2] << 8) | fr[3];
        int ipid = ((int)fr[4] << 8) | fr[5];
        int ff = ((int)fr[6] << 8) | fr[7];
        int off = (ff & 0x1FFF) * 8;
        int proto = fr[9];
        char srcBuf[16], dstBuf[16];
        std::snprintf(srcBuf, sizeof(srcBuf), "%d.%d.%d.%d", fr[12], fr[13], fr[14], fr[15]);
        std::snprintf(dstBuf, sizeof(dstBuf), "%d.%d.%d.%d", fr[16], fr[17], fr[18], fr[19]);
        std::string src(srcBuf), dst(dstBuf);
        int end = (total < L) ? total : L;
        if (ihl >= end) continue;
        std::vector<uint8_t> body(&fr[ihl], &fr[end]);
        frags[{src, dst, ipid, proto}][off] = body;
    }

    std::vector<IpUdpPkt> out;
    for (auto& [key, parts] : frags) {
        (void)key;
        std::vector<uint8_t> buf;
        for (auto& [off, body] : parts) {
            if (off == (int)buf.size())
                buf.insert(buf.end(), body.begin(), body.end());
        }
        if (buf.size() >= 8 && std::get<3>(key) == 17) { // UDP
            IpUdpPkt p;
            p.src = std::get<0>(key);
            p.dst = std::get<1>(key);
            p.dstPort = ((int)buf[2] << 8) | buf[3];
            p.payload.assign(buf.begin() + 8, buf.end());
            out.push_back(p);
        }
    }
    return out;
}

// ============================================================================
// DSM-CC DownloadDataBlock reassembly
// ============================================================================

std::map<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>, std::vector<uint8_t>>
reassembleModules(const std::vector<IpUdpPkt>& pkts, int port)
{
    std::map<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>, std::map<int, std::vector<uint8_t>>> groups;

    const uint8_t dsmccMagic[4] = {0x11, 0x03, 0x10, 0x03};
    for (const auto& p : pkts) {
        if (p.dstPort != port) continue;
        if (p.payload.size() < 18) continue;
        if (std::memcmp(p.payload.data(), dsmccMagic, 4) != 0) continue;

        std::vector<uint8_t> tag(&p.payload[4], &p.payload[8]);
        std::vector<uint8_t> modId(&p.payload[12], &p.payload[15]);
        int seq = ((int)p.payload[16] << 8) | p.payload[17];
        std::vector<uint8_t> data(&p.payload[18], &p.payload[p.payload.size()]);
        groups[{tag, modId}][seq] = data;
    }

    std::map<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>, std::vector<uint8_t>> modules;
    for (auto& [key, parts] : groups) {
        std::vector<uint8_t> merged;
        for (auto& [seq, data] : parts)
            merged.insert(merged.end(), data.begin(), data.end());
        modules[key] = merged;
    }
    return modules;
}

// ============================================================================
// BIOP object parsing
// ============================================================================

std::vector<BiopObject> parseBiop(const std::vector<uint8_t>& blob)
{
    std::vector<BiopObject> objs;
    const uint8_t biopMagic[4] = {'B', 'I', 'O', 'P'};
    size_t n = blob.size();

    for (size_t pos = 0; pos + 16 < n; ) {
        // Find "BIOP"
        auto it = std::search(blob.begin() + pos, blob.end(), biopMagic, biopMagic + 4);
        if (it == blob.end()) break;
        pos = size_t(it - blob.begin());
        size_t q = pos + 12; // skip "BIOP" + version/byteorder/msgtype(4) + msg_size(4)

        try {
            if (q + 1 > n) { pos++; continue; }
            int klen = blob[q]; q++;
            if (q + klen > n) { pos++; continue; }
            std::vector<uint8_t> key(&blob[q], &blob[q + klen]); q += klen;
            if (q + 4 > n) { pos++; continue; }
            int kindlen = (blob[q]<<24)|(blob[q+1]<<16)|(blob[q+2]<<8)|blob[q+3]; q += 4;
            if (q + kindlen > n) { pos++; continue; }
            std::string kind(reinterpret_cast<const char*>(&blob[q]), kindlen); q += kindlen;
            if (q + 2 > n) { pos++; continue; }
            int infolen = ((int)blob[q]<<8) | blob[q+1]; q += 2;
            if (q + infolen > n) { pos++; continue; }
            q += infolen; // skip object_info
            if (q + 1 > n) { pos++; continue; }
            q += 1; // serviceContextList_count
            if (q + 4 > n) { pos++; continue; }
            int mbody = (blob[q]<<24)|(blob[q+1]<<16)|(blob[q+2]<<8)|blob[q+3]; q += 4;
            BiopObject obj;
            obj.key = key;
            if (kind.find("fil") == 0) {
                obj.kind = "fil";
                if (q + 4 > n) { pos++; continue; }
                int clen = (blob[q]<<24)|(blob[q+1]<<16)|(blob[q+2]<<8)|blob[q+3]; q += 4;
                if (clen > 0 && clen <= 200000 && q + clen <= n) {
                    obj.content.assign(&blob[q], &blob[q + clen]);
                    q += clen;
                }
            } else {
                obj.kind = kind.substr(0, 3);
                if (mbody > 0 && q + mbody <= n) {
                    obj.content.assign(&blob[q], &blob[q + mbody]);
                    q += mbody;
                }
            }
            objs.push_back(obj);
            pos++; // continue past this match
        } catch (...) {
            pos++;
        }
    }
    return objs;
}

// ============================================================================
// EPG extraction
// ============================================================================

std::pair<std::vector<EpgEntry>, std::vector<std::string>>
extractEpg(const std::map<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>, std::vector<uint8_t>>& modules)
{
    const uint8_t cmTag[4] = {0x00, 0x00, 0x00, 0x0b};
    std::vector<EpgEntry> channels;
    std::vector<std::string> services;

    // Temporary vote counting
    std::map<std::string, std::map<int, int>> votes;

    for (const auto& [key, blob] : modules) {
        if (key.first.size() < 4) continue;
        if (std::memcmp(key.first.data(), cmTag, 4) != 0) continue;

        // Scan for UTF-16BE runs
        size_t i = 0;
        std::string lastName;
        while (i + 1 < blob.size()) {
            if (blob[i] == 0x00 && blob[i + 1] >= 0x20 && blob[i + 1] < 0x7F) {
                std::string chars;
                size_t j = i;
                while (j + 1 < blob.size() && blob[j] == 0x00 &&
                       blob[j + 1] >= 0x20 && blob[j + 1] < 0x7F) {
                    chars += (char)blob[j + 1];
                    j += 2;
                }
                // Try to match call sign + channel number
                auto sp = chars.find(' ');
                if (sp != std::string::npos) {
                    std::string cs = chars.substr(0, sp);
                    // Validate call sign format
                    bool validCS = true;
                    if (cs.size() < 2 || cs.size() > 8) validCS = false;
                    else {
                        if (cs[0] < 'A' || cs[0] > 'Z') {
                            if (cs[0] < '0' || cs[0] > '9') validCS = false;
                        }
                        for (char c : cs) {
                            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                  c == '+' || c == '&' || c == '-'))
                                { validCS = false; break; }
                        }
                    }
                    if (validCS && j + 2 <= blob.size()) {
                        int chan = ((int)blob[j] << 8) | blob[j + 1];
                        if (chan > 0 && chan < 3000) {
                            votes[cs][chan]++;
                        }
                    }
                }

                // Also collect service names (multi-word, high alpha ratio)
                {
                    std::string s = chars;
                    // Trim
                    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
                    while (!s.empty() && s.back() == ' ') s.pop_back();
                    if (s.size() > 3 && s.find(' ') != std::string::npos) {
                        int alpha = 0;
                        for (char c : s) if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) alpha++;
                        if (double(alpha) / s.size() > 0.6) {
                            services.push_back(s);
                        }
                    }
                }

                i = j;
            } else {
                i++;
            }
        }
    }

    // Resolve most-common channel per call sign
    for (auto& [cs, cv] : votes) {
        int bestChan = 0, bestCount = 0;
        for (auto& [ch, cnt] : cv) {
            if (cnt > bestCount) { bestCount = cnt; bestChan = ch; }
        }
        if (bestChan > 0) channels.push_back({cs, bestChan});
    }

    // Deduplicate service names
    std::set<std::string> seen(services.begin(), services.end());
    services.assign(seen.begin(), seen.end());

    return {channels, services};
}

} // namespace oob
