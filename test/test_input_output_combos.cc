// Every input/output combination for the per-channel DSP and framing. Drives the
// real ChannelChain and Framer with synthesized wideband IQ carrying, per channel,
// a known upper tone (+2000 Hz) and lower tone (-1000 Hz).
// Validates, for every combination of channel count (1,2,3), mode (usb/lsb/iq),
// format (i16/f32), and output rate (12/48/96/192 k) over a range of input rates:
//   - stream structure: framed sdrf_hdr for audio (magic/version/rate/flags/nsamples/
//     timestamps) vs raw headerless for iq, correct sample size and count
//   - data accuracy: usb passes the upper tone and rejects the lower, lsb the reverse,
//     iq preserves both sidebands at the right baseband frequencies
#include "dsp.h"
#include "framer.h"
#include "sdrf.h"
#include "capture.h"
#include <complex>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <numeric>
#include <string>
#include <map>
#include <fcntl.h>
#include <unistd.h>

static const double PI = 3.14159265358979323846;
static const double CENTER = 7100000.0;
static const double SPACING = 120000.0;            // > any outrate/2, so channels don't overlap
static const double F_UP = 2000.0, A_UP = 0.40;    // upper-sideband test tone
static const double F_LO = 1000.0, A_LO = 0.30;    // lower-sideband test tone
static const double DUR = 0.1;                     // seconds of signal per case

static int fails = 0, checks = 0;
static std::map<int, double> audio_min_rej;   // worst want/other ratio per output rate
static void fail(const std::string &m) { std::printf("  FAIL %s\n", m.c_str()); fails++; }

static std::vector<double> dials_of(int n) {
    std::vector<double> d;
    for (int i = 0; i < n; i++) d.push_back(CENTER + SPACING * (i - (n - 1) / 2.0));
    return d;
}
static long lcm_all(const std::vector<int> &rs) {
    long l = 1; for (int r : rs) l = std::lcm(l, (long)r); return l;
}
// smallest fs (mult=1) or larger (mult=2) that is a multiple of every outrate and
// leaves Nyquist room for the farthest channel plus its passband.
static long pick_fs(const std::vector<int> &outrates, double max_off, int mult) {
    long lcm = lcm_all(outrates);
    int maxhalf = 0; for (int r : outrates) maxhalf = std::max(maxhalf, r / 2);
    double need = 2.0 * (max_off + maxhalf + 3000.0);
    long base = (long)std::ceil(need / lcm) * lcm;
    return base * mult;
}
static std::vector<std::complex<float>> gen(long fs, const std::vector<double> &dials, long nsamp) {
    std::vector<std::complex<float>> v(nsamp);
    std::vector<double> up, lo;
    for (double d : dials) { up.push_back(d - CENTER + F_UP); lo.push_back(d - CENTER - F_LO); }
    for (long n = 0; n < nsamp; n++) {
        std::complex<double> s(0, 0);
        for (size_t c = 0; c < dials.size(); c++) {
            double pu = 2 * PI * up[c] * n / fs, pl = 2 * PI * lo[c] * n / fs;
            s += std::complex<double>(A_UP * cos(pu), A_UP * sin(pu));
            s += std::complex<double>(A_LO * cos(pl), A_LO * sin(pl));
        }
        v[n] = std::complex<float>((float)s.real(), (float)s.imag());
    }
    return v;
}
static std::vector<std::complex<double>> decode(const std::vector<uint8_t> &b, Mode m, Format f) {
    std::vector<std::complex<double>> out;
    size_t es = (f == Format::F32) ? 4 : 2;
    auto val = [&](const uint8_t *p) -> double {
        if (f == Format::F32) { float x; std::memcpy(&x, p, 4); return x; }
        int16_t x; std::memcpy(&x, p, 2); return x / 16000.0;     // inverse of the i16 scale
    };
    if (m == Mode::IQ) {
        for (size_t i = 0; i + 2 * es <= b.size(); i += 2 * es) out.push_back({val(&b[i]), val(&b[i + es])});
    } else {
        for (size_t i = 0; i + es <= b.size(); i += es) out.push_back({val(&b[i]), 0.0});
    }
    return out;
}
static double binmag(const std::vector<std::complex<double>> &s, double f, double rate) {
    std::complex<double> acc(0, 0);
    for (size_t n = 0; n < s.size(); n++) acc += s[n] * std::polar(1.0, -2 * PI * f * n / rate);
    return s.empty() ? 0 : std::abs(acc) / s.size();
}

