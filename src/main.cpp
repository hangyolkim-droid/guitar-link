// main.cpp - offline renderer + self-test for the amp.
//
// No JUCE, no external deps: this compiles with a bare g++ and gives you a WAV
// you can inspect. The real-time host adapter is a thin wrapper around
// amp::Engine::processBlock.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "amp.h"

// ---------------------------------------------------------------- wav i/o

struct Wav {
    int sr = 0;
    std::vector<double> x;
};

static uint16_t rd16(const unsigned char* p) { return uint16_t(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static bool readWav(const std::string& path, Wav& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> b(static_cast<size_t>(sz));
    if (std::fread(b.data(), 1, b.size(), f) != b.size()) { std::fclose(f); err = "short read"; return false; }
    std::fclose(f);

    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) {
        err = "not a RIFF/WAVE file"; return false;
    }

    int fmt = 0, ch = 0, bits = 0;
    size_t pos = 12;
    const unsigned char* data = nullptr;
    size_t dataLen = 0;

    while (pos + 8 <= b.size()) {
        char id[5] = {0};
        std::memcpy(id, b.data() + pos, 4);
        uint32_t len = rd32(b.data() + pos + 4);
        size_t body = pos + 8;
        if (!std::strcmp(id, "fmt ") && body + 16 <= b.size()) {
            fmt = rd16(b.data() + body);
            ch = rd16(b.data() + body + 2);
            out.sr = int(rd32(b.data() + body + 4));
            bits = rd16(b.data() + body + 14);
        } else if (!std::strcmp(id, "data")) {
            data = b.data() + body;
            dataLen = std::min<size_t>(len, b.size() - body);
        }
        pos = body + len + (len & 1);
    }

    if (!data || !ch || !out.sr) { err = "missing fmt/data chunk"; return false; }
    int bytes = bits / 8;
    if (bytes < 1) { err = "bad bit depth"; return false; }
    size_t frames = dataLen / (size_t(bytes) * ch);
    out.x.assign(frames, 0.0);

    for (size_t i = 0; i < frames; ++i) {
        double acc = 0.0;
        for (int c = 0; c < ch; ++c) {
            const unsigned char* p = data + (i * ch + c) * bytes;
            double v = 0.0;
            if (fmt == 3 && bits == 32) {
                float fv;
                std::memcpy(&fv, p, 4);
                v = fv;
            } else if (bits == 16) {
                v = int16_t(rd16(p)) / 32768.0;
            } else if (bits == 24) {
                int32_t q = int32_t(p[0]) | (int32_t(p[1]) << 8) | (int32_t(p[2]) << 16);
                if (q & 0x800000) q -= 0x1000000;
                v = q / 8388608.0;
            } else if (bits == 32) {
                v = int32_t(rd32(p)) / 2147483648.0;
            }
            acc += v;
        }
        out.x[i] = acc / ch;
    }
    return true;
}

static bool writeWav(const std::string& path, const std::vector<double>& x, int sr) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    uint32_t dataBytes = uint32_t(x.size() * 2);
    unsigned char h[44] = {0};
    std::memcpy(h, "RIFF", 4);
    uint32_t riff = 36 + dataBytes;
    std::memcpy(h + 4, &riff, 4);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t fmtLen = 16; std::memcpy(h + 16, &fmtLen, 4);
    uint16_t pcm = 1; std::memcpy(h + 20, &pcm, 2);
    uint16_t mono = 1; std::memcpy(h + 22, &mono, 2);
    std::memcpy(h + 24, &sr, 4);
    uint32_t byteRate = uint32_t(sr) * 2; std::memcpy(h + 28, &byteRate, 4);
    uint16_t align = 2; std::memcpy(h + 32, &align, 2);
    uint16_t bits = 16; std::memcpy(h + 34, &bits, 2);
    std::memcpy(h + 36, "data", 4);
    std::memcpy(h + 40, &dataBytes, 4);
    std::fwrite(h, 1, 44, f);
    for (double v : x) {
        double c = std::max(-1.0, std::min(1.0, v));
        int16_t s = int16_t(std::lround(c * 32767.0));
        std::fwrite(&s, 1, 2, f);
    }
    std::fclose(f);
    return true;
}

