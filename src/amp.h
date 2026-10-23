// amp.h - guitar preamp voicings, cabinet, spring/plate reverb, tape delay.
//
// Honest note on the three voicings: Fender and Mesa are spring-reverb amps.
// Marshall heads historically shipped with no reverb at all, and none of the
// three has built-in delay - that is tape echo or a pedal. So "reverb and delay
// like a Marshall" is not a thing that exists; the preset here is Marshall
// preamp + no reverb, which is what the amps actually do.
#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "dsp.h"

namespace amp {

enum class Voice { Fender = 0, Marshall = 1, Mesa = 2 };

struct Params {
    Voice voice = Voice::Fender;
    double gain = 0.35;          // 0..1, mapped to dB internally
    double bass = 4.0;           // dB
    double mid = -3.5;           // dB
    double treble = 3.0;         // dB
    double presence = 0.0;       // dB
    int oversample = 4;          // 1, 2 or 4

    int reverbType = 1;          // 0 off, 1 spring, 2 plate
    double reverbMix = 0.30;
    double reverbDecay = 0.70;

    double delayMs = 0.0;        // 0 = off
    double delayFeedback = 0.30;
    double delayMix = 0.0;

    double outputTrim = 1.0;
};

inline double softclip(double x) {
    // transparent below 0.95, so clean tones stay clean
    if (x > 0.95) return 0.95 + 0.05 * std::tanh((x - 0.95) / 0.05);
    if (x < -0.95) return -0.95 + 0.05 * std::tanh((x + 0.95) / 0.05);
    return x;
}

// ---------------------------------------------------------------- shapers

inline double shapeStage(double x, Voice v, double drive, bool second) {
    double u = x * drive;
    switch (v) {
        case Voice::Fender: {
            // soft, slightly asymmetric -> even harmonics, hence the DC blocker
            const double bias = 0.10;
            return std::tanh(u + bias) - std::tanh(bias);
        }
        case Voice::Marshall: {
            // harder knee: algebraic soft clip has a sharper transition than tanh
            if (!second) {
                double t = u * 1.4;
                return t / std::sqrt(1.0 + t * t);
            }
            return std::tanh(u * 1.9);
        }
        case Voice::Mesa: {
            // cascaded: stage one compresses, stage two saturates
            double a = std::tanh(u * 1.3);
            return second ? std::tanh(a * 2.0) : a;
        }
    }
    return x;
}

// ------------------------------------------------------------------ preamp

class Preamp {
public:
    void prepare(double sr) {
        sr_ = sr;
        inHp_.setHighpass(sr, 80.0, 0.7);
        // Let setOversample own initialisation: it also sets the interstage filter
        // for the new rate. Calling os_.init() first would make setOversample a
        // no-op and leave interHp_ at its default coefficient.
        setOversample(4);
        dc_.set(20.0, sr);
        os_.reset();
        inHp_.reset();
        dc_.reset();
        interHp_.reset();
        smooth_.setSampleRate(sr, 8.0);
        toneDirty_ = true;
    }

    void setOversample(int f) {
        if (f == os_.factor()) return;
        os_.init(f);
        updateInterStage();
    }

    void setParams(const Params& p) {
        p_ = p;
        setOversample(p.oversample <= 1 ? 1 : (p.oversample >= 4 ? 4 : 2));
        smooth_.setTarget(targetDrive());
        toneDirty_ = true;
    }

    // Tonal filters run at base rate: linear stages cannot alias, so there is no
    // reason to pay 4x for them.
    void updateTone() {
        lowShelf_.setLowShelf(sr_, 120.0, 0.7, p_.bass);
        mid_.setPeaking(sr_, 720.0, 1.1, p_.mid);
        highShelf_.setHighShelf(sr_, 3200.0, 0.7, p_.treble);
        presence_.setPeaking(sr_, 4800.0, 1.4, p_.presence);
        toneDirty_ = false;
    }

    void processBlock(double* buf, int n) {
        if (toneDirty_) updateTone();

        for (int i = 0; i < n; ++i) buf[i] = inHp_.process(buf[i]);

        double drive = smooth_.nextN(n);
        double drive2 = drive * secondStageFactor();

        int L = os_.factor();
        double* hi = os_.upsample(buf, n);
        int hn = n * L;
        for (int i = 0; i < hn; ++i) {
            double y = shapeStage(hi[i], p_.voice, drive, false);
            y = interHp_.process(y);
            y = shapeStage(y, p_.voice, drive2, true);
            hi[i] = y;
        }
        os_.downsample(buf, n);

        // compensate for the gain the shapers add, so presets stay comparable
        double comp = 1.0 / std::sqrt(std::max(1.0, drive * 0.5 + 0.5));
        for (int i = 0; i < n; ++i) {
            double y = buf[i] * comp;
            y = lowShelf_.process(y);
            y = mid_.process(y);
            y = highShelf_.process(y);
            y = presence_.process(y);
            y = dc_.process(y);  // asymmetric stages make DC
            buf[i] = y;
        }
    }

