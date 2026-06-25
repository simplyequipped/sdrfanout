// Validation of the per-channel DSP chain.
// Feeds synthetic IQ tones and checks: USB-side tone passes, LSB-side tone is
// rejected, and the audio lands at the right frequency.
#include "dsp.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <complex>
#include <vector>

static const double PI = 3.14159265358979323846;

// amplitude of out[] (int16 @12k) at f_hz, via single-bin DFT
static double tone_amp(const std::vector<int16_t> &out, double f_hz, int fs = 12000) {
    double re = 0, im = 0;
    for (size_t n = 0; n < out.size(); n++) {
        double a = out[n] / 32768.0;
        re += a * std::cos(2 * PI * f_hz * n / fs);
        im += a * std::sin(2 * PI * f_hz * n / fs);
    }
    return 2.0 * std::sqrt(re * re + im * im) / out.size();
}

// Run a complex tone at (delta + tone_off) Hz through the chain; return USB audio.
static std::vector<int16_t> run(int fs, double delta, double tone_off) {
    ChannelChain ch(delta, fs);
    std::vector<std::complex<float>> iq(fs);  // 1 second of IQ
    double f = delta + tone_off;
    for (int n = 0; n < fs; n++) {
        double ph = 2 * PI * f * n / fs;
        iq[n] = std::complex<float>(0.5f * std::cos(ph), 0.5f * std::sin(ph));
    }
    std::vector<int16_t> out;
    int blk = fs / 10;                         // 0.1 s blocks, to exercise streaming state
    for (int b = 0; b + blk <= fs; b += blk) ch.process(&iq[b], blk, out);
    return out;
}

int main() {
    int fs = 96000;          // 8 × 12000
    double delta = 9000.0;   // channel offset from SDR center

    auto usb = run(fs, delta, +1500.0);  // USB side → expect audio at 1500 Hz
    auto lsb = run(fs, delta, -1500.0);  // LSB side → expect rejected

    double a_usb = tone_amp(usb, 1500.0);
    double a_lsb = tone_amp(lsb, 1500.0);
    double rej_db = 20 * std::log10((a_lsb + 1e-9) / (a_usb + 1e-9));

    std::printf("output samples: %zu (expect %d)\n", usb.size(), fs / 10 * 10 / 8);
    std::printf("USB tone (+1500): |A| @1500Hz = %.3f\n", a_usb);
    std::printf("LSB tone (-1500): |A| @1500Hz = %.3f\n", a_lsb);
    std::printf("sideband rejection: %.1f dB\n", rej_db);

    bool ok = a_usb > 0.3 && rej_db < -25.0;
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
