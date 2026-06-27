#include "capture.h"
#include "util.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>
#include <sys/time.h>

double now() {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static const double CHAN_HALF = 6000.0;   // a channel's half-width (OUT_RATE/2), the anti-alias floor

double resolve_center(const CaptureConfig &c) {
    if (c.center_mode == CenterMode::Fixed) return c.want_center;
    std::vector<double> d = c.dials;
    std::sort(d.begin(), d.end());
    double mn = d.front(), mx = d.back();
    if (c.center_mode == CenterMode::Edge)
        return mn - c.guard;   // LO below all channels: every channel at a positive offset

    // Auto: minimize the farthest channel offset (and so the sample rate) while
    // keeping every channel at least `guard` from the LO, so the DC spike never
    // lands in a channel. The unconstrained optimum is the cluster midpoint. If a
    // channel sits within `guard` of it, slide the LO into the best gap between two
    // adjacent channels that is wide enough; if none is, fall back to the one-sided
    // layout (LO just below all channels).
    double mid = 0.5 * (mn + mx);
    double best = mn - c.guard;                  // one-sided fallback
    double best_off = mx - best;                 // = span + guard
    for (size_t i = 0; i + 1 < d.size(); i++) {
        if (d[i + 1] - d[i] < 2 * c.guard) continue;   // no room for the LO in this gap
        double lo = d[i] + c.guard, hi = d[i + 1] - c.guard;
        double cand = std::min(std::max(mid, lo), hi);  // midpoint clamped into the gap
        double off = std::max(mx - cand, cand - mn);
        if (off < best_off) { best_off = off; best = cand; }
    }
    return best;
}

double max_offset(const CaptureConfig &c, double center) {
    double m = 0;
    for (double d : c.dials) m = std::max(m, std::fabs(d - center));
    return m;
}

int auto_threads(int channels, int cores) {
    if (channels < 1) return 1;
    if (cores < 1) cores = 1;
    if (cores > channels) cores = channels;          // never more workers than channels
    int best_max = (channels + cores - 1) / cores;   // ceil: minimal busiest-worker load
    return (channels + best_max - 1) / best_max;      // smallest thread count that reaches it
}

std::string normalize_driver(const std::string &name) {
    return name.empty() ? name : "driver=" + name;   // "hackrf" -> "driver=hackrf"
}

bool rate_ok(double rate) {
    if (rate <= 0) return false;
    long n = (long)std::lround(rate / 12000.0);
    return n >= 1 && std::fabs(rate - (double)n * 12000.0) < 1.0;
}

double resolve_rate(const CaptureConfig &c, double center, const std::vector<double> &ranges) {
    const int OUT = 12000;
    if (c.want_rate > 0) return c.want_rate;   // explicit override (trust the operator)

    // rate/2 must reach the farthest channel plus `guard` clearance to the band edge.
    // The channel half-width (6 kHz) is the floor so a small guard still cannot alias.
    double edge = std::max(c.guard, CHAN_HALF);
    double lo = 2.0 * (max_offset(c, center) + edge);

    int n = (int)std::ceil(lo / OUT);
    if (n < 1) n = 1;
    for (; n < 1000000; n++) {
        double r = (double)n * OUT;
        if (ranges.empty()) return r;                    // any rate ok (synth)
        for (size_t i = 0; i + 1 < ranges.size(); i += 2)
            if (r >= ranges[i] - 1 && r <= ranges[i + 1] + 1) return r;
    }
    return 0;   // no integer-x12k rate the device supports spans the channels
}

std::string check_channels(const CaptureConfig &c, double center, double rate) {
    const double half = rate / 2.0;
    for (double d : c.dials) {                       // hard error: passband over Nyquist
        double off = std::fabs(d - center);
        if (off + CHAN_HALF > half) {
            char buf[192];
            std::snprintf(buf, sizeof buf,
                "channel %.0f Hz aliases at rate %.0f Hz (offset %.0f from center needs "
                "rate >= %.0f). raise -rate or use -center auto",
                d, rate, off, 2.0 * (off + CHAN_HALF));
            return buf;
        }
    }
    for (double d : c.dials) {                       // soft warnings
        double off = std::fabs(d - center);
        if (off > half - c.guard)
            std::fprintf(stderr, "sdrfanout: warning: channel %.0f Hz is within guard "
                "(%.0f Hz) of the band edge, the SDR roll-off may attenuate it\n",
                d, c.guard);
        if (off < c.guard)
            std::fprintf(stderr, "sdrfanout: warning: channel %.0f Hz is within guard "
                "(%.0f Hz) of the LO, the DC spike may land in it\n", d, c.guard);
    }
    return "";
}
