// =============================================================================
// SuVtitle - engine/TrDecimator.cpp   (AudioPB 50f79d6's translator/TrAudio.cpp, the Decimator section, verbatim)
// =============================================================================
#include "TrDecimator.h"

#include <algorithm>
#include <cmath>

namespace pbt {

void Decimator::init(uint32_t inRate) {
    inRate_ = inRate ? inRate : (uint32_t)kAsrRate;
    step_   = (double)inRate_ / (double)kAsrRate;

    // Cutoff, expressed as a fraction of the INPUT rate. 6800 Hz keeps every formant
    // that matters for speech and leaves a real transition band below the 8 kHz output
    // Nyquist, so nothing above it folds back onto the voice (measured: see TrDecimator.h).
    const double fcNorm = kCutoffFrac * (kAsrRate * 0.5) / (double)inRate_;
    const double twoFc  = 2.0 * fcNorm;

    // sinc(twoFc * x) crosses zero every 1/twoFc input samples; kZeroCrossings lobes
    // each side is the sharpness knob. At 96 k that is ~226 taps a side -- ~7 MMAC/s,
    // which is nothing next to what the GPU behind this is about to do.
    halfTaps_ = (int)std::ceil((double)kZeroCrossings / twoFc);
    if (halfTaps_ < 4)   halfTaps_ = 4;
    if (halfTaps_ > 512) halfTaps_ = 512;   // a rate we do not support would blow the table

    const int n = halfTaps_ * kOversample + 2;
    table_.assign((size_t)n, 0.0f);
    const double pi = 3.14159265358979323846;
    for (int i = 0; i < n; ++i) {
        const double x = (double)i / (double)kOversample;         // input samples from centre
        const double a = twoFc * x;
        const double s = (i == 0) ? 1.0 : std::sin(pi * a) / (pi * a);
        // Blackman, folded to the half-kernel: u = x / (2*halfTaps) in [0, 0.5].
        const double u = x / (2.0 * (double)halfTaps_);
        const double w = (x >= (double)halfTaps_)
                       ? 0.0
                       : 0.42 + 0.5 * std::cos(2.0 * pi * u) + 0.08 * std::cos(4.0 * pi * u);
        table_[(size_t)i] = (float)(s * w);
    }
    reset();
}

void Decimator::reset() {
    hist_.assign((size_t)std::max(1, halfTaps_), 0.0f);   // zero-padded start = silence before us
    pos_ = (double)halfTaps_;
}

void Decimator::process(const float* in, int n, std::vector<float>& out) {
    if (!inRate_ || n <= 0) return;
    hist_.insert(hist_.end(), in, in + n);

    const int    ht    = halfTaps_;
    const double dens  = (double)kOversample;
    const int    tblN  = (int)table_.size();
    while ((int)pos_ + ht < (int)hist_.size()) {
        const int    i0   = (int)pos_;
        const double frac = pos_ - (double)i0;
        double acc = 0.0, wsum = 0.0;
        for (int t = -ht + 1; t <= ht; ++t) {
            const double x  = std::fabs((double)t - frac);
            const double fi = x * dens;
            const int    j  = (int)fi;
            if (j + 1 >= tblN) continue;                 // outside the kernel support
            const double ff = fi - (double)j;
            const double h  = (double)table_[(size_t)j] * (1.0 - ff)
                            + (double)table_[(size_t)(j + 1)] * ff;
            acc  += h * (double)hist_[(size_t)(i0 + t)];
            wsum += h;
        }
        // Normalise by the taps actually used: unity DC gain at EVERY fractional phase,
        // with no window-energy constant to get subtly wrong.
        out.push_back(wsum != 0.0 ? (float)(acc / wsum) : 0.0f);
        pos_ += step_;
    }

    // Retire history the kernel can no longer reach.
    const int keep = (int)pos_ - ht + 1;
    if (keep > 0) {
        hist_.erase(hist_.begin(), hist_.begin() + keep);
        pos_ -= (double)keep;
    }
}

} // namespace pbt