    double latencySamples() const { return os_.delayBaseSamples(); }

private:
    void updateInterStage() {
        double hiRate = sr_ * std::max(1, os_.factor());
        interHp_.set(90.0, hiRate);
    }

    double targetDrive() const {
        // 0..1 -> 0..24 dB of gain before the first stage
        return std::pow(10.0, (p_.gain * 24.0) / 20.0);
    }

    double secondStageFactor() const {
        switch (p_.voice) {
            case Voice::Fender: return 1.15;
            case Voice::Marshall: return 1.70;
            case Voice::Mesa: return 2.30;
        }
        return 1.0;
    }

    Params p_{};
    double sr_ = 48000.0;
    dsp::Biquad inHp_, lowShelf_, mid_, highShelf_, presence_;
    dsp::OnePoleHP dc_, interHp_;
    dsp::Oversampler os_;
    dsp::Smoothed smooth_;
    bool toneDirty_ = true;
};

// ----------------------------------------------------------------- cabinet

// Procedural placeholder: not a real cab. Load a measured IR for anything you
// would let someone else hear via setIr().
inline std::vector<double> placeholderCab(double sr) {
    const int n = 257;
    std::vector<double> ir(static_cast<size_t>(n), 0.0);
    struct Res { double f, q, g; };
    const Res res[] = {{95, 3.5, 1.00}, {140, 4.0, 0.70}, {230, 3.0, 0.50},
                       {480, 2.0, 0.30}, {1900, 1.5, 0.30}, {3200, 1.2, 0.15}};
    for (int i = 0; i < n; ++i) {
        double t = i / sr;
        double v = 0.0;
        for (const auto& r : res) {
            double w = 2 * dsp::kPi * r.f;
            v += r.g * std::exp(-t * w / (2 * r.q)) * std::sin(w * t);
        }
        v *= std::exp(-t * 180.0);   // overall decay
        ir[static_cast<size_t>(i)] = v;
    }
    // Energy-normalise (sum of squares = 1), which preserves loudness for a
    // broadband signal. DC-normalising a resonant IR is wrong: the sum can sit
    // near zero, so the taps either explode or the output goes 20 dB quiet.
    double e = 0;
    for (double v : ir) e += v * v;
    if (e > 1e-12) {
        double s = 1.0 / std::sqrt(e);
        for (auto& v : ir) v *= s;
    }
    return ir;
}

// ------------------------------------------------------------- spring reverb

// A real spring is a dispersive transmission line: high frequencies travel
// faster, which is exactly the "drip". A cascade of allpasses reproduces that
// chirp far better than any comb/plate model.
class SpringReverb {
public:
    void prepare(double sr) {
        sr_ = sr;
        const double ms[5] = {1.1, 1.5, 1.9, 2.3, 2.9};
        for (int i = 0; i < 5; ++i) {
            aps_[i].init(static_cast<int>(ms[i] * 0.001 * sr));
            aps_[i].setGain(0.62 + 0.03 * i);
        }
        line_.init(static_cast<int>(0.20 * sr));
        damp_.set(5200.0, sr);
        hp_.set(140.0, sr);
        baseDelay_ = 0.058 * sr;
        phase_ = 0.0;
    }

    void setDecay(double d) { decay_ = dsp::clampd(0.35 + 0.6 * d, 0.35, 0.95); }
    void clear() { for (auto& a : aps_) a.clear(); line_.clear(); damp_.reset(); hp_.reset(); }

    double process(double x) {
        double d = x;
        for (auto& a : aps_) d = a.process(d);

        phase_ += 0.6 / sr_;
        if (phase_ > 1.0) phase_ -= 1.0;
        double mod = 0.35 * std::sin(2 * dsp::kPi * phase_);  // samples

        double dl = line_.readFrac(baseDelay_ + mod);
        double fb = hp_.process(damp_.process(dl));
        line_.write(dsp::dn(d + decay_ * fb));

        return 0.55 * dl + 0.45 * d;
    }

private:
    double sr_ = 48000.0, baseDelay_ = 2800.0, decay_ = 0.8, phase_ = 0.0;
    dsp::Allpass aps_[5];
    dsp::DelayLine line_;
    dsp::OnePoleLP damp_;
    dsp::OnePoleHP hp_;
};

// --------------------------------------------------------------- plate (FDN)

// 8-line feedback delay network with a Hadamard mixing matrix. Cheap, stable,
// and much closer to a plate/hall than a bank of combs.
class PlateReverb {
public:
    void prepare(double sr) {
        sr_ = sr;
        const double ms[8] = {29.7, 37.1, 41.1, 43.7, 48.3, 53.9, 59.1, 64.7};
        for (int i = 0; i < 8; ++i) {
            int len = static_cast<int>(ms[i] * 0.001 * sr);
            lens_[i] = len;
            lines_[i].init(len + 4);
            damp_[i].set(4200.0 + 400.0 * i, sr);
        }
    }