// One channel: structure of the raw process() bytes + data accuracy.
static void check_channel(const std::string &tag, int c, double delta, long fs, int R,
                          Mode m, Format fmt, const std::vector<std::complex<float>> &iq) {
    checks++;
    ChannelChain ch(delta, (int)fs, R, m, fmt);
    std::vector<uint8_t> bytes;
    ch.process(iq.data(), iq.size(), bytes);

    int es = (fmt == Format::F32) ? 4 : 2;
    int sb = es * (m == Mode::IQ ? 2 : 1);
    std::string id = tag + " ch" + std::to_string(c);
    if (bytes.size() % sb != 0) { fail(id + " bytes not a whole number of samples"); return; }
    long got = (long)(bytes.size() / sb), expect = (long)(iq.size() / (fs / R));
    if (std::labs(got - expect) > 2) { fail(id + " sample count " + std::to_string(got) + " != ~" + std::to_string(expect)); return; }

    auto s = decode(bytes, m, fmt);
    if (m == Mode::IQ) {
        double up = binmag(s, +F_UP, R), lo = binmag(s, -F_LO, R), empty = binmag(s, F_UP + 1500, R);
        if (!(up > 0.05 && lo > 0.05 && up > 4 * empty && lo > 4 * empty))
            fail(id + " iq up=" + std::to_string(up) + " lo=" + std::to_string(lo) + " empty=" + std::to_string(empty));
    } else {
        // the correct sideband must clearly dominate (selection); absolute rejection
        // is tracked separately because the fixed-tap Hilbert weakens it for audio-band
        // tones at high output rates.
        double want = (m == Mode::USB) ? binmag(s, F_UP, R) : binmag(s, F_LO, R);
        double other = (m == Mode::USB) ? binmag(s, F_LO, R) : binmag(s, F_UP, R);
        double ratio = want / std::max(other, 1e-9);
        auto it = audio_min_rej.find(R);
        if (it == audio_min_rej.end() || ratio < it->second) audio_min_rej[R] = ratio;
        if (!(want > 0.05 && ratio > 2.0))
            fail(id + (m == Mode::USB ? " usb" : " lsb") + " want=" + std::to_string(want) + " other=" + std::to_string(other));
    }
}

// Framed/raw stream structure for one (mode, format, outrate).
static void check_stream(Mode m, Format fmt, int R) {
    checks++;
    std::vector<int> outr = {R};
    long fs = pick_fs(outr, 0, 1);
    auto iq = gen(fs, dials_of(1), (long)(fs * 0.3));
    ChannelChain ch(0, (int)fs, R, m, fmt);
    std::vector<uint8_t> bytes; ch.process(iq.data(), iq.size(), bytes);

    int es = (fmt == Format::F32) ? 4 : 2, sb = es * (m == Mode::IQ ? 2 : 1);
    int frame = R / 10; bool framed = (m != Mode::IQ), f32 = (fmt == Format::F32);
    size_t fb = (size_t)frame * sb, whole = (bytes.size() / fb) * fb;
    std::string label = std::string(m == Mode::USB ? "usb" : m == Mode::LSB ? "lsb" : "iq") +
                        "/" + (f32 ? "f32" : "i16") + "/" + std::to_string(R);

    char path[] = "/tmp/sftest_stream.XXXXXX";
    int fd = mkstemp(path);
    { Framer fr(fd, R, frame, 1.0, sb, framed, f32); fr.add(bytes.data(), bytes.size(), 1000000.0); }
    lseek(fd, 0, SEEK_SET);

    if (framed) {
        sdrf_hdr h; int frames = 0; bool ok = true; double last_t = 0;
        std::vector<uint8_t> payload;
        while (read(fd, &h, sizeof h) == (ssize_t)sizeof h) {
            if (h.magic != SDRF_MAGIC || h.version != SDRF_VERSION || h.rate_hz != (uint32_t)R ||
                h.nsamples != (uint32_t)frame || h.flags != (f32 ? SDRF_FLAG_F32 : 0)) { ok = false; break; }
            double t = h.t_usec / 1e6;
            if (frames > 0 && !(t - last_t > 0.09 && t - last_t < 0.11)) ok = false;   // ~0.1 s spacing
            last_t = t;
            std::vector<uint8_t> p(fb);
            if (read(fd, p.data(), fb) != (ssize_t)fb) { ok = false; break; }
            payload.insert(payload.end(), p.begin(), p.end()); frames++;
        }
        if (!ok || frames < 1 || payload.size() != whole || std::memcmp(payload.data(), bytes.data(), whole) != 0)
            fail("stream " + label + " framed (frames=" + std::to_string(frames) + ")");
    } else {
        std::vector<uint8_t> file; uint8_t buf[65536]; ssize_t r;
        while ((r = read(fd, buf, sizeof buf)) > 0) file.insert(file.end(), buf, buf + r);
        uint32_t magic = file.size() >= 4 ? *(uint32_t *)file.data() : 0;
        if (magic == SDRF_MAGIC || file.size() != whole || std::memcmp(file.data(), bytes.data(), whole) != 0)
            fail("stream " + label + " raw (size=" + std::to_string(file.size()) + " whole=" + std::to_string(whole) + ")");
    }
    close(fd); unlink(path);
}

