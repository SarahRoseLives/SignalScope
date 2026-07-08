// AM / narrowband-FM demodulator for the audio path.
// Input : complex baseband IQ centred on the station (interleaved double), at
//         the channel DDC rate (48 kHz IF for AM/NFM).
// Output: mono float audio at the same rate (no decimation; the DDC already
//         narrows the channel), DC-blocked and soft-clipped.
//
// AM  : envelope detector (magnitude) -> DC block -> AGC-ish normalise.
// NFM : quadrature (polar) discriminator scaled for ~5 kHz deviation.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class AmNfmDemod
{
public:
    enum Mode { AM, NFM };

    // rate: complex baseband sample rate (Hz) = audio output rate.
    // deviationHz: peak FM deviation used to scale NFM (ignored for AM).
    void configure(Mode mode, double rate, double deviationHz = 5000.0)
    {
        mode_ = mode;
        rate_ = rate;
        prev_ = std::complex<double>(0.0, 0.0);
        dcPrev_ = 0.0;
        dcOut_ = 0.0;
        env_ = 0.0;
        agc_ = 1.0;
        // NFM: rad/sample -> unity at peak deviation.
        fmGain_ = rate_ / (2.0 * M_PI * deviationHz);
    }

    double audioRate() const { return rate_; }

    // Process nComplex interleaved double IQ samples; append mono float audio.
    void process(const double* iq, int nComplex, std::vector<float>& out)
    {
        for (int i = 0; i < nComplex; ++i)
        {
            std::complex<double> s(iq[i * 2], iq[i * 2 + 1]);
            double a;

            if (mode_ == AM)
            {
                // Envelope = |IQ|. Track a slow average for AGC/carrier removal.
                double mag = std::abs(s);
                agc_ = 0.9995 * agc_ + 0.0005 * mag; // slow carrier-level estimate
                double norm = (agc_ > 1e-9) ? (mag / agc_) - 1.0 : 0.0;
                a = norm;
            }
            else // NFM
            {
                std::complex<double> d = s * std::conj(prev_);
                prev_ = s;
                a = std::atan2(d.imag(), d.real()) * fmGain_;
            }

            // DC / subsonic block (one-pole high-pass).
            dcOut_ = a - dcPrev_ + 0.9995 * dcOut_;
            dcPrev_ = a;
            double y = std::clamp(dcOut_, -1.5, 1.5);
            out.push_back((float)y);
        }
    }

private:
    Mode mode_ = AM;
    double rate_ = 48000.0;
    std::complex<double> prev_{0.0, 0.0};
    double dcPrev_ = 0.0, dcOut_ = 0.0;
    double env_ = 0.0;
    double agc_ = 1.0;
    double fmGain_ = 1.0;
};
