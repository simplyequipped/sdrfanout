#include "synth.h"
#include "util.h"
#include <cmath>
#include <ctime>

SynthCapture::SynthCapture(double rate, double center, std::vector<double> dials,
                           long total, bool paced)
    : rate_(rate), center_(center), dials_(std::move(dials)),
      total_(total), n_(0), paced_(paced) {
    t0_ = now();
}

int SynthCapture::read(std::complex<float> *buf, int maxn, double &t_recv) {
    if (total_ > 0 && n_ >= total_) return -1;          // finite stream ended
    int n = maxn;
    int cap = (int)(rate_ / 20);                        // ~50 ms chunks: stream smoothly
    if (n > cap) n = cap;                               // (a real SDR delivers small chunks too)
    if (total_ > 0 && n_ + n > total_) n = (int)(total_ - n_);

    if (paced_) {                                        // crude real-time pacing
        double target = t0_ + (double)(n_ + n) / rate_;
        double wait = target - now();
        if (wait > 0) {
            struct timespec ts{(time_t)wait, (long)((wait - (time_t)wait) * 1e9)};
            nanosleep(&ts, 0);
        }
    }

    for (int i = 0; i < n; i++) {
        long idx = n_ + i;
        std::complex<float> s(0, 0);
        for (double d : dials_) {
            double f = (d - center_) + 1500.0;           // USB tone 1500 Hz above the dial
            double ph = 2 * M_PI * f * idx / rate_;
            s += std::complex<float>(0.3f * cosf(ph), 0.3f * sinf(ph));
        }
        buf[i] = s;
    }
    n_ += n;
    t_recv = paced_ ? now() : (t0_ + (double)n_ / rate_);
    return n;
}

Capture *make_synth(const CaptureConfig &c, std::string &err) {
    double center = resolve_center(c);
    double rate = resolve_rate(c, center, {});           // synth: any rate
    if (rate <= 0) { err = "synth: could not resolve sample rate"; return 0; }
    if (!rate_ok(rate)) {
        err = "rate " + std::to_string((long)rate) + " is not an integer multiple of 12000";
        return 0;
    }
    return new SynthCapture(rate, center, c.dials, 0, true);   // infinite, paced
}
