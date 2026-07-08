// Wideband FM demodulator (broadcast, mono).
// Input : complex baseband IQ centred on the station (interleaved double), at
//         whatever rate the channel DDC produces (~200-250 kHz).
// Output: mono float audio, decimated to ~48 kHz, 75 us de-emphasis applied.
//
// Chain: quadrature (polar) discriminator -> anti-alias FIR + integer decimate
//        -> 75 us de-emphasis IIR -> DC/subsonic block -> soft clip.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class WfmDemod
{
public:
    // inputRate: complex baseband sample rate (Hz). audioTarget: desired audio
    // rate; the actual audioRate() is inputRate/decim where decim is an integer.
    void configure(double inputRate, double audioTarget = 48000.0)
    {
        inputRate_ = inputRate;
        decim_ = (int)std::lround(inputRate / audioTarget);
        if (decim_ < 1)
            decim_ = 1;
        audioRate_ = inputRate_ / decim_;

        // Anti-alias low-pass on the discriminator output before decimation.
        double cutoff = std::min(15000.0, audioRate_ * 0.45); // keep FM audio band
        int taps = 8 * decim_ + 33;
        if ((taps & 1) == 0)
            ++taps; // odd length -> symmetric linear phase
        h_.resize(taps);
        double fc = cutoff / inputRate_; // normalized (0..0.5)
        double sum = 0.0;
        int M = taps - 1;
        for (int n = 0; n < taps; ++n)
        {
            double x = n - M / 2.0;
            double sinc = (x == 0.0) ? 2.0 * fc : std::sin(2.0 * M_PI * fc * x) / (M_PI * x);
            double w = 0.54 - 0.46 * std::cos(2.0 * M_PI * n / M); // Hamming
            h_[n] = sinc * w;
            sum += h_[n];
        }
        for (double& v : h_)
            v /= sum; // unity DC gain

        delay_.assign(taps, 0.0);
        dpos_ = 0;
        dcount_ = 0;

        // 75 us de-emphasis (North America / Korea). One-pole low-pass.
        deemphA_ = 1.0 - std::exp(-1.0 / (audioRate_ * 75e-6));
        deemphPrev_ = 0.0;
        dcPrev_ = 0.0;
        dcOut_ = 0.0;
        prev_ = std::complex<double>(0.0, 0.0);

        // Normalize so full broadcast deviation (~75 kHz) maps to ~unity.
        gain_ = inputRate_ / (2.0 * M_PI * 75000.0);
    }

    double audioRate() const { return audioRate_; }

    // Process nComplex interleaved double IQ samples; append mono float audio.
    void process(const double* iq, int nComplex, std::vector<float>& out)
    {
        const int L = (int)h_.size();
        for (int i = 0; i < nComplex; ++i)
        {
            std::complex<double> s(iq[i * 2], iq[i * 2 + 1]);
            std::complex<double> d = s * std::conj(prev_);
            prev_ = s;
            double demod = std::atan2(d.imag(), d.real()); // rad/sample ~ instantaneous freq

            delay_[dpos_] = demod;
            if (++dpos_ >= L)
                dpos_ = 0;
            if (++dcount_ < decim_)
                continue;
            dcount_ = 0;

            double acc = 0.0;
            int idx = dpos_ - 1;
            if (idx < 0)
                idx += L;
            for (int k = 0; k < L; ++k)
            {
                int di = idx - k;
                if (di < 0)
                    di += L;
                acc += delay_[di] * h_[k];
            }

            double a = acc * gain_;
            deemphPrev_ += deemphA_ * (a - deemphPrev_); // de-emphasis
            double de = deemphPrev_;
            dcOut_ = de - dcPrev_ + 0.9995 * dcOut_; // DC / subsonic block
            dcPrev_ = de;
            double y = dcOut_;
            y = std::clamp(y, -1.5, 1.5); // soft ceiling before the sink's volume
            out.push_back((float)y);
        }
    }

private:
    double inputRate_ = 0.0, audioRate_ = 0.0;
    int decim_ = 1;

    std::complex<double> prev_{0.0, 0.0};

    std::vector<double> h_;
    std::vector<double> delay_;
    int dpos_ = 0, dcount_ = 0;

    double deemphA_ = 0.0, deemphPrev_ = 0.0;
    double dcPrev_ = 0.0, dcOut_ = 0.0;
    double gain_ = 1.0;
};