// ------------------------------------------------------------- test helpers

static double toneAmp(const std::vector<double>& x, double freq, double sr) {
    size_t n = x.size();
    if (n < 16) return 0.0;
    double w = 2 * dsp::kPi * freq / sr;
    double coeff = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; ++i) {
        double win = 0.5 - 0.5 * std::cos(2 * dsp::kPi * (double)i / (double)(n - 1));
        double s0 = x[i] * win + coeff * s1 - s2;
        s2 = s1; s1 = s0;
    }
    double p = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return std::sqrt(std::max(0.0, p)) * 2.0 / (double)n;
}

static double db(double v) { return 20.0 * std::log10(std::max(v, 1e-18)); }

static bool finite(const std::vector<double>& x) {
    for (double v : x) if (!std::isfinite(v)) return false;
    return true;
}

static double peak(const std::vector<double>& x) {
    double p = 0;
    for (double v : x) p = std::max(p, std::fabs(v));
    return p;
}

static double rmsRange(const std::vector<double>& x, size_t a, size_t b) {
    if (b > x.size()) b = x.size();
    if (a >= b) return 0.0;
    double s = 0;
    for (size_t i = a; i < b; ++i) s += x[i] * x[i];
    return std::sqrt(s / double(b - a));
}

// Mesa at high gain: 5th harmonic of 7 kHz is 35 kHz, which folds to 13 kHz at
// 48 kHz. With 4x oversampling it must not.
static double aliasEnergy(int os, double sr, double f0) {
    amp::Preamp pre;
    pre.prepare(sr);
    amp::Params p;
    p.voice = amp::Voice::Mesa;
    p.gain = 0.9;
    p.oversample = os;
    p.bass = p.mid = p.treble = p.presence = 0;
    pre.setParams(p);

    int n = int(sr * 1.0);
    std::vector<double> buf(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) buf[static_cast<size_t>(i)] = 0.5 * std::sin(2 * dsp::kPi * f0 * i / sr);

    const int block = 64;
    for (int i = 0; i + block <= n; i += block) pre.processBlock(&buf[static_cast<size_t>(i)], block);

    std::vector<double> tail(buf.begin() + n / 2, buf.end());
    return toneAmp(tail, 13000.0, sr);
}

static std::vector<double> pluck(double sr, double f0, double seconds, double amp) {
    int N = std::max(2, int(sr / f0));
    std::vector<double> ring(static_cast<size_t>(N));
    uint32_t s = 22222u;
    for (auto& v : ring) {
        s = s * 1664525u + 1013904223u;
        v = (double((s >> 16) & 0x7fff) / 16384.0) - 1.0;
    }
    std::vector<double> out(static_cast<size_t>(seconds * sr));
    size_t idx = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        double v = ring[idx];
        double nxt = ring[(idx + 1) % ring.size()];
        ring[idx] = 0.996 * 0.5 * (v + nxt);
        out[i] = v * amp;
        idx = (idx + 1) % ring.size();
    }
    // 30 ms fade so the demo does not click
    size_t fade = std::min<size_t>(out.size(), size_t(0.03 * sr));
    for (size_t i = 0; i < fade; ++i) out[out.size() - 1 - i] *= double(i) / double(fade);
    return out;
}

// ---------------------------------------------------------------- selftest

