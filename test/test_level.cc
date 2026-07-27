// Validation of the -level signal level report.
// Checks the reported dBFS against known input amplitudes, that every mode
// measures the same thing, that a quiet channel reports a finite floor, and
// that the levels reset between reports.
#include "dsp.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <complex>
#include <vector>

static const double PI = 3.14159265358979323846;

static int fails = 0;

static void near(float got, float want, float tol, const char *what) {
    if (std::fabs(got - want) > tol) {
        std::printf("FAIL %s: got %.2f want %.2f (+/-%.2f)\n", what, got, want, tol);
        fails++;
    }
}

// Feed a complex tone of the given amplitude at the channel dial, return the
// level report. Amplitude 1.0 at the dial is full scale, so it reports 0 dBFS.
static bool levels(Mode mode, float amp, float &mn, float &av, float &mx,
                   int fs = 96000, int out_rate = 12000, double seconds = 1.0) {
    ChannelChain ch(0.0, fs, out_rate, mode, Format::I16);
    ch.enable_level();
    long n = (long)(fs * seconds);
    std::vector<std::complex<float>> iq(n);
    for (long k = 0; k < n; k++) {
        double ph = 2 * PI * 1000.0 * k / fs;    // inside the channel passband
        iq[k] = std::complex<float>(amp * std::cos(ph), amp * std::sin(ph));
    }
    std::vector<uint8_t> out;
    ch.process(iq.data(), iq.size(), out);
    return ch.take_level(mn, av, mx);
}

int main() {
    float mn, av, mx;

    // A full scale tone at the dial reads 0 dBFS, and a steady tone makes
    // min, avg and max agree.
    if (!levels(Mode::USB, 1.0f, mn, av, mx)) { std::printf("FAIL no level data\n"); fails++; }
    near(mn, 0.0f, 0.5f, "full scale min");
    near(av, 0.0f, 0.5f, "full scale avg");
    near(mx, 0.0f, 0.5f, "full scale max");

    // Halving the amplitude drops the level by 6 dB.
    levels(Mode::USB, 0.5f, mn, av, mx);
    near(av, -6.0f, 0.5f, "half scale avg");

    // A tenth of full scale is -20 dBFS.
    levels(Mode::USB, 0.1f, mn, av, mx);
    near(av, -20.0f, 0.5f, "tenth scale avg");

    // Levels are taken before the demod, so the mode cannot change them.
    float u_mn, u_av, u_mx, l_mn, l_av, l_mx, q_mn, q_av, q_mx;
    levels(Mode::USB, 0.25f, u_mn, u_av, u_mx);
    levels(Mode::LSB, 0.25f, l_mn, l_av, l_mx);
    levels(Mode::IQ,  0.25f, q_mn, q_av, q_mx);
    if (u_av != l_av || u_av != q_av || u_mn != l_mn || u_mn != q_mn) {
        std::printf("FAIL mode changes the level: usb %.3f lsb %.3f iq %.3f\n",
                    u_av, l_av, q_av);
        fails++;
    }

    // Noise: the minimum is a quiet level, not a single sample null, so it has
    // to stay close to the mean rather than running off toward -inf.
    {
        const int fs = 96000, out_rate = 12000;
        ChannelChain ch(0.0, fs, out_rate, Mode::USB, Format::I16);
        ch.enable_level();
        std::srand(1);
        std::vector<std::complex<float>> iq(fs);
        for (int k = 0; k < fs; k++) {
            float i = (float)((std::rand() / (double)RAND_MAX) - 0.5) * 0.2f;
            float q = (float)((std::rand() / (double)RAND_MAX) - 0.5) * 0.2f;
            iq[k] = std::complex<float>(i, q);
        }
        std::vector<uint8_t> out;
        ch.process(iq.data(), iq.size(), out);
        if (!ch.take_level(mn, av, mx)) { std::printf("FAIL no noise level\n"); fails++; }
        std::printf("noise: min %.1f avg %.1f max %.1f dBFS\n", mn, av, mx);
        if (!std::isfinite(mn) || mn < -120.0f) {
            std::printf("FAIL noise min is not a level: %.1f\n", mn);
            fails++;
        }
        if (av - mn > 6.0f) {
            std::printf("FAIL noise min sits %.1f dB under the mean\n", av - mn);
            fails++;
        }
    }

    // A silent channel reports a floor instead of -inf.
    {
        ChannelChain ch(0.0, 96000, 12000, Mode::USB, Format::I16);
        ch.enable_level();
        std::vector<std::complex<float>> iq(96000, std::complex<float>(0, 0));
        std::vector<uint8_t> out;
        ch.process(iq.data(), iq.size(), out);
        if (!ch.take_level(mn, av, mx)) { std::printf("FAIL no silent level\n"); fails++; }
        if (!std::isfinite(mn) || !std::isfinite(av)) {
            std::printf("FAIL silence is not finite: min %.1f avg %.1f\n", mn, av);
            fails++;
        }
    }

    // Reporting resets the accumulators: a second read with no new samples has
    // nothing to report, and levels do not carry across reports.
    {
        const int fs = 96000;
        ChannelChain ch(0.0, fs, 12000, Mode::USB, Format::I16);
        ch.enable_level();
        std::vector<std::complex<float>> iq(fs);
        for (int k = 0; k < fs; k++) {
            double ph = 2 * PI * 1000.0 * k / fs;
            iq[k] = std::complex<float>(0.5f * std::cos(ph), 0.5f * std::sin(ph));
        }
        std::vector<uint8_t> out;
        ch.process(iq.data(), iq.size(), out);
        if (!ch.take_level(mn, av, mx)) { std::printf("FAIL first read\n"); fails++; }
        near(av, -6.0f, 0.5f, "first read avg");
        if (ch.take_level(mn, av, mx)) { std::printf("FAIL second read had data\n"); fails++; }

        // A quieter burst reports its own level, not the louder earlier one.
        for (int k = 0; k < fs; k++) {
            double ph = 2 * PI * 1000.0 * k / fs;
            iq[k] = std::complex<float>(0.05f * std::cos(ph), 0.05f * std::sin(ph));
        }
        out.clear();
        ch.process(iq.data(), iq.size(), out);
        if (!ch.take_level(mn, av, mx)) { std::printf("FAIL third read\n"); fails++; }
        near(av, -26.0f, 0.5f, "after reset avg");
    }

    // Without enable_level there is nothing to report.
    {
        ChannelChain ch(0.0, 96000, 12000, Mode::USB, Format::I16);
        std::vector<std::complex<float>> iq(96000, std::complex<float>(0.5f, 0.0f));
        std::vector<uint8_t> out;
        ch.process(iq.data(), iq.size(), out);
        if (ch.take_level(mn, av, mx)) { std::printf("FAIL level reported while off\n"); fails++; }
    }

    std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
