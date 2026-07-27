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

ChannelChain::ChannelChain(double delta_hz, int fs, int out_rate, Mode mode, Format format) {
    double w = -2.0 * PI * delta_hz / fs;   // mix the channel down to baseband
    rot_ = std::complex<double>(std::cos(w), std::sin(w));
    osc_ = std::complex<double>(1.0, 0.0);
    osc_count_ = 0;

    mode_ = mode;
    format_ = format;
    demod_sign_ = (mode_ == Mode::LSB) ? 1.0f : -1.0f;   // usb = I - H{Q}, lsb = I + H{Q}

    out_rate_ = out_rate;
    D_ = fs / out_rate_;              // integer by contract (sdrfanout enforces the rate rule)
    dec_count_ = 0;
    int lp_len = 8 * D_ + 1;          // taps scale with D for a sharp-enough transition near the edge
    if (lp_len > 1023) lp_len = 1023;
    lp_ = design_lowpass(out_rate_ * 0.45 / fs, lp_len);
    lp_hist_.assign(2 * lp_.size(), std::complex<float>(0, 0));  // double-length: see process()
    lp_pos_ = 0;

    int hl = 65;
    hil_ = design_hilbert(hl);
    hdelay_ = hl / 2;
    i_hist_.assign(hl, 0.0f);
    q_hist_.assign(hl, 0.0f);
    h_pos_ = 0;

    level_on_ = false;
    sub_len_ = 0;
    level_reset();
}

void ChannelChain::enable_level() {
    level_on_ = true;
    sub_len_ = out_rate_ / 10;        // 0.1 s of output
    if (sub_len_ < 1) sub_len_ = 1;
    level_reset();
}

void ChannelChain::level_reset() {
    sub_sumsq_ = 0;
    sub_n_ = 0;
    lvl_min_ = 0;
    lvl_max_ = 0;
    lvl_sum_ = 0;
    lvl_blocks_ = 0;
}

bool ChannelChain::take_level(float &min_db, float &avg_db, float &max_db) {
    if (lvl_blocks_ < 1) return false;
    // Floor the argument so a digitally silent channel reports a finite level
    // instead of -inf.
    auto dbfs = [](double rms) {
        if (rms < 1e-10) rms = 1e-10;
        return (float)(20.0 * std::log10(rms));
    };
    min_db = dbfs(lvl_min_);
    max_db = dbfs(lvl_max_);
    avg_db = dbfs(lvl_sum_ / (double)lvl_blocks_);
    level_reset();
    return true;
}

void ChannelChain::put(std::vector<uint8_t> &out, float v) const {
    if (format_ == Format::I16) {
        int x = (int)std::lrint(v * 16000.0f);   // scale with headroom, then clamp
        if (x > 32767) x = 32767;
        else if (x < -32768) x = -32768;
        int16_t s = (int16_t)x;
        const uint8_t *p = reinterpret_cast<const uint8_t *>(&s);
        out.insert(out.end(), p, p + sizeof s);
    } else {
        const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);   // raw float, ~[-1,1]
        out.insert(out.end(), p, p + sizeof v);
    }
}

void ChannelChain::process(const std::complex<float> *iq, size_t n, std::vector<uint8_t> &out) {
    const size_t L = lp_.size();
    const size_t H = hil_.size();

    for (size_t k = 0; k < n; k++) {
        // 1) down-convert by delta_f (recursive phasor, renormalize to hold |osc_| = 1)
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

        // 2b) signal level, taken here so every mode measures the same thing
        if (level_on_) {
            sub_sumsq_ += (double)I * I + (double)Q * Q;
            if (++sub_n_ >= sub_len_) {
                float rms = (float)std::sqrt(sub_sumsq_ / (double)sub_n_);
                if (lvl_blocks_ == 0 || rms < lvl_min_) lvl_min_ = rms;
                if (rms > lvl_max_) lvl_max_ = rms;
                lvl_sum_ += rms;
                lvl_blocks_++;
                sub_sumsq_ = 0;
                sub_n_ = 0;
            }
        }

        if (mode_ == Mode::IQ) {              // ship complex baseband, no demod
            put(out, I);
            put(out, Q);
            continue;
        }

        // 3) SSB demod: ssb = I(delayed by group delay) +/- Hilbert{Q} (sign per mode)
        i_hist_[h_pos_] = I;
        q_hist_[h_pos_] = Q;
        float hq = 0.0f;
        size_t xi = h_pos_;                   // newest sample = x[n]
        for (size_t t = 0; t < H; t++) {      // standard convolution: hil_[0] * newest
            hq += hil_[t] * q_hist_[xi];
            xi = (xi + H - 1) % H;            // step backward in time
        }
        size_t di = (h_pos_ + H - (size_t)hdelay_) % H;  // I delayed by group delay
        float ssb = i_hist_[di] + demod_sign_ * hq;
        h_pos_ = (h_pos_ + 1) % H;
        put(out, ssb);
    }
}