int main() {
    int rates[] = {12000, 48000, 96000, 192000};

    std::printf("== stream structure: every mode x format x outrate ==\n");
    for (int mi = 0; mi < 3; mi++) for (int fi = 0; fi < 2; fi++) for (int R : rates)
        check_stream((Mode)mi, fi ? Format::F32 : Format::I16, R);

    std::printf("== data accuracy + structure: 1/2/3 channels, all mode combos, mixed rates ==\n");
    std::vector<std::vector<std::vector<int>>> oc(4);
    oc[1] = {{12000}, {48000}, {96000}, {192000}};
    oc[2] = {{12000,12000},{48000,48000},{96000,96000},{192000,192000},{12000,48000},{48000,192000},{96000,12000}};
    oc[3] = {{12000,12000,12000},{48000,48000,48000},{96000,96000,96000},{192000,192000,192000},
             {12000,48000,96000},{192000,96000,12000},{48000,48000,192000}};
    for (int N = 1; N <= 3; N++) {
        auto dials = dials_of(N);
        double max_off = SPACING * (N - 1) / 2.0;
        int combos = 1; for (int i = 0; i < N; i++) combos *= 3;
        for (auto &outr : oc[N]) {
            for (int mult : {1, 2}) {
                long fs = pick_fs(outr, max_off, mult);
                auto iq = gen(fs, dials, (long)(fs * DUR));     // shared across modes/formats
                for (int mc = 0; mc < combos; mc++) {
                    std::vector<int> modes(N); int x = mc; for (int i = 0; i < N; i++) { modes[i] = x % 3; x /= 3; }
                    for (Format fmt : {Format::I16, Format::F32}) {
                        char tag[128]; int o = std::snprintf(tag, sizeof tag, "N=%d fs=%ld m%d fmt=%s [", N, fs, mult, fmt == Format::F32 ? "f32" : "i16");
                        for (int c = 0; c < N; c++) o += std::snprintf(tag + o, sizeof tag - o, "%s%s/%d", c ? "," : "", modes[c] == 0 ? "usb" : modes[c] == 1 ? "lsb" : "iq", outr[c]);
                        std::snprintf(tag + o, sizeof tag - o, "]");
                        for (int c = 0; c < N; c++)
                            check_channel(tag, c, dials[c] - CENTER, fs, outr[c], (Mode)modes[c], fmt, iq);
                    }
                }
            }
        }
    }

    std::printf("\naudio sideband selection margin (min want/other) by output rate:\n");
    for (auto &kv : audio_min_rej)
        std::printf("  %6d Hz: %.1fx  (%.0f dB)\n", kv.first, kv.second, 20 * std::log10(kv.second));

    std::printf("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
