#include "dsp.h"
#include <cmath>
#include <cstddef>

static const double PI = 3.14159265358979323846;

// Hamming-windowed sinc low-pass, cutoff fc (cycles/sample), odd length n. Unity DC gain.
static std::vector<float> design_lowpass(double fc, int n) {
    std::vector<float> h(n);
    int m = n / 2;
    double sum = 0;
    for (int i = 0; i < n; i++) {
        int k = i - m;
        double s = (k == 0) ? 2 * fc : std::sin(2 * PI * fc * k) / (PI * k);
        double w = 0.54 - 0.46 * std::cos(2 * PI * i / (n - 1));
        h[i] = (float)(s * w);
        sum += h[i];
    }
    for (auto &x : h) x /= (float)sum;
    return h;
}

// Hamming-windowed Hilbert FIR, odd length n. Anti-symmetric, group delay (n-1)/2.
static std::vector<float> design_hilbert(int n) {
    std::vector<float> h(n, 0.0f);
    int m = n / 2;
    for (int i = 0; i < n; i++) {
        int k = i - m;
        if (k & 1) {  // nonzero on odd taps only
            double w = 0.54 - 0.46 * std::cos(2 * PI * i / (n - 1));
            h[i] = (float)((2.0 / (PI * k)) * w);
        }
    }
    return h;
}

ChannelChain::ChannelChain(double delta_hz, int fs) {
    double w = -2.0 * PI * delta_hz / fs;   // mix the channel down to baseband
    rot_ = std::complex<double>(std::cos(w), std::sin(w));
    osc_ = std::complex<double>(1.0, 0.0);
    osc_count_ = 0;

    D_ = fs / OUT_RATE;               // integer by contract (sdrfanout enforces the rate rule)
    dec_count_ = 0;
    int lp_len = 8 * D_ + 1;          // taps scale with D for a sharp-enough transition near 6 kHz
    if (lp_len > 1023) lp_len = 1023;
    lp_ = design_lowpass(5400.0 / fs, lp_len);
    lp_hist_.assign(2 * lp_.size(), std::complex<float>(0, 0));  // double-length: see process()
    lp_pos_ = 0;

    int hl = 65;
    hil_ = design_hilbert(hl);
    hdelay_ = hl / 2;
    i_hist_.assign(hl, 0.0f);
    q_hist_.assign(hl, 0.0f);
    h_pos_ = 0;
}

void ChannelChain::process(const std::complex<float> *iq, size_t n, std::vector<int16_t> &out) {
    const size_t L = lp_.size();
    const size_t H = hil_.size();

    for (size_t k = 0; k < n; k++) {
        // 1) down-convert by Δf (recursive phasor; renormalize to hold |osc_| = 1)
        std::complex<float> x = iq[k] * std::complex<float>((float)osc_.real(),
                                                            (float)osc_.imag());
        osc_ *= rot_;
        if (++osc_count_ >= 512) {
            osc_count_ = 0;
            osc_ /= std::abs(osc_);
        }

        // 2) anti-alias FIR + decimate by D. History is double-length: each sample is
        //    stored at j and j+L, so the convolution reads L contiguous samples with no
        //    per-tap modulo (faster, and the inner loop vectorizes cleanly).
        lp_hist_[lp_pos_] = x;
        lp_hist_[lp_pos_ + L] = x;
        if (++lp_pos_ >= L) lp_pos_ -= L;
        if (++dec_count_ < D_) continue;
        dec_count_ = 0;

        std::complex<float> acc(0, 0);
        const std::complex<float> *h = &lp_hist_[lp_pos_];   // oldest sample, L contiguous
        for (size_t t = 0; t < L; t++)        // symmetric LPF: history order is irrelevant
            acc += h[t] * lp_[t];
        float I = acc.real(), Q = acc.imag();

        // 3) USB demod: usb = I(delayed by group delay) − Hilbert{Q}
        i_hist_[h_pos_] = I;
        q_hist_[h_pos_] = Q;
        float hq = 0.0f;
        size_t xi = h_pos_;                   // newest sample = x[n]
        for (size_t t = 0; t < H; t++) {      // standard convolution: hil_[0] · newest
            hq += hil_[t] * q_hist_[xi];
            xi = (xi + H - 1) % H;            // step backward in time
        }
        size_t di = (h_pos_ + H - (size_t)hdelay_) % H;  // I delayed by group delay
        float usb = i_hist_[di] - hq;
        h_pos_ = (h_pos_ + 1) % H;

        // 4) to int16 with headroom
        int v = (int)std::lrint(usb * 16000.0f);
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        out.push_back((int16_t)v);
    }
}
