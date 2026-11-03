// =============================================================================
// SuVtitle - engine/TrDecimator.h   (any capture rate -> the 16 kHz every ASR model wants)
//
// SuVtitle 1.1.0 (2026-10-25): the Decimator of AudioPB's translator/TrAudio.h at commit 50f79d6 (AudioPB
// Translator 1.1.0), moved VERBATIM into its own file because two parts of the live path now need it: the engine
// core (TrAudio, whose selftest path decimates a bench file) and the capture hub (one instance per live input, in the
// source thread). kAsrRate came with it -- it was TrAudio.h's "one definition of 16 kHz, shared with the front end"
// before the split parked it in TrEngine.h, which now includes this file for it.
// OWNED BY THE DIRECTOR (docs/LIVE_PLAN.md): both live agents read it, neither edits it.
// =============================================================================
#pragma once

#include <cstdint>
#include <vector>

namespace pbt {

// Every ASR model we drive wants exactly this.
constexpr int kAsrRate = 16000;

// ---------------------------------------------------------------------------
// Windowed-sinc decimator, any input rate -> 16 kHz.
//
// A capture device runs at 44.1 / 48 / 88.2 / 96 k (and more). Two of those are integer ratios
// to 16 k and two are NOT (44100/16000 = 2.75625), so a "take every Nth sample"
// decimator is not an option -- and dropping samples without a filter would fold
// everything above 8 kHz onto the speech we are trying to recognise. This is the
// textbook answer: one anti-alias kernel, evaluated at a fractional position.
//
// The kernel is a Blackman-windowed sinc, cut at 6800 Hz (0.85 x the 8 kHz output
// Nyquist), tabulated once at kOversample points per input sample and linearly
// interpolated between entries. Each output normalises by the sum of the taps it
// actually used, so DC gain is exactly 1 at every fractional phase -- no window
// scaling to get subtly wrong.
//
// The two numbers below are the sharpness/cost trade. 32 lobes puts the transition
// band comfortably inside 8 kHz at every rate we support, and 0.85 keeps the corner
// (6800 Hz at 16 k out) below anything speech or whisper's own front end cares about.
// The selftest measures both ends of the bargain: 1 kHz reproduced to 0.01 %, and a
// 9 kHz tone -- which a naive decimator would fold onto 7 kHz at full level -- gone.
//
// /!\ INPUT RATES BELOW 16 kHz ARE NOT ITS JOB. The cutoff is drawn from the OUTPUT Nyquist, so at 8 kHz in (a
// Bluetooth headset's narrow-band microphone) the "decimator" would be an upsampler with a kernel wider than the
// input band. The capture asks Windows for 16 kHz instead when a device's mix format is below it
// (SUVTITLE_LIVE_PLAN section 4.4); init() of such a rate is a caller bug.
// ---------------------------------------------------------------------------
class Decimator {
public:
    // inRate must be > 0. Safe to call again to re-arm at a new rate (a device's
    // format changed); it clears the history.
    void init(uint32_t inRate);
    void reset();

    uint32_t inRate() const { return inRate_; }

    // Push `n` input-rate mono frames; appends whatever 16 kHz frames come out.
    void process(const float* in, int n, std::vector<float>& out);

private:
    uint32_t            inRate_    = 0;
    int                 halfTaps_  = 0;    // kernel support, in input samples, each side
    double              step_      = 1.0;  // input samples advanced per output sample
    double              pos_       = 0.0;  // fractional read position inside hist_
    std::vector<float>  table_;            // h(x) sampled at kOversample points / input sample
    std::vector<float>  hist_;             // input history (>= 2*halfTaps_ + a block)

    static constexpr int kZeroCrossings = 32;    // kernel sharpness: lobes each side
    static constexpr int kOversample    = 64;    // table density, points per input sample
    static constexpr double kCutoffFrac = 0.85;  // corner, as a fraction of the OUTPUT Nyquist
};

} // namespace pbt
