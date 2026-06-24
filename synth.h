// synth: a synthetic Capture that emits IQ carrying a USB tone (+1500 Hz) at each
// channel's dial. A signal generator for the unit tests and `-driver synth`
// smoke-checks. paced=true sleeps to ~real-time (for `-driver synth`). paced=false
// runs flat-out and ends after `total` samples (tests).
#pragma once
#include "capture.h"

class SynthCapture : public Capture {
public:
    SynthCapture(double rate, double center, std::vector<double> dials,
                 long total, bool paced);
    double rate() const override { return rate_; }
    double center() const override { return center_; }
    int read(std::complex<float> *buf, int maxn, double &t_recv) override;

private:
    double rate_, center_, t0_;
    std::vector<double> dials_;
    long total_, n_;
    bool paced_;
};