static int selftest(double sr) {
    bool ok = true;
    auto check = [&](const char* name, bool cond, const std::string& detail) {
        ok = ok && cond;
        std::printf("  %s %-40s %s\n", cond ? "ok  " : "FAIL", name, detail.c_str());
    };

    // 1. oversampler reports the delay it actually has
    {
        dsp::Oversampler os;
        os.init(4);
        check("oversampler reports latency", std::fabs(os.delayBaseSamples() - 32.0) < 1e-9,
              std::to_string(os.delayBaseSamples()) + " base samples");
    }

    // 2. FIR is unity-DC
    {
        dsp::Fir fir;
        fir.setTaps(dsp::designLowpass(129, 0.2));
        double y = 0;
        for (int i = 0; i < 2000; ++i) y = fir.process(0.5);
        check("FIR unity DC gain", std::fabs(y - 0.5) < 0.01, "y=" + std::to_string(y));
    }

    // 3. silence in -> silence out, including reverb and delay tanks
    for (const char* name : {"fender", "marshall", "mesa"}) {
        amp::Engine e;
        e.prepare(sr);
        amp::Params p = amp::presetByName(name);
        p.delayMs = 320; p.delayMix = 0.25; p.delayFeedback = 0.35;
        e.setParams(p);
        std::vector<double> buf(static_cast<size_t>(sr), 0.0);
        for (size_t i = 0; i < buf.size(); i += 64)
            e.processBlock(&buf[i], 64);
        check((std::string("silence stays silent: ") + name).c_str(),
              peak(buf) < 1e-9, "peak=" + std::to_string(peak(buf)));
    }

    // 4. signal passes, stays finite, and never exceeds full scale
    for (const char* name : {"fender", "marshall", "mesa"}) {
        amp::Engine e;
        e.prepare(sr);
        e.setParams(amp::presetByName(name));
        auto in = pluck(sr, 110.0, 1.5, 0.35);
        std::vector<double> buf = in;
        for (size_t i = 0; i + 64 <= buf.size(); i += 64) e.processBlock(&buf[i], 64);
        bool fin = finite(buf);
        double pk = peak(buf);
        double r = rmsRange(buf, 0, buf.size());
        check((std::string("renders finite, bounded: ") + name).c_str(),
              fin && pk <= 1.0 && r > 1e-3,
              "peak=" + std::to_string(pk) + " rms=" + std::to_string(r));
    }

    // 5. no DC left after the asymmetric Fender stage
    {
        amp::Engine e;
        e.prepare(sr);
        amp::Params p = amp::presetByName("fender");
        p.gain = 0.85;
        e.setParams(p);
        int n = int(sr);
        std::vector<double> buf(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) buf[static_cast<size_t>(i)] = 0.6 * std::sin(2 * dsp::kPi * 220.0 * i / sr);
        for (int i = 0; i + 64 <= n; i += 64) e.processBlock(&buf[static_cast<size_t>(i)], 64);
        double mean = 0;
        for (size_t i = buf.size() / 2; i < buf.size(); ++i) mean += buf[i];
        mean /= double(buf.size() / 2);
        check("DC blocker removes offset", std::fabs(mean) < 1e-3, "mean=" + std::to_string(mean));
    }

    // 6. oversampling actually suppresses the folded harmonic
    {
        double a1 = aliasEnergy(1, sr, 7000.0);
        double a4 = aliasEnergy(4, sr, 7000.0);
        double improvement = db(a1) - db(a4);
        check("4x oversampling suppresses aliasing", improvement > 10.0,
              std::to_string(improvement) + " dB less energy at 13 kHz");
    }

    // 7. reverb/delay tails decay rather than run away
    {
        amp::Engine e;
        e.prepare(sr);
        amp::Params p = amp::presetByName("fender");
        p.reverbMix = 0.45; p.reverbDecay = 0.85;
        p.delayMs = 250; p.delayMix = 0.3; p.delayFeedback = 0.5;
        e.setParams(p);
        int n = int(sr * 4.0);
        std::vector<double> buf(static_cast<size_t>(n), 0.0);
        buf[100] = 0.7;
        for (int i = 0; i + 64 <= n; i += 64) e.processBlock(&buf[static_cast<size_t>(i)], 64);
        double early = rmsRange(buf, size_t(0.30 * sr), size_t(0.50 * sr));
        double late = rmsRange(buf, size_t(3.6 * sr), size_t(3.9 * sr));
        // early > 1e-4 guards against a vacuous pass: "tail decays" is trivially
        // true if the tail never existed in the first place.
        check("tails decay", early > 1e-4 && late < early * 0.5,
              "early=" + std::to_string(early) + " late=" + std::to_string(late));
    }

    // 8. reported latency matches the model used by the Python budget
    {
        amp::Engine e;
        e.prepare(sr);
        double ms = e.addedLatencyMs();
        check("engine latency matches budget", std::fabs(ms - 0.667) < 0.02,
              std::to_string(ms) + " ms");
    }

    std::printf("\n%s\n", ok ? "all assertions passed" : "FAILURES above");
    return ok ? 0 : 1;
}