    void setDecay(double d) { decay_ = dsp::clampd(0.45 + 0.5 * d, 0.45, 0.97); }
    void clear() { for (int i = 0; i < 8; ++i) { lines_[i].clear(); damp_[i].reset(); } }

    double process(double x) {
        double v[8];
        for (int i = 0; i < 8; ++i) v[i] = damp_[i].process(lines_[i].readInt(lens_[i]));
        hadamard(v);
        for (int i = 0; i < 8; ++i) lines_[i].write(v[i] * decay_ + (i == 0 ? x : 0.0));
        double s = 0;
        for (int i = 0; i < 8; ++i) s += v[i];
        return s * 0.35355339059327373;  // 1/sqrt(8)
    }

private:
    static void hadamard(double* v) {
        for (int step = 1; step < 8; step <<= 1)
            for (int i = 0; i < 8; i += step * 2)
                for (int j = i; j < i + step; ++j) {
                    double a = v[j], b = v[j + step];
                    v[j] = a + b;
                    v[j + step] = a - b;
                }
        constexpr double k = 0.35355339059327373;  // 1/sqrt(8)
        for (int i = 0; i < 8; ++i) v[i] *= k;
    }

    double sr_ = 48000.0, decay_ = 0.8;
    dsp::DelayLine lines_[8];
    dsp::OnePoleLP damp_[8];
    int lens_[8] = {0};
};

// --------------------------------------------------------------- tape delay

class TapeDelay {
public:
    void prepare(double sr) {
        sr_ = sr;
        line_.init(static_cast<int>(2.1 * sr));
        lp_.set(3800.0, sr);
        hp_.set(120.0, sr);
        wow_ = flutter_ = 0.0;
    }

    void setTimeMs(double ms) { timeMs_ = dsp::clampd(ms, 0.0, 2000.0); }
    void setFeedback(double f) { fb_ = dsp::clampd(f, 0.0, 0.95); }
    void setMix(double m) { mix_ = dsp::clampd(m, 0.0, 1.0); }
    void clear() { line_.clear(); lp_.reset(); hp_.reset(); }
    bool active() const { return timeMs_ > 1.0 && mix_ > 0.001; }

    double process(double x) {
        if (!active()) return 0.0;

        // wow 0.6 Hz + flutter 6.5 Hz: the two main tape transport artefacts
        wow_ += 0.6 / sr_;
        flutter_ += 6.5 / sr_;
        if (wow_ > 1.0) wow_ -= 1.0;
        if (flutter_ > 1.0) flutter_ -= 1.0;
        double mod = 1.0 + 0.0016 * std::sin(2 * dsp::kPi * wow_) +
                     0.0008 * std::sin(2 * dsp::kPi * flutter_);

        double d = timeMs_ * 0.001 * sr_ * mod;
        double y = line_.readFrac(d);
        // saturation inside the loop is what makes repeats decay into mush
        double fbSig = std::tanh(y * 1.2);
        line_.write(dsp::dn(x + fb_ * hp_.process(lp_.process(fbSig))));
        return mix_ * y;
    }

    double mix() const { return mix_; }

private:
    double sr_ = 48000.0, timeMs_ = 0.0, fb_ = 0.3, mix_ = 0.0, wow_ = 0.0, flutter_ = 0.0;
    dsp::DelayLine line_;
    dsp::OnePoleLP lp_;
    dsp::OnePoleHP hp_;
};

// ------------------------------------------------------------------ engine

// preamp -> reverb -> delay -> cabinet.  The cab is last because it is the
// speaker; the delay before it means repeats get cab-filtered, i.e. an FX loop.
class Engine {
public:
    void prepare(double sr) {
        sr_ = sr;
        pre_.prepare(sr);
        cab_.setTaps(placeholderCab(sr), false);
        spring_.prepare(sr);
        plate_.prepare(sr);
        delay_.prepare(sr);
        // 2nd order, not 1st: a one-pole gives only 6 dB/oct and leaves a
        // measurable offset behind. A biquad highpass has a true zero at DC.
        // Q=0.5, not 0.707: at 0.707 the step response overshoots ~40% on sharp
        // pluck attacks, which pushes the output past full scale and forces the
        // safety clamp to engage (and clamping an asymmetric wave re-adds DC).
        outDc_.setHighpass(sr, 18.0, 0.5);
        outDc_.reset();
    }

