// dsp.h - minimal, dependency-free DSP primitives for a guitar amp sim.
//
// Design constraints, in priority order:
//   1. zero or near-zero latency in the dry path
//   2. no allocation, no locks, no exceptions inside process()
//   3. denormal-safe (a reverb tail decaying to 1e-310 will destroy a CPU)
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace dsp {

constexpr double kPi = 3.14159265358979323846;

// Anything this small is denormal territory on x86 and ARM alike.
inline double dn(double v) { return (std::fabs(v) < 1e-18) ? 0.0 : v; }

inline double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

// ---------------------------------------------------------------- smoothed

// One-pole parameter ramp. The coefficient is per-sample, so a block-based
// caller must advance it once per sample (nextN) or the ramp becomes 64x too
// slow at a 64-sample block and the first notes of every session are muted.
class Smoothed {
public:
    void setSampleRate(double sr, double ms = 10.0) {
        a_ = 1.0 - std::exp(-1.0 / (0.001 * ms * sr));
    }
    void snap(double v) { target_ = v; current_ = v; primed_ = true; }
    void setTarget(double v) {
        target_ = v;
        // first assignment is a snap: a cold start should not fade in from zero
        if (!primed_) { current_ = v; primed_ = true; }
    }
    double next() {
        current_ += a_ * (target_ - current_);
        if (std::fabs(target_ - current_) < 1e-9) current_ = target_;
        return current_;
    }
    double nextN(int n) {
        for (int i = 0; i < n; ++i) next();
        return current_;
    }
    double value() const { return current_; }

private:
    double a_ = 0.01, target_ = 0.0, current_ = 0.0;
    bool primed_ = false;
};

// ----------------------------------------------------------------- biquads

// RBJ cookbook, transposed direct form II.
class Biquad {
public:
    void reset() { z1_ = z2_ = 0.0; }

    void setLowpass(double sr, double f, double q) {
        double w = 2 * kPi * clampd(f, 10.0, sr * 0.49) / sr;
        double cw = std::cos(w), alpha = std::sin(w) / (2 * q);
        double a0 = 1 + alpha;
        set((1 - cw) * 0.5 / a0, (1 - cw) / a0, (1 - cw) * 0.5 / a0,
            -2 * cw / a0, (1 - alpha) / a0);
    }

    void setHighpass(double sr, double f, double q) {
        double w = 2 * kPi * clampd(f, 5.0, sr * 0.49) / sr;
        double cw = std::cos(w), alpha = std::sin(w) / (2 * q);
        double a0 = 1 + alpha;
        set((1 + cw) * 0.5 / a0, -(1 + cw) / a0, (1 + cw) * 0.5 / a0,
            -2 * cw / a0, (1 - alpha) / a0);
    }

    void setPeaking(double sr, double f, double q, double gainDb) {
        double A = std::pow(10.0, gainDb / 40.0);
        double w = 2 * kPi * clampd(f, 10.0, sr * 0.49) / sr;
        double cw = std::cos(w), sw = std::sin(w);
        double alpha = sw / (2 * q);
        double a0 = 1 + alpha / A;
        set((1 + alpha * A) / a0, (-2 * cw) / a0, (1 - alpha * A) / a0,
            (-2 * cw) / a0, (1 - alpha / A) / a0);
    }

    void setLowShelf(double sr, double f, double q, double gainDb) {
        double A = std::pow(10.0, gainDb / 40.0);
        double w = 2 * kPi * clampd(f, 10.0, sr * 0.49) / sr;
        double cw = std::cos(w), sw = std::sin(w);
        double alpha = sw / (2 * q);
        double beta = 2 * std::sqrt(A) * alpha;
        double a0 = (A + 1) + (A - 1) * cw + beta;
        set(A * ((A + 1) - (A - 1) * cw + beta) / a0,
            2 * A * ((A - 1) - (A + 1) * cw) / a0,
            A * ((A + 1) - (A - 1) * cw - beta) / a0,
            -2 * ((A - 1) + (A + 1) * cw) / a0,
            ((A + 1) + (A - 1) * cw - beta) / a0);
    }

    void setHighShelf(double sr, double f, double q, double gainDb) {
        double A = std::pow(10.0, gainDb / 40.0);
        double w = 2 * kPi * clampd(f, 10.0, sr * 0.49) / sr;
        double cw = std::cos(w), sw = std::sin(w);
        double alpha = sw / (2 * q);
        double beta = 2 * std::sqrt(A) * alpha;
        double a0 = (A + 1) - (A - 1) * cw + beta;
        set(A * ((A + 1) + (A - 1) * cw + beta) / a0,
            -2 * A * ((A - 1) + (A + 1) * cw) / a0,
            A * ((A + 1) + (A - 1) * cw - beta) / a0,
            2 * ((A - 1) - (A + 1) * cw) / a0,
            ((A + 1) - (A - 1) * cw - beta) / a0);
    }

    inline double process(double x) {
        double y = b0_ * x + z1_;
        z1_ = dn(b1_ * x - a1_ * y + z2_);
        z2_ = dn(b2_ * x - a2_ * y);
        return y;
    }

private:
    void set(double b0, double b1, double b2, double a1, double a2) {
        b0_ = b0; b1_ = b1; b2_ = b2; a1_ = a1; a2_ = a2;
    }
    double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    double z1_ = 0, z2_ = 0;
};

// ------------------------------------------------------------- one-poles

class OnePoleLP {
public:
    void set(double fc, double sr) { a_ = 1.0 - std::exp(-2 * kPi * fc / sr); }
    inline double process(double x) { y_ = dn(y_ + a_ * (x - y_)); return y_; }
    void reset() { y_ = 0; }
private:
    double a_ = 0.1, y_ = 0;
};

class OnePoleHP {
public:
    void set(double fc, double sr) { a_ = 1.0 / (1.0 + 2 * kPi * fc / sr); }
    inline double process(double x) {
        double y = a_ * (y_ + x - x1_);
        x1_ = x; y_ = dn(y);
        return y;
    }
    void reset() { x1_ = y_ = 0; }
private:
    double a_ = 0.99, x1_ = 0, y_ = 0;
};

// ------------------------------------------------------------ delay lines

// Fractional delay with linear interpolation. Linear is enough for modulated
// reverb/delay taps; a chorus that needs better would want allpass interpolation.
class DelayLine {
public:
    void init(int maxSamples) {
        buf_.assign(static_cast<size_t>(std::max(4, maxSamples)), 0.0);
        w_ = 0;
    }
    void clear() { std::fill(buf_.begin(), buf_.end(), 0.0); w_ = 0; }

    inline void write(double x) {
        buf_[w_] = dn(x);
        if (++w_ >= buf_.size()) w_ = 0;
    }

    inline double readInt(int d) const {
        int i = static_cast<int>(w_) - d;
        while (i < 0) i += static_cast<int>(buf_.size());
        return buf_[static_cast<size_t>(i)];
    }

    inline double readFrac(double d) const {
        double d0 = std::floor(d);
        double frac = d - d0;
        int i0 = static_cast<int>(w_) - static_cast<int>(d0);
        int i1 = i0 - 1;
        while (i0 < 0) i0 += static_cast<int>(buf_.size());
        while (i1 < 0) i1 += static_cast<int>(buf_.size());
        return buf_[static_cast<size_t>(i0)] * (1 - frac) + buf_[static_cast<size_t>(i1)] * frac;
    }

    size_t size() const { return buf_.size(); }

private:
    std::vector<double> buf_;
    size_t w_ = 0;
};

// -------------------------------------------------------------- allpass

// Schroeder allpass. Used in series to fake spring dispersion.
class Allpass {
public:
    void init(int delay) {
        line_.init(delay + 2);
        d_ = delay;
        g_ = 0.6;
    }
    void setGain(double g) { g_ = clampd(g, -0.95, 0.95); }
    void setDelay(int d) { d_ = std::max(1, d); }
    inline double process(double x) {
        double delayed = line_.readInt(d_);
        double v = x + g_ * delayed;
        line_.write(v);
        return delayed - g_ * v;
    }
    void clear() { line_.clear(); }
private:
    DelayLine line_;
    int d_ = 1;
    double g_ = 0.6;
};

// ---------------------------------------------------------------- FIR

// Direct-form FIR, DC-normalised. 129 taps at 48 kHz is ~2.7 ms and costs
// nothing on a modern core; above ~1024 taps switch to partitioned FFT.
class Fir {
public:
    // normalizeDc is correct for interpolation/decimation filters, which are
    // designed for unity DC gain. It is wrong for a cab IR: summing a resonant
    // IR can land near zero, which either explodes the taps or buries the output
    // ~20 dB down. Cab IRs get their own (energy) normalisation.
    void setTaps(std::vector<double> taps, bool normalizeDc = true) {
        h_ = taps.empty() ? std::vector<double>{1.0} : std::move(taps);
        z_.assign(h_.size(), 0.0);
        idx_ = 0;
        if (normalizeDc) {
            double s = 0;
            for (double v : h_) s += v;
            if (std::fabs(s) > 1e-12)
                for (auto& v : h_) v /= s;
        }
    }
    void reset() { std::fill(z_.begin(), z_.end(), 0.0); idx_ = 0; }

    inline double process(double x) {
        z_[idx_] = x;
        double y = 0;
        size_t j = idx_;
        for (size_t i = 0; i < h_.size(); ++i) {
            y += h_[i] * z_[j];
            j = (j == 0) ? h_.size() - 1 : j - 1;
        }
        idx_ = (idx_ + 1 == h_.size()) ? 0 : idx_ + 1;
        return dn(y);
    }

    size_t taps() const { return h_.size(); }

private:
    std::vector<double> h_, z_;
    size_t idx_ = 0;
};

// Windowed-sinc lowpass, Blackman. fc is normalised to the SAMPLE RATE, not to
// Nyquist: the corner lands at fc * sr, so a 19.2 kHz cutoff at 192 kHz is
// fc = 0.1. Getting this wrong by 2x puts the reconstruction filter above the
// base Nyquist and passes aliases straight through.
inline std::vector<double> designLowpass(int taps, double fc) {
    if (taps % 2 == 0) ++taps;
    std::vector<double> h(static_cast<size_t>(taps));
    int m = (taps - 1) / 2;
    fc = clampd(fc, 0.001, 0.499);
    for (int i = 0; i < taps; ++i) {
        int n = i - m;
        double s = (n == 0) ? 2 * fc : std::sin(2 * kPi * fc * n) / (kPi * n);
        double w = 0.42 - 0.5 * std::cos(2 * kPi * i / (taps - 1)) +
                   0.08 * std::cos(4 * kPi * i / (taps - 1));
        h[static_cast<size_t>(i)] = s * w;
    }
    return h;
}

// ------------------------------------------------------------ oversampling

// Zero-stuff + FIR at L-times rate, nonlinearity at L-times rate, FIR + decimate.
//
// Chosen over a polyphase halfband cascade because it is short enough to audit.
// Cost is real (~32*L taps per direction per base sample), so L is a switch, not
// a constant: 4x for the amp, 2x in Low Latency Mode.
class Oversampler {
public:
    void init(int factor) {
        factor_ = factor;
        if (factor <= 1) { up_.setTaps({1.0}); dn_.setTaps({1.0}); high_.clear(); return; }
        int taps = 32 * factor + 1;
        // 19.2 kHz corner at any rate. As a fraction of the base rate that is
        // 19.2/48 = 0.4, and as a fraction of the high rate it is 0.4/factor.
        double fcNorm = 0.40 / factor;
        auto h = designLowpass(taps, fcNorm);
        up_.setTaps(h);
        dn_.setTaps(h);
    }

    int factor() const { return factor_; }

    // Added latency of the up + decimate pair, in base-rate samples.
    double delayBaseSamples() const {
        if (factor_ <= 1) return 0.0;
        return static_cast<double>(up_.taps() - 1) / factor_;
    }

    // Zero-stuff and filter. Returns the internal high-rate buffer, length n*factor.
    double* upsample(const double* in, int n) {
        int L = factor_;
        if (L <= 1) {
            high_.resize(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i) high_[static_cast<size_t>(i)] = in[i];
            return high_.data();
        }
        high_.resize(static_cast<size_t>(n) * L);
        for (int i = 0; i < n; ++i) {
            for (int k = 0; k < L; ++k) {
                // gain L compensates for the energy lost to zero-stuffing
                double x = (k == 0) ? in[i] * L : 0.0;
                high_[static_cast<size_t>(i) * L + k] = up_.process(x);
            }
        }
        return high_.data();
    }

    // Run the filter across every high-rate sample, keep one in every L.
    void downsample(double* out, int n) {
        int L = factor_;
        if (L <= 1) {
            for (int i = 0; i < n; ++i) out[i] = high_[static_cast<size_t>(i)];
            return;
        }
        for (int i = 0; i < n; ++i) {
            double y = 0.0;
            for (int k = 0; k < L; ++k) y = dn_.process(high_[static_cast<size_t>(i) * L + k]);
            out[i] = y;
        }
    }

    void reset() {
        up_.reset();
        dn_.reset();
        std::fill(high_.begin(), high_.end(), 0.0);
    }

private:
    int factor_ = 1;
    Fir up_, dn_;
    std::vector<double> high_;
};

}  // namespace dsp