// --------------------------------------------------------------------- cli

static void usage() {
    std::printf(
        "usage:\n"
        "  amp_sim --selftest\n"
        "  amp_sim --demo DIR\n"
        "  amp_sim --in di.wav --out amp.wav [options]\n\n"
        "options:\n"
        "  --preset fender|marshall|mesa   --gain 0..1\n"
        "  --bass / --mid / --treble / --presence  dB\n"
        "  --os 1|2|4                      --trim linear\n"
        "  --reverb spring|plate|off       --rex 0..1   --rdecay 0..1\n"
        "  --delay MS                      --dfb 0..0.95 --dmix 0..1\n"
        "  --ir cab.wav                    --no-cab\n"
        "\n"
        "Only flags you pass override the preset, so --bass 0 really means 0.\n");
}

// Runs every sample, including a final partial block. The obvious loop
// `for (i; i + 64 <= size; i += 64)` silently drops the last 63 samples.
static void processAll(amp::Engine& e, std::vector<double>& buf, int block = 64) {
    size_t i = 0;
    while (i < buf.size()) {
        int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), buf.size() - i));
        e.processBlock(&buf[i], n);
        i += static_cast<size_t>(n);
    }
}

int main(int argc, char** argv) {
    std::string in, out, dir = ".", preset = "fender", reverb, ir;
    std::optional<double> gain, bass, mid, treble, presence, trim, rex, rdecay, dms, dfb, dmix;
    std::optional<int> os;
    bool doTest = false, doDemo = false, noCab = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires a value\n", a.c_str());
                std::exit(2);
            }
            return std::string(argv[++i]);
        };
        auto num = [&]() -> double {
            std::string s = need();
            try {
                size_t k = 0;
                double v = std::stod(s, &k);
                if (k != s.size()) throw std::invalid_argument("trailing");
                return v;
            } catch (...) {
                std::fprintf(stderr, "%s: not a number: '%s'\n", a.c_str(), s.c_str());
                std::exit(2);
            }
        };

        if (a == "--selftest") doTest = true;
        else if (a == "--demo") { doDemo = true; if (i + 1 < argc && argv[i + 1][0] != '-') dir = argv[++i]; }
        else if (a == "--in") in = need();
        else if (a == "--out") out = need();
        else if (a == "--ir") ir = need();
        else if (a == "--preset") preset = need();
        else if (a == "--no-cab") noCab = true;
        else if (a == "--gain") gain = num();
        else if (a == "--bass") bass = num();
        else if (a == "--mid") mid = num();
        else if (a == "--treble") treble = num();
        else if (a == "--presence") presence = num();
        else if (a == "--os") os = static_cast<int>(num());
        else if (a == "--trim") trim = num();
        else if (a == "--reverb") reverb = need();
        else if (a == "--rex") rex = num();
        else if (a == "--rdecay") rdecay = num();
        else if (a == "--delay") dms = num();
        else if (a == "--dfb") dfb = num();
        else if (a == "--dmix") dmix = num();
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); usage(); return 2; }
    }

    if (doTest) return selftest(48000.0);
    if (preset != "fender" && preset != "marshall" && preset != "mesa") {
        std::fprintf(stderr, "unknown preset '%s' (fender|marshall|mesa)\n", preset.c_str());
        return 2;
    }

    // The preset is the base; only flags actually supplied override it. The old
    // version compared against default values, so `--bass 0` was ignored.
    amp::Params p = amp::presetByName(preset);
    if (gain) p.gain = *gain;
    if (bass) p.bass = *bass;
    if (mid) p.mid = *mid;
    if (treble) p.treble = *treble;
    if (presence) p.presence = *presence;
    if (trim) p.outputTrim = *trim;
    if (rex) p.reverbMix = *rex;
    if (rdecay) p.reverbDecay = *rdecay;
    if (dms) p.delayMs = *dms;
    if (dfb) p.delayFeedback = *dfb;
    if (dmix) p.delayMix = *dmix;
    if (os) {
        if (*os != 1 && *os != 2 && *os != 4) {
            std::fprintf(stderr, "--os must be 1, 2 or 4\n");
            return 2;
        }
        p.oversample = *os;
    }

    if (reverb == "off") { p.reverbType = 0; p.reverbMix = 0.0; }
    else if (reverb == "spring") p.reverbType = 1;
    else if (reverb == "plate") p.reverbType = 2;
    else if (!reverb.empty()) {
        std::fprintf(stderr, "--reverb must be spring, plate or off\n");
        return 2;
    }

    p.gain = std::max(0.0, std::min(1.0, p.gain));
    p.reverbMix = std::max(0.0, std::min(1.0, p.reverbMix));
    p.reverbDecay = std::max(0.0, std::min(1.0, p.reverbDecay));
    p.delayMs = std::max(0.0, std::min(2000.0, p.delayMs));
    p.delayFeedback = std::max(0.0, std::min(0.95, p.delayFeedback));
    p.delayMix = std::max(0.0, std::min(1.0, p.delayMix));
    p.outputTrim = std::max(0.0, std::min(4.0, p.outputTrim));

    if (doDemo) {
        const double sr = 48000.0;
        std::vector<double> di;
        const double notes[] = {110.0, 146.83, 164.81, 196.0, 146.83, 110.0};
        for (double f : notes) {
            auto n = pluck(sr, f, 0.42, 0.35);
            di.insert(di.end(), n.begin(), n.end());
        }
        std::string diPath = dir + "/di.wav";
        if (!writeWav(diPath, di, int(sr))) { std::fprintf(stderr, "cannot write %s\n", diPath.c_str()); return 1; }

        for (const char* name : {"fender", "marshall", "mesa"}) {
            amp::Engine e;
            e.prepare(sr);
            amp::Params pp = amp::presetByName(name);
            pp.delayMs = (std::string(name) == "mesa") ? 380.0 : 0.0;
            pp.delayMix = (std::string(name) == "mesa") ? 0.22 : 0.0;
            pp.delayFeedback = 0.32;
            e.setParams(pp);
            std::vector<double> buf = di;
            processAll(e, buf);
            std::string path = dir + "/" + name + ".wav";
            writeWav(path, buf, int(sr));
            std::printf("%-10s -> %s  peak %.3f rms %.4f\n", name, path.c_str(), peak(buf),
                        rmsRange(buf, 0, buf.size()));
        }
        std::printf("wrote %s/di.wav plus three renders\n", dir.c_str());
        return 0;
    }

    if (in.empty() || out.empty()) { usage(); return 2; }
    Wav w;
    std::string err;
    if (!readWav(in, w, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    if (w.x.empty()) { std::fprintf(stderr, "%s: no audio frames\n", in.c_str()); return 1; }
    double sr = double(w.sr);

    amp::Engine e;
    e.prepare(sr);
    if (!ir.empty()) {
        Wav irw;
        if (!readWav(ir, irw, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        if (irw.x.size() < 2) { std::fprintf(stderr, "%s: IR needs at least 2 samples\n", ir.c_str()); return 1; }
        std::vector<double> taps = irw.x;
        if (int(irw.sr) != int(sr)) {
            // linear resample to the session rate
            size_t n = static_cast<size_t>(double(taps.size()) * sr / double(irw.sr));
            if (n < 2) { std::fprintf(stderr, "IR resamples to %zu samples\n", n); return 1; }
            std::vector<double> r(n);
            for (size_t i = 0; i < n; ++i) {
                double t = double(i) * double(irw.sr) / sr;
                size_t i0 = std::min(taps.size() - 1, static_cast<size_t>(t));
                size_t i1 = std::min(taps.size() - 1, i0 + 1);
                double f = t - double(i0);
                r[i] = taps[i0] * (1 - f) + taps[i1] * f;
            }
            taps.swap(r);
        }
        e.setIr(taps);
        std::printf("cab IR: %s (%zu taps)\n", ir.c_str(), taps.size());
    }
    e.setCabEnabled(!noCab);
    e.setParams(p);

    processAll(e, w.x);

    if (!writeWav(out, w.x, int(sr))) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
    std::printf("%s -> %s  added latency %.3f ms  peak %.3f\n", in.c_str(), out.c_str(),
                e.addedLatencyMs(), peak(w.x));
    return 0;
}