    void setIr(const std::vector<double>& ir) {
        if (!ir.empty()) cab_.setTaps(ir, false);
    }

    // Bypass the cabinet entirely, e.g. to audition the preamp or to drive an
    // external IR loader downstream.
    void setCabEnabled(bool on) { cabOn_ = on; }

    void setParams(const Params& p) {
        p_ = p;
        pre_.setParams(p);
        spring_.setDecay(p.reverbDecay);
        plate_.setDecay(p.reverbDecay);
        delay_.setTimeMs(p.delayMs);
        delay_.setFeedback(p.delayFeedback);
        delay_.setMix(p.delayMix);
    }

    void processBlock(double* buf, int n) {
        pre_.processBlock(buf, n);

        for (int i = 0; i < n; ++i) buf[i] = cabOn_ ? cab_.process(buf[i]) : buf[i];

        if (p_.reverbType == 1 && p_.reverbMix > 0.0001) {
            for (int i = 0; i < n; ++i) {
                double dry = buf[i];
                double wet = spring_.process(dry);
                buf[i] = dry * (1.0 - p_.reverbMix) + wet * p_.reverbMix;
            }
        } else if (p_.reverbType == 2 && p_.reverbMix > 0.0001) {
            for (int i = 0; i < n; ++i) {
                double dry = buf[i];
                double wet = plate_.process(dry);
                buf[i] = dry * (1.0 - p_.reverbMix) + wet * p_.reverbMix;
            }
        }

        if (delay_.active()) {
            for (int i = 0; i < n; ++i) {
                double dry = buf[i];
                buf[i] = dry + delay_.process(dry);
            }
        }

        // Order matters here.
        //  1. clip first, so the output is bounded;
        //  2. then remove the DC that clipping an asymmetric waveform creates;
        //  3. then clamp again, because a highpass overshoots on the sharp edges
        //     it was just fed (peak measured at 1.4 without this).
        // Blocking before the clipper cannot work: the clipper re-adds the
        // offset, which is exactly what the previous version did.
        for (int i = 0; i < n; ++i) {
            double v = softclip(buf[i] * p_.outputTrim);
            v = outDc_.process(v);
            buf[i] = (v > 1.0) ? 1.0 : ((v < -1.0) ? -1.0 : v);
        }
    }

    // Added latency of the dry path, in samples. Cab is direct-form FIR, and
    // reverb/delay are wet-only, so oversampling is the whole story.
    double addedLatencySamples() const { return pre_.latencySamples(); }
    double addedLatencyMs() const { return addedLatencySamples() * 1000.0 / sr_; }

private:
    Params p_{};
    double sr_ = 48000.0;
    Preamp pre_;
    dsp::Fir cab_;
    SpringReverb spring_;
    PlateReverb plate_;
    TapeDelay delay_;
    dsp::Biquad outDc_;
    bool cabOn_ = true;
};

// ----------------------------------------------------------------- presets

inline Params presetFender() {
    Params p;
    p.voice = Voice::Fender;
    p.gain = 0.32; p.bass = 4.0; p.mid = -3.5; p.treble = 3.0; p.presence = 0.0;
    p.reverbType = 1; p.reverbMix = 0.32; p.reverbDecay = 0.65;
    p.delayMs = 0.0; p.delayMix = 0.0;
    return p;
}

// No reverb and no delay, because a Marshall head has neither.
inline Params presetMarshall() {
    Params p;
    p.voice = Voice::Marshall;
    p.gain = 0.68; p.bass = 1.5; p.mid = 3.0; p.treble = 2.0; p.presence = 2.0;
    p.reverbType = 0; p.reverbMix = 0.0;
    p.delayMs = 0.0; p.delayMix = 0.0;
    // trimmed so the post-HP peak lands under full scale: otherwise the safety
    // clamp engages, and clamping an asymmetric wave re-introduces DC
    p.outputTrim = 0.70;
    return p;
}

inline Params presetMesa() {
    Params p;
    p.voice = Voice::Mesa;
    p.gain = 0.84; p.bass = 2.5; p.mid = -2.0; p.treble = 2.5; p.presence = 1.0;
    p.reverbType = 1; p.reverbMix = 0.24; p.reverbDecay = 0.72;
    p.delayMs = 0.0; p.delayMix = 0.0;
    p.outputTrim = 0.72;
    return p;
}

inline Params presetByName(const std::string& name) {
    if (name == "marshall") return presetMarshall();
    if (name == "mesa") return presetMesa();
    return presetFender();
}

}  // namespace amp
